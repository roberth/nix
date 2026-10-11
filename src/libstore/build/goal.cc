#include "nix/store/build/goal.hh"
#include "nix/store/build/worker.hh"
#include "nix/store/worker-settings.hh"

#include <algorithm>

namespace nix {

namespace {

/**
 * Wakes a goal through the queue of its `Worker`, from any thread.
 */
struct GoalWakeTarget : WakeTarget
{
    std::weak_ptr<Worker::WakeupQueue> queue;
    WeakGoalPtr goal;

    GoalWakeTarget(std::weak_ptr<Worker::WakeupQueue> queue, WeakGoalPtr goal)
        : queue(std::move(queue))
        , goal(std::move(goal))
    {
    }

    void wake() noexcept override
    {
        /* The Worker might have died, and the queue with it. */
        if (auto q = queue.lock())
            q->enqueue(goal);
    }
};

} // namespace

struct Goal::WaiteeSet
{
    Goal & waiter;
    Goals waitees;
    Waker waker;
};

Goal::Goal(Worker & worker, Task<ExitCode> init)
    : task(std::move(init))
    , worker(worker)
{
}

Goal::~Goal()
{
    trace("goal destroyed");
    if (suspendedElsewhere)
        --worker.nrGoalsSuspendedElsewhere;
}

void WorkerSettings::anchor() {}

TimedOut::TimedOut(time_t maxDuration)
    : CloneableError(BuildResult::Failure::TimedOut, "timed out after %1% seconds", maxDuration)
    , maxDuration(maxDuration)
{
}

void TimedOut::anchor() {}

void Goal::anchor() {}

void Goal::ChildEvents::pushChildEvent(ChildOutput event)
{
    if (childTimeout)
        return; // Already timed out, ignore
    childOutputs.push(std::move(event));
    waker.wakeByRef();
}

void Goal::ChildEvents::pushChildEvent(ChildEOF event)
{
    if (childTimeout)
        return; // Already timed out, ignore
    assert(!childEOF);
    childEOF = std::move(event);
    waker.wakeByRef();
}

void Goal::ChildEvents::pushChildEvent(TimedOut event)
{
    // Timeout is immediate - flush pending events
    childOutputs = {};
    childEOF.reset();
    childTimeout = std::make_unique<TimedOut>(std::move(event));
    waker.wakeByRef();
}

bool Goal::ChildEvents::hasChildEvent() const
{
    return !childOutputs.empty() || childEOF || childTimeout;
}

Goal::ChildEvent Goal::ChildEvents::popChildEvent()
{
    if (!childOutputs.empty()) {
        auto event = std::move(childOutputs.front());
        childOutputs.pop();
        return event;
    }
    if (childEOF)
        return *std::exchange(childEOF, std::nullopt);
    if (childTimeout)
        return std::exchange(childTimeout, nullptr);
    unreachable();
}

WaitQueue::Entry::~Entry()
{
    if (queue)
        queue->entries.erase(pos);
}

void WaitQueue::Entry::wait(WaitQueue & q, const Waker & w)
{
    if (!waker.willWake(w))
        waker = w;
    if (!queue) {
        queue = &q;
        pos = q.entries.insert(q.entries.end(), this);
    }
}

WaitQueue::~WaitQueue()
{
    /* The entries live in the frames of tasks, which can outlive the
       queue. */
    for (auto * e : entries)
        e->queue = nullptr;
}

bool WaitQueue::wakeOne()
{
    if (entries.empty())
        return false;
    auto * e = entries.front();
    entries.pop_front();
    e->queue = nullptr;
    e->woken = true;
    e->waker.wakeByRef();
    return true;
}

void WaitQueue::wakeAll()
{
    while (wakeOne())
        ;
}

bool CompareGoalPtrs::operator()(const GoalPtr & a, const GoalPtr & b) const
{
    return a->keyCached() < b->keyCached();
}

void addToWeakGoals(WeakGoals & goals, GoalPtr p)
{
    if (goals.find(p) != goals.end())
        return;
    goals.insert(p);
}

namespace {

/**
 * Base class of the leaves that the `Worker` wakes. A goal that suspends
 * on one of them can only continue through the `Worker`, which lets the
 * `Worker` tell a deadlock from a wait for something outside of it.
 */
struct WorkerLeaf : LeafAwaiter
{
    bool & suspendedOnWorker;

    explicit WorkerLeaf(bool & suspendedOnWorker)
        : suspendedOnWorker(suspendedOnWorker)
    {
    }

    nix_poll pollLeaf(const Waker & waker) final
    {
        auto res = pollWorker(waker);
        if (res == NIX_POLL_PENDING)
            suspendedOnWorker = true;
        return res;
    }

    virtual nix_poll pollWorker(const Waker & waker) = 0;
};

/**
 * Suspend once and wake immediately, so that the `Worker` polls the other
 * goals that are awake first.
 */
struct YieldLeaf : WorkerLeaf
{
    bool yielded = false;

    using WorkerLeaf::WorkerLeaf;

    nix_poll pollWorker(const Waker & waker) override
    {
        if (yielded)
            return NIX_POLL_READY;
        yielded = true;
        waker.wakeByRef();
        return NIX_POLL_PENDING;
    }

    void await_resume() noexcept {}
};

/**
 * Wait in a `WaitQueue` of the `Worker`. If `ready` is true, yield
 * instead of waiting.
 */
struct WaitQueueLeaf : WorkerLeaf
{
    WaitQueue & queue;
    bool ready;
    bool yielded = false;
    WaitQueue::Entry entry;

    WaitQueueLeaf(bool & suspendedOnWorker, WaitQueue & queue, bool ready)
        : WorkerLeaf(suspendedOnWorker)
        , queue(queue)
        , ready(ready)
    {
    }

    nix_poll pollWorker(const Waker & waker) override
    {
        if (ready) {
            if (yielded)
                return NIX_POLL_READY;
            yielded = true;
            waker.wakeByRef();
            return NIX_POLL_PENDING;
        }
        if (entry.isWoken())
            return NIX_POLL_READY;
        entry.wait(queue, waker);
        return NIX_POLL_PENDING;
    }

    void await_resume() noexcept {}
};

} // namespace

void Goal::removeWaiter(const WaiteeSet * set)
{
    std::erase_if(waiters, [&](const std::weak_ptr<WaiteeSet> & w) {
        auto s = w.lock();
        return !s || s.get() == set;
    });
}

Goal::Co<void> Goal::await(Goals newWaitees)
{
    if (newWaitees.empty())
        co_return;

    struct WaiteesLeaf : WorkerLeaf
    {
        std::shared_ptr<WaiteeSet> set;

        WaiteesLeaf(Goal & goal, Goals waitees)
            : WorkerLeaf(goal.suspendedOnWorker)
            , set(std::make_shared<WaiteeSet>(goal, std::move(waitees)))
        {
            for (auto & waitee : set->waitees)
                waitee->waiters.push_back(set);
        }

        WaiteesLeaf(const WaiteesLeaf &) = delete;

        ~WaiteesLeaf()
        {
            /* Unregister before releasing the waitees: releasing the last
               reference cancels a waitee. */
            for (auto & waitee : set->waitees)
                waitee->removeWaiter(set.get());
        }

        nix_poll pollWorker(const Waker & waker) override
        {
            if (set->waitees.empty())
                return NIX_POLL_READY;
            if (!set->waker.willWake(waker))
                set->waker = waker;
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    co_await WaiteesLeaf(*this, std::move(newWaitees));
}

void Goal::trace(std::string_view s)
{
    debug("%1%: %2%", name, s);
}

void Goal::work()
{
    /* A waker can wake the goal after its task completed, for example
       when the goal woke twice before it ran. */
    if (!task)
        return;

    if (!waker)
        waker = makeWakeTarget<GoalWakeTarget>(worker.getCrossThreadWaker(), weak_from_this())->waker();

    if (suspendedElsewhere) {
        suspendedElsewhere = false;
        --worker.nrGoalsSuspendedElsewhere;
    }
    suspendedOnWorker = false;

    if (task.poll(waker) == NIX_POLL_PENDING) {
        if (!suspendedOnWorker) {
            suspendedElsewhere = true;
            ++worker.nrGoalsSuspendedElsewhere;
        }
        return;
    }

    trace("done");

    /* An exception escapes to the caller of `Worker::run`, as before the
       goals were tasks. */
    auto result = task.takeResult();
    task.reset();
    finish(result);
}

void Goal::finish(ExitCode result)
{
    assert(!exitCode);
    assert(result == ecSuccess || result == ecFailed || result == ecNoSubstituters);
    exitCode = result;

    auto me = shared_from_this();

    /* Take the list, so that the cancellations below cannot change it
       while we iterate. */
    auto waiters2 = std::exchange(waiters, {});

    // Log the failure if we have one and shouldn't preserve it.
    // Only log for actual failures (ecFailed), not for ecNoSubstituters
    // which indicates "couldn't substitute, will try building" - that's
    // expected behavior, not an error.
    if (result == ecFailed) {
        if (auto * failure = buildResult.tryGetFailure()) {
            bool haveWaiters = std::ranges::any_of(waiters2, [](auto & w) { return !w.expired(); });
            if (!preserveFailure && haveWaiters)
                logError(failure->info());
        }
    }

    for (auto & w : waiters2) {
        /* A cancellation in an earlier iteration can have destroyed the
           waiter. */
        auto set = w.lock();
        if (!set || !set->waitees.erase(me))
            continue;

        Goal & goal = set->waiter;

        goal.trace(fmt("waitee '%s' done; %d left", name, set->waitees.size()));

        if (result == ecFailed || result == ecNoSubstituters)
            ++goal.nrFailed;

        if (result == ecNoSubstituters)
            ++goal.nrNoSubstituters;

        if (set->waitees.empty()) {
            set->waker.wakeByRef();
        } else if (result == ecFailed && !worker.settings.keepGoing) {
            /* If we failed and keepGoing is not set, we stop waiting for
               the remaining waitees. Releasing them cancels those that
               nothing else holds. */
            auto rest = std::exchange(set->waitees, {});
            for (auto & g : rest)
                g->removeWaiter(set.get());
            set->waker.wakeByRef();
            rest.clear();
        }
    }

    worker.removeGoal(me);

    cleanup();
}

void Goal::handleChildOutput(Descriptor fd, std::string_view data)
{
    childEvents.pushChildEvent(ChildOutput{fd, std::string{data}});
}

void Goal::handleEOF(Descriptor fd)
{
    childEvents.pushChildEvent(ChildEOF{fd});
}

void Goal::timedOut(TimedOut && ex)
{
    childEvents.pushChildEvent(std::move(ex));
}

Goal::Co<void> Goal::yield()
{
    co_await YieldLeaf(suspendedOnWorker);
}

Goal::Co<void> Goal::waitForAWhile()
{
    trace("wait for a while");
    co_await WaitQueueLeaf(suspendedOnWorker, worker.waitingForAWhile, false);
}

Goal::Co<void> Goal::waitForBuildSlot()
{
    trace("wait for build slot");
    auto category = jobCategory();
    co_await WaitQueueLeaf(
        suspendedOnWorker,
        category == JobCategory::Substitution ? worker.wantingToSubstitute : worker.wantingToBuild,
        worker.slotAvailable(category));
}

Goal::Co<Goal::ChildEvent> Goal::waitForChildEvent()
{
    struct ChildEventLeaf : WorkerLeaf
    {
        ChildEvents & events;

        ChildEventLeaf(Goal & goal)
            : WorkerLeaf(goal.suspendedOnWorker)
            , events(goal.childEvents)
        {
        }

        ~ChildEventLeaf()
        {
            events.waker.reset();
        }

        nix_poll pollWorker(const Waker & waker) override
        {
            if (events.hasChildEvent())
                return NIX_POLL_READY;
            if (!events.waker.willWake(waker))
                events.waker = waker;
            return NIX_POLL_PENDING;
        }

        ChildEvent await_resume()
        {
            return events.popChildEvent();
        }
    };

    co_return co_await ChildEventLeaf(*this);
}

} // namespace nix

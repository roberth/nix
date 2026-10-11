#include "nix/store/local-store.hh"
#include "nix/store/machines.hh"
#include "nix/store/store-open.hh"
#include "nix/store/build/worker.hh"
#include "nix/store/build/substitution-goal.hh"
#include "nix/store/build/drv-output-substitution-goal.hh"
#include "nix/store/build/derivation-goal.hh"
#include "nix/store/build/derivation-resolution-goal.hh"
#include "nix/store/build/derivation-building-goal.hh"
#include "nix/store/build/derivation-trampoline-goal.hh"
#ifndef _WIN32 // TODO Enable building on Windows
#  include "nix/store/build/hook-instance.hh"
#endif
#include "nix/util/signals.hh"
#include "nix/util/reactor.hh"
#include "nix/util/task-thread.hh"
#include "nix/util/environment-variables.hh"
#include "nix/store/globals.hh"

namespace nix {

Worker::Worker(Store & store, Store & evalStore)
    /* Can't use make_ref, because the constructor is private. */
    : wakerState(ref<WakeupQueue>(new WakeupQueue{}))
    , act(*logger, actRealise)
    , actDerivations(*logger, actBuilds)
    , actSubstitutions(*logger, actCopyPaths)
#ifdef _WIN32
    , ioport{CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)}
#endif
    , store(store)
    , evalStore(evalStore)
    , settings(nix::settings.getWorkerSettings())
    , getSubstituters{[] {
        return nix::settings.getWorkerSettings().useSubstitutes ? getDefaultSubstituters() : std::list<ref<Store>>{};
    }}
    , blockingPool(settings.maxBuildJobs)
    , builderStartPool(1)
{
#ifdef _WIN32
    if (!ioport)
        throw windows::WinError("CreateIoCompletionPort");
#endif
    nrLocalBuilds = 0;
    nrSubstitutions = 0;
}

Worker::~Worker()
{
    /* Explicitly get rid of all strong pointers now.  After this all
       goals that refer to this worker should be gone.  (Otherwise we
       are in trouble, since goals may call childTerminated() etc. in
       their destructors). */
    topGoals.clear();

    if (goalPollStats.polls && getEnv("_NIX_REACTOR_STATS")) {
        auto & s = goalPollStats;
        /* A measurement aid, like the statistics of the reactor: straight
           to stderr, past the logger. */
        writeToStderr(
            fmt("nix: goals: %d polls, busy %d ms, longest poll %d us (%s), polls of at least 1/10/50 ms: %d/%d/%d\n",
                s.polls,
                s.busy.count() / 1000,
                s.longest.count(),
                s.longestGoal,
                s.atLeast[0],
                s.atLeast[1],
                s.atLeast[2]));
    }

    assert(expectedSubstitutions == 0);
    assert(expectedDownloadSize == 0);
    assert(expectedNarSize == 0);
}

template<class G, typename... Args>
std::shared_ptr<G> Worker::initGoalIfNeeded(std::weak_ptr<G> & goal_weak, Args &&... args)
{
    if (auto goal = goal_weak.lock())
        return goal;

    auto goal = std::make_shared<G>(std::forward<Args>(args)...);
    goal_weak = goal;
    wakeUp(goal);
    return goal;
}

std::shared_ptr<DerivationTrampolineGoal> Worker::makeDerivationTrampolineGoal(
    ref<const SingleDerivedPath> drvReq, const OutputsSpec & wantedOutputs, BuildMode buildMode)
{
    return initGoalIfNeeded(
        derivationTrampolineGoals.ensureSlot(*drvReq).value[wantedOutputs], drvReq, wantedOutputs, *this, buildMode);
}

std::shared_ptr<DerivationTrampolineGoal> Worker::makeDerivationTrampolineGoal(
    const StorePath & drvPath, const OutputsSpec & wantedOutputs, const Derivation & drv, BuildMode buildMode)
{
    return initGoalIfNeeded(
        derivationTrampolineGoals.ensureSlot(DerivedPath::Opaque{drvPath}).value[wantedOutputs],
        drvPath,
        wantedOutputs,
        drv,
        *this,
        buildMode);
}

std::shared_ptr<DerivationGoal> Worker::makeDerivationGoal(
    const StorePath & drvPath,
    ref<const Derivation> drv,
    const OutputName & wantedOutput,
    BuildMode buildMode,
    bool storeDerivation)
{
    return initGoalIfNeeded(
        derivationGoals[drvPath][wantedOutput],
        drvPath,
        std::move(drv),
        wantedOutput,
        *this,
        buildMode,
        storeDerivation);
}

std::shared_ptr<DerivationResolutionGoal>
Worker::makeDerivationResolutionGoal(const StorePath & drvPath, ref<const Derivation> drv, BuildMode buildMode)
{
    return initGoalIfNeeded(derivationResolutionGoals[drvPath], drvPath, drv, *this, buildMode);
}

std::shared_ptr<DerivationBuildingGoal>
Worker::makeDerivationBuildingGoal(const StorePath & drvPath, ref<const BasicDerivation> drv, BuildMode buildMode)
{
    return initGoalIfNeeded(derivationBuildingGoals[drvPath], drvPath, std::move(drv), *this, buildMode);
}

std::shared_ptr<PathSubstitutionGoal>
Worker::makePathSubstitutionGoal(const StorePath & path, RepairFlag repair, std::optional<ContentAddress> ca)
{
    return initGoalIfNeeded(substitutionGoals[path], path, *this, repair, ca);
}

std::shared_ptr<DrvOutputSubstitutionGoal> Worker::makeDrvOutputSubstitutionGoal(const DrvOutput & id)
{
    return initGoalIfNeeded(drvOutputSubstitutionGoals[id], id, *this);
}

GoalPtr Worker::makeGoal(const DerivedPath & req, BuildMode buildMode)
{
    return std::visit(
        overloaded{
            [&](const DerivedPath::Built & bfd) -> GoalPtr {
                return makeDerivationTrampolineGoal(bfd.drvPath, bfd.outputs, buildMode);
            },
            [&](const DerivedPath::Opaque & bo) -> GoalPtr {
                return makePathSubstitutionGoal(bo.path, buildMode == bmRepair ? Repair : NoRepair);
            },
        },
        req.raw());
}

void Worker::removeGoal(GoalPtr goal)
{
    if (auto drvGoal = std::dynamic_pointer_cast<DerivationTrampolineGoal>(goal)) {
        derivationTrampolineGoals.removeSlot(*drvGoal->drvReq, [&](auto & node) {
            node.value.erase(drvGoal->wantedOutputs);
            /* Return true if ancestors don't need to be pruned. */
            return !node.value.empty();
        });
    } else if (auto drvGoal = std::dynamic_pointer_cast<DerivationGoal>(goal)) {
        if (auto it = derivationGoals.find(drvGoal->drvPath); it != derivationGoals.end()) {
            it->second.erase(drvGoal->wantedOutput);
            if (it->second.empty())
                derivationGoals.erase(it);
        }
    } else if (auto drvResolutionGoal = std::dynamic_pointer_cast<DerivationResolutionGoal>(goal)) {
        derivationResolutionGoals.erase(drvResolutionGoal->drvPath);
    } else if (auto drvBuildingGoal = std::dynamic_pointer_cast<DerivationBuildingGoal>(goal)) {
        derivationBuildingGoals.erase(drvBuildingGoal->drvPath);
    } else if (auto subGoal = std::dynamic_pointer_cast<PathSubstitutionGoal>(goal)) {
        substitutionGoals.erase(subGoal->getStorePath());
    } else if (auto subGoal = std::dynamic_pointer_cast<DrvOutputSubstitutionGoal>(goal)) {
        drvOutputSubstitutionGoals.erase(subGoal->id);
    } else {
        unreachable();
    }

    if (topGoals.find(goal) != topGoals.end()) {
        topGoals.erase(goal);
        /* If a top-level goal failed, then kill all other goals
           (unless keepGoing was set). */
        if (goal->exitCode == Goal::ecFailed && !settings.keepGoing)
            topGoals.clear();
    }
}

void Worker::wakeUp(GoalPtr goal)
{
    goal->trace("woken up");
    addToWeakGoals(awake, goal);
}

size_t Worker::getNrLocalBuilds()
{
    return nrLocalBuilds;
}

size_t Worker::getNrSubstitutions()
{
    return nrSubstitutions;
}

void Worker::childStarted(
    GoalPtr goal, const std::set<MuxablePipePollState::CommChannel> & channels, bool inBuildSlot, bool respectTimeouts)
{
    Child child;
    child.goal = goal;
    child.goal2 = goal.get();
    child.channels = channels;
    child.timeStarted = child.lastOutput = steady_time_point::clock::now();
    child.inBuildSlot = inBuildSlot;
    child.respectTimeouts = respectTimeouts;
    children.emplace_back(child);
    if (inBuildSlot) {
        switch (goal->jobCategory()) {
        case JobCategory::Substitution:
            nrSubstitutions++;
            break;
        case JobCategory::Build:
            nrLocalBuilds++;
            break;
        case JobCategory::Administration:
        default:
            /* Doesn't make sense, since there are only building and substitution slots. */
            unreachable();
        }
    }
}

void Worker::childTerminated(Goal * goal)
{
    childTerminated(goal, goal->jobCategory());
}

void Worker::childTerminated(Goal * goal, JobCategory jobCategory)
{
    // FIXME: Inefficient. Make children a map from Goal -> Child instead.
    auto i = std::find_if(children.begin(), children.end(), [&](const Child & child) { return child.goal2 == goal; });
    if (i == children.end())
        return;

    if (i->inBuildSlot) {
        switch (jobCategory) {
        case JobCategory::Substitution:
            assert(nrSubstitutions > 0);
            nrSubstitutions--;
            break;
        case JobCategory::Build:
            assert(nrLocalBuilds > 0);
            nrLocalBuilds--;
            break;
        case JobCategory::Administration:
        default:
            /* Doesn't make sense, since there are only building and substitution slots. */
            unreachable();
        }
    }

    children.erase(i);
}

bool Worker::slotAvailable(JobCategory category)
{
    if (category == JobCategory::Substitution)
        return getNrSubstitutions() < settings.maxSubstitutionJobs;
    else
        return getNrLocalBuilds() < settings.maxBuildJobs;
}

namespace {

/**
 * The executor of the goals of a `Worker`, as a leaf: each poll runs the
 * goals that woke (`Worker::runRound`), until none is awake. It completes
 * when the top-level goals have finished.
 */
struct GoalExecutorLeaf : LeafAwaiter
{
    Worker & worker;
    std::exception_ptr error;

    /**
     * Wakes the executor every 10 seconds when the store collects garbage
     * automatically, to check the free space (`LocalStore::autoGC`).
     */
    bool wantGcTimer;
    std::optional<Task<void>> gcTimer;

    GoalExecutorLeaf(Worker & worker, bool wantGcTimer)
        : worker(worker)
        , wantGcTimer(wantGcTimer)
    {
    }

    GoalExecutorLeaf(const GoalExecutorLeaf &) = delete;

    nix_poll pollLeaf(const Waker & waker) override
    {
        worker.setExecutorWaker(waker);
        try {
            while (true) {
                if (worker.runRound())
                    return NIX_POLL_READY;
                if (!wantGcTimer)
                    return NIX_POLL_PENDING;
                if (!gcTimer)
                    gcTimer.emplace(sleepFor(std::chrono::seconds(10)));
                if (gcTimer->poll(waker) == NIX_POLL_PENDING)
                    return NIX_POLL_PENDING;
                gcTimer.reset();
            }
        } catch (...) {
            /* `pollLeaf` must not throw; the exception goes to the
               awaiter. */
            error = std::current_exception();
            return NIX_POLL_READY;
        }
    }

    void await_resume()
    {
        if (error)
            std::rethrow_exception(error);
    }
};

} // namespace

void Worker::setExecutorWaker(const nix::Waker & waker)
{
    auto w(wakerState->executor.lock());
    if (!w->willWake(waker))
        *w = waker;
}

Task<void> Worker::runGoals(Goals _topGoals)
{
    debug("entered goal loop");
    for (std::shared_ptr<Goal> goal : _topGoals)
        topGoals.insert(std::move(goal));
    _topGoals.clear();

    auto localStore = dynamic_cast<LocalStore *>(&store);
    bool wantGcTimer = localStore && localStore->config->getLocalSettings().getGCSettings().minFree.get() != 0;

    auto execute = [](Worker & worker, bool wantGcTimer) -> Task<void> {
        co_await GoalExecutorLeaf(worker, wantGcTimer);
    };

    /* A user interrupt cancels the goals, also when the executor that
       polls this task does not check for interrupts. */
    co_await race(execute(*this, wantGcTimer), waitForInterrupt());

    /* If --keep-going is not set, it's possible that the main goal
       exited while some of its subgoals were still active.  But if
       --keep-going *is* set, then they must all be finished now. */
    assert(!settings.keepGoing || awake.empty());
    assert(!settings.keepGoing || wantingToBuild.empty());
    assert(!settings.keepGoing || wantingToSubstitute.empty());
    assert(!settings.keepGoing || children.empty());
}

void Worker::run(const Goals & topGoals)
{
    runBlocking(runGoals(topGoals));
}

namespace {

/**
 * The threshold of a slow poll, as for the reactor: `_NIX_REACTOR_SLOW_POLL_MS`,
 * 50 ms by default.
 */
std::chrono::milliseconds slowPollThreshold()
{
    static const auto threshold = [] {
        if (auto s = getEnv("_NIX_REACTOR_SLOW_POLL_MS"))
            if (auto n = string2Int<int64_t>(*s))
                return std::chrono::milliseconds(*n);
        return std::chrono::milliseconds(50);
    }();
    return threshold;
}

} // namespace

void Worker::recordGoalPoll(const Goal & goal, std::chrono::steady_clock::duration duration)
{
    auto & s = goalPollStats;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(duration);
    s.polls++;
    s.busy += us;
    if (us > s.longest) {
        s.longest = us;
        s.longestGoal = goal.name;
    }
    using namespace std::chrono_literals;
    if (us >= 1ms)
        s.atLeast[0]++;
    if (us >= 10ms)
        s.atLeast[1]++;
    if (us >= 50ms)
        s.atLeast[2]++;
    if (duration >= slowPollThreshold())
        debug(
            "the goal '%s' held the thread of the goals for %d ms in one poll; no other goal ran in that time",
            goal.name,
            us.count() / 1000);
}

bool Worker::runRound()
{
    // TODO GC interface?
    if (auto localStore = dynamic_cast<LocalStore *>(&store))
        localStore->autoGC(false);

    wakerState->wakeAll(*this);

    /* Call every wake goal (in the ordering established by
       CompareGoalPtrs). */
    while (!awake.empty() && !topGoals.empty()) {
        Goals awake2;
        for (auto & i : awake) {
            GoalPtr goal = i.lock();
            if (goal)
                awake2.insert(goal);
        }
        awake.clear();

        for (auto & goal : awake2) {
            auto startTime = std::chrono::steady_clock::now();

            goal->work();

            auto duration = std::chrono::steady_clock::now() - startTime;
            recordGoalPoll(*goal, duration);

            /* Useful for tracing which goals hod the event loop. */
            vomit(
                "worker event loop worked goal '%1%' for %2$.3fms",
                goal->name,
                std::chrono::duration_cast<std::chrono::duration<float, std::milli>>(duration).count());

            if (topGoals.empty())
                break; // stuff may have been cancelled
        }

        auto wakeSlotWaiters = [](WaitQueue & waiting, size_t running, size_t limit) {
            while (running < limit && waiting.wakeOne())
                ++running;
        };

        wakeSlotWaiters(
            wantingToSubstitute, getNrSubstitutions(), std::max<std::size_t>(1, settings.maxSubstitutionJobs));
        wakeSlotWaiters(wantingToBuild, getNrLocalBuilds(), settings.maxBuildJobs);

        /* The goals that woke during this round, on this thread. */
        wakerState->wakeAll(*this);
    }

    if (topGoals.empty())
        return true;

    /* Wait for a goal that waits for something other than another goal
       or a slot. Otherwise no goal can continue. */
    if (!children.empty() || nrGoalsSuspendedElsewhere > 0)
        return false;

    if (0U == settings.maxBuildJobs) {
        if (Machine::parseConfig({nix::settings.thisSystem}, nix::settings.getWorkerSettings().builders).empty())
            throw Error(
                "Unable to start any build; either increase '--max-jobs' or enable remote builds.\n"
                "\n"
                "For more information run 'man nix.conf' and search for '/machines'.");
        else
            throw Error(
                "Unable to start any build; remote machines may not have all required system features.\n"
                "\n"
                "For more information run 'man nix.conf' and search for '/machines'.");
    }

    throw Error("internal error: no goal can continue, but the build is not finished");
}

Task<void> Worker::waitForChild(Goal & goal)
{
#ifndef _WIN32
    auto findChild = [&]() {
        return std::find_if(children.begin(), children.end(), [&](const Child & c) { return c.goal2 == &goal; });
    };

    auto i = findChild();
    if (i == children.end())
        throw Error("internal error: goal '%s' waits for a child that it does not have", goal.getName());

    /* The first deadline of the child, if it has one. */
    std::optional<steady_time_point> deadline;
    if (i->respectTimeouts) {
        if (0 != settings.maxSilentTime)
            deadline = i->lastOutput + std::chrono::seconds(settings.maxSilentTime);
        if (0 != settings.buildTimeout) {
            auto d = i->timeStarted + std::chrono::seconds(settings.buildTimeout);
            deadline = deadline ? std::min(*deadline, d) : d;
        }
    }

    /* Wait until the first channel is readable (EOF included), or until
       the deadline. The set cancels the other waits at the end of this
       scope, which removes their descriptors from the reactor. */
    static constexpr int timerExpired = -1;
    std::optional<int> fired;
    {
        TaskSet<int> waits;
        for (auto fd : i->channels)
            waits.push([](int fd) -> Task<int> {
                co_await waitFd(fd, true, false);
                co_return fd;
            }(fd));
        if (deadline) {
            auto delay = std::chrono::ceil<std::chrono::milliseconds>(*deadline - steady_time_point::clock::now());
            waits.push([](std::chrono::milliseconds delay) -> Task<int> {
                co_await sleepFor(delay);
                co_return timerExpired;
            }(delay));
        }
        fired = co_await waits.next();
    }

    /* Only the goal's own task awaits this, so the goal is alive. Look up
       the child again: the list of children can have changed. */
    i = findChild();
    if (i == children.end())
        co_return;

    auto after = steady_time_point::clock::now();

    if (fired && *fired != timerExpired) {
        /* Read once from every channel that is ready now, in the order of
           the descriptors, as the poll loop of the Worker did. Reading
           only the channel that woke would deliver the EOF of one channel
           before the data that is waiting on another, for example the
           log of a remote build on the channel of the build hook. */
        MuxablePipePollState state;
        for (auto fd : i->channels) {
            state.pollStatus.push_back(pollfd{.fd = fd, .events = POLLIN});
            state.fdToPollStatus[fd] = state.pollStatus.size() - 1;
        }
        state.poll(std::optional<unsigned int>{0});
        state.iterate(
            i->channels,
            [&](Descriptor fd, std::string_view data) {
                printMsg(lvlVomit, "%1%: read %2% bytes", goal.getName(), data.size());
                i->lastOutput = after;
                goal.handleChildOutput(fd, data);
            },
            [&](Descriptor fd) {
                debug("%1%: got EOF", goal.getName());
                goal.handleEOF(fd);
            });
    }

    if (!goal.exitCode && 0 != settings.maxSilentTime && i->respectTimeouts
        && after - i->lastOutput >= std::chrono::seconds(settings.maxSilentTime)) {
        goal.timedOut(TimedOut(settings.maxSilentTime));
    }

    else if (
        !goal.exitCode && 0 != settings.buildTimeout && i->respectTimeouts
        && after - i->timeStarted >= std::chrono::seconds(settings.buildTimeout)) {
        goal.timedOut(TimedOut(settings.buildTimeout));
    }
#else
    /* Windows has no `waitFd`: poll the completion port of the children
       now and then. */
    co_await sleepFor(std::chrono::milliseconds(50));
    pollChildrenNow();
#endif
}

#ifdef _WIN32
void Worker::pollChildrenNow()
{
    MuxablePipePollState state;

    state.poll(ioport.get(), std::optional<unsigned int>{0});

    auto after = steady_time_point::clock::now();

    /* Process all available file descriptors. FIXME: this is
       O(children * fds). */
    decltype(children)::iterator i;
    for (auto j = children.begin(); j != children.end(); j = i) {
        i = std::next(j);

        GoalPtr goal = j->goal.lock();
        assert(goal);

        state.iterate(
            j->channels,
            [&](Descriptor k, std::string_view data) {
                printMsg(lvlVomit, "%1%: read %2% bytes", goal->getName(), data.size());
                j->lastOutput = after;
                goal->handleChildOutput(k, data);
            },
            [&](Descriptor k) {
                debug("%1%: got EOF", goal->getName());
                goal->handleEOF(k);
            });

        if (!goal->exitCode && 0 != settings.maxSilentTime && j->respectTimeouts
            && after - j->lastOutput >= std::chrono::seconds(settings.maxSilentTime)) {
            goal->timedOut(TimedOut(settings.maxSilentTime));
        }

        else if (
            !goal->exitCode && 0 != settings.buildTimeout && j->respectTimeouts
            && after - j->timeStarted >= std::chrono::seconds(settings.buildTimeout)) {
            goal->timedOut(TimedOut(settings.buildTimeout));
        }
    }
}
#endif

std::weak_ptr<Worker::WakeupQueue> Worker::getCrossThreadWaker()
{
    return wakerState.get_ptr();
}

void Worker::WakeupQueue::wakeAll(Worker & worker)
{
    /* Take the queue first: releasing the last reference to a goal below
       destroys it, and its destruction can wake other goals. */
    std::queue<WeakGoalPtr> queue;
    std::swap(queue, *wakeupQueue_.lock());
    while (!queue.empty()) {
        auto ptr = queue.front().lock();
        queue.pop();
        if (ptr)
            worker.wakeUp(ptr);
    }
}

void Worker::WakeupQueue::enqueue(WeakGoalPtr goal)
{
    wakeupQueue_.lock()->push(goal);
    /* Copy the waker, so that the wake runs outside of the lock. */
    nix::Waker executorWaker = *executor.lock();
    executorWaker.wakeByRef();
}

bool Worker::pathContentsGood(const StorePath & path)
{
    auto i = pathContentsGoodCache.find(path);
    if (i != pathContentsGoodCache.end())
        return i->second;
    printInfo("checking path '%s'...", store.printStorePath(path));
    auto info = store.queryPathInfo(path);
    bool res = false;
    if (auto accessor = store.getFSAccessor(path, /*requireValidPath=*/false)) {
        auto current = hashPath({ref{accessor}}, FileIngestionMethod::NixArchive, info->narHash.algo).first;
        Hash nullHash(HashAlgorithm::SHA256);
        res = info->narHash == nullHash || info->narHash == current;
    }
    pathContentsGoodCache.insert_or_assign(path, res);
    if (!res)
        printError("path '%s' is corrupted or missing!", store.printStorePath(path));
    return res;
}

void Worker::markContentsGood(const StorePath & path)
{
    pathContentsGoodCache.insert_or_assign(path, true);
}

GoalPtr upcast_goal(std::shared_ptr<PathSubstitutionGoal> subGoal)
{
    return subGoal;
}

GoalPtr upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal> subGoal)
{
    return subGoal;
}

GoalPtr upcast_goal(std::shared_ptr<DerivationGoal> subGoal)
{
    return subGoal;
}

} // namespace nix

#include <gtest/gtest.h>

#include "nix/util/task.hh"
#include "nix/util/task-thread.hh"
#include "nix/util/error.hh"
#include "nix/util/rust-error.hh"
#include "nix/util/signals.hh"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nix {

namespace {

using Log = std::vector<std::string>;

/**
 * A leaf that completes when the test calls `fire()`.
 */
struct Trigger
{
    struct State
    {
        bool ready = false;
        Waker waker;
        int polls = 0;
    };

    std::shared_ptr<State> state = std::make_shared<State>();

    void fire()
    {
        state->ready = true;
        std::move(state->waker).wake();
    }

    struct Awaiter : LeafAwaiter
    {
        std::shared_ptr<State> state;
        Log * log;
        std::string name;

        Awaiter(std::shared_ptr<State> state, Log * log, std::string name)
            : state(std::move(state))
            , log(log)
            , name(std::move(name))
        {
        }

        Awaiter(const Awaiter &) = delete;

        ~Awaiter()
        {
            /* Cancel: the trigger no longer wakes this task. */
            state->waker.reset();
            if (log)
                log->push_back(name + " leaf destroyed");
        }

        nix_poll pollLeaf(const Waker & waker) override
        {
            state->polls++;
            if (state->ready)
                return NIX_POLL_READY;
            state->waker = waker;
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    Awaiter wait(Log * log = nullptr, std::string name = "")
    {
        return Awaiter(state, log, std::move(name));
    }
};

/**
 * A waker that counts how often it is woken.
 */
struct CountingTarget : WakeTarget
{
    std::atomic<int> count = 0;

    void wake() noexcept override
    {
        count++;
    }
};

struct Guard
{
    Log & log;
    std::string name;

    ~Guard()
    {
        log.push_back(name + " destroyed");
    }
};

MakeError(TestError, Error);
MakeError(DerivedTestError, TestError);

void TestError::anchor() {}

void DerivedTestError::anchor() {}

} // namespace

TEST(Task, isLazy)
{
    Log log;
    auto task = [](Log & log) -> Task<int> {
        log.push_back("body");
        co_return 1;
    }(log);

    EXPECT_EQ(log, Log{});

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(task.takeResult(), 1);
    EXPECT_EQ(log, Log{"body"});
}

TEST(Task, chainSuspendsOnLeafAndResumesOnWake)
{
    Log log;
    Trigger trigger;

    auto inner = [](Log & log, Trigger & trigger) -> Task<int> {
        log.push_back("inner start");
        co_await trigger.wait();
        log.push_back("inner resumed");
        co_return 41;
    };

    auto outer = [](Log & log, Task<int> inner) -> Task<int> {
        log.push_back("outer start");
        auto v = co_await std::move(inner);
        log.push_back("outer resumed");
        co_return v + 1;
    }(log, inner(log, trigger));

    auto target = makeWakeTarget<CountingTarget>();
    auto waker = target->waker();

    EXPECT_EQ(outer.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"outer start", "inner start"}));
    EXPECT_EQ(target->count, 0);

    /* A spurious poll does not resume the chain. */
    EXPECT_EQ(outer.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"outer start", "inner start"}));

    trigger.fire();
    EXPECT_EQ(target->count, 1);

    EXPECT_EQ(outer.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(outer.takeResult(), 42);
    EXPECT_EQ(log, (Log{"outer start", "inner start", "inner resumed", "outer resumed"}));
}

TEST(Task, dropCancelsTheWholeChain)
{
    Log log;
    Trigger trigger;

    auto inner = [](Log & log, Trigger & trigger) -> Task<void> {
        Guard guard{log, "inner local"};
        try {
            co_await trigger.wait(&log, "inner");
        } catch (...) {
            log.push_back("inner catch");
            throw;
        }
        log.push_back("inner resumed");
    };

    auto outer = [](Log & log, Task<void> inner) -> Task<void> {
        Guard guard{log, "outer local"};
        co_await std::move(inner);
        log.push_back("outer resumed");
    }(log, inner(log, trigger));

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(outer.poll(target->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(log, Log{});

    outer.reset();

    /* The leaf, then the inner frame, then the outer frame. No catch
       block runs, and nothing resumes. */
    EXPECT_EQ(log, (Log{"inner leaf destroyed", "inner local destroyed", "outer local destroyed"}));

    /* The leaf's waker was dropped with it; firing wakes nobody. */
    trigger.fire();
    EXPECT_EQ(target->count, 0);
    EXPECT_EQ(log, (Log{"inner leaf destroyed", "inner local destroyed", "outer local destroyed"}));
}

TEST(Task, exceptionKeepsDynamicType)
{
    auto inner = []() -> Task<int> {
        throw DerivedTestError("boom");
        co_return 0;
    };

    auto outer = [](Task<int> inner) -> Task<std::string> {
        try {
            co_await std::move(inner);
        } catch (DerivedTestError & e) {
            co_return std::string("caught derived: ") + e.info().msg.str();
        }
        co_return "not caught";
    }(inner());

    EXPECT_EQ(runBlocking(std::move(outer)), "caught derived: boom");

    try {
        runBlocking(inner());
        FAIL() << "expected an exception";
    } catch (TestError & e) {
        EXPECT_NE(dynamic_cast<DerivedTestError *>(&e), nullptr);
        EXPECT_EQ(e.info().msg.str(), "boom");
    }
}

TEST(Task, callbackFromAnotherThread)
{
    std::thread thread;
    auto task = callbackToTask<int>([&](Callback<int> cb) {
        thread = std::thread([cb = std::move(cb)]() mutable {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            cb(17);
        });
    });
    EXPECT_EQ(runBlocking(std::move(task)), 17);
    thread.join();
}

TEST(Task, callbackCancellationCallsCancellerAndDiscardsLateResult)
{
    Log log;
    std::optional<Callback<int>> pending;

    auto task = callbackToTask<int>([&](Callback<int> cb) {
        log.push_back("initiated");
        pending.emplace(std::move(cb));
        return [&]() { log.push_back("cancelled"); };
    });

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    task.reset();
    EXPECT_EQ(log, (Log{"initiated", "cancelled"}));

    /* The operation completes after the task is gone. */
    (*pending)(5);
    EXPECT_EQ(target->count, 0);
}

TEST(Task, callbackCompletedSynchronouslyDoesNotCancel)
{
    Log log;
    auto task = callbackToTask<int>([&](Callback<int> cb) {
        cb(3);
        return [&]() { log.push_back("cancelled"); };
    });
    EXPECT_EQ(runBlocking(std::move(task)), 3);
    EXPECT_EQ(log, Log{});
}

TEST(Task, whenAllReturnsResultsInOrder)
{
    Trigger t0, t1, t2;

    auto child = [](Trigger & t, int v) -> Task<int> {
        co_await t.wait();
        co_return v;
    };

    std::vector<Task<int>> tasks;
    tasks.push_back(child(t0, 10));
    tasks.push_back(child(t1, 11));
    tasks.push_back(child(t2, 12));
    auto all = whenAll(std::move(tasks));

    auto target = makeWakeTarget<CountingTarget>();
    auto waker = target->waker();
    EXPECT_EQ(all.poll(waker), NIX_POLL_PENDING);

    t2.fire();
    t0.fire();
    EXPECT_EQ(target->count, 2);
    EXPECT_EQ(all.poll(waker), NIX_POLL_PENDING);

    /* Only the woken children are polled again. */
    EXPECT_EQ(t0.state->polls, 2);
    EXPECT_EQ(t1.state->polls, 1);
    EXPECT_EQ(t2.state->polls, 2);

    t1.fire();
    EXPECT_EQ(all.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(all.takeResult(), (std::vector<int>{10, 11, 12}));
}

TEST(Task, whenAllErrorCancelsSiblings)
{
    Log log;
    Trigger failing, other;

    auto fails = [](Log & log, Trigger & t) -> Task<void> {
        co_await t.wait(&log, "failing");
        throw TestError("first failure");
    };

    auto waits = [](Log & log, Trigger & t, std::string name) -> Task<void> {
        Guard guard{log, name + " local"};
        co_await t.wait(&log, name);
        log.push_back(name + " resumed");
    };

    std::vector<Task<void>> tasks;
    tasks.push_back(waits(log, other, "sibling A"));
    tasks.push_back(fails(log, failing));
    tasks.push_back(waits(log, other, "sibling B"));

    auto all = whenAll(std::move(tasks));
    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);

    failing.fire();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_READY);

    EXPECT_EQ(
        log,
        (Log{
            "failing leaf destroyed",
            "sibling A leaf destroyed",
            "sibling A local destroyed",
            "sibling B leaf destroyed",
            "sibling B local destroyed",
        }));

    EXPECT_THROW(all.takeResult(), TestError);
}

TEST(Task, raceCancelsLoser)
{
    Log log;
    Trigger fast, slow;

    auto a = [](Trigger & t) -> Task<int> {
        co_await t.wait();
        co_return 1;
    }(fast);

    auto b = [](Log & log, Trigger & t) -> Task<void> {
        Guard guard{log, "loser local"};
        co_await t.wait(&log, "loser");
    }(log, slow);

    auto r = race(std::move(a), std::move(b));
    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(r.poll(target->waker()), NIX_POLL_PENDING);

    fast.fire();
    EXPECT_EQ(r.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(log, (Log{"loser leaf destroyed", "loser local destroyed"}));

    auto result = r.takeResult();
    EXPECT_EQ(result.index(), 0u);
    EXPECT_EQ(std::get<0>(result), 1);
}

TEST(Task, taskSetReturnsResultsInCompletionOrder)
{
    Trigger t0, t1, t2;

    auto child = [](Trigger & t, int v) -> Task<int> {
        co_await t.wait();
        co_return v;
    };

    TaskSet<int> set;
    set.push(child(t0, 10));
    set.push(child(t1, 11));
    set.push(child(t2, 12));
    EXPECT_EQ(set.size(), 3u);

    auto target = makeWakeTarget<CountingTarget>();
    auto waker = target->waker();

    auto next = set.next();
    EXPECT_EQ(next.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(t0.state->polls, 1);
    EXPECT_EQ(t1.state->polls, 1);
    EXPECT_EQ(t2.state->polls, 1);

    t2.fire();
    EXPECT_EQ(target->count, 1);
    EXPECT_EQ(next.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(next.takeResult(), std::optional<int>(12));
    EXPECT_EQ(set.size(), 2u);

    /* Only the woken task was polled again. */
    EXPECT_EQ(t0.state->polls, 1);
    EXPECT_EQ(t1.state->polls, 1);
    EXPECT_EQ(t2.state->polls, 2);

    t0.fire();
    t1.fire();
    next = set.next();
    EXPECT_EQ(next.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(next.takeResult(), std::optional<int>(10));

    /* The second task woke too; the next call returns it without a new
       wake. */
    next = set.next();
    EXPECT_EQ(next.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(next.takeResult(), std::optional<int>(11));

    EXPECT_EQ(set.size(), 0u);
    EXPECT_EQ(runBlocking(set.next()), std::nullopt);
}

TEST(Task, taskSetAcceptsTasksWhileOthersRun)
{
    Trigger slow, fast;

    auto child = [](Trigger & t, int v) -> Task<int> {
        co_await t.wait();
        co_return v;
    };

    auto driver = [](TaskSet<int> & set, Task<int> later) -> Task<std::vector<int>> {
        std::vector<int> results;
        set.push(std::move(later));
        while (auto r = co_await set.next())
            results.push_back(*r);
        co_return results;
    };

    TaskSet<int> set;
    set.push(child(slow, 1));

    auto target = makeWakeTarget<CountingTarget>();
    auto waker = target->waker();

    /* A task that pushes into the set while the first task is pending. */
    auto task = driver(set, child(fast, 2));
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(set.size(), 2u);

    fast.fire();
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    slow.fire();
    EXPECT_EQ(task.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(task.takeResult(), (std::vector<int>{2, 1}));
}

TEST(Task, taskSetErrorLeavesOtherTasks)
{
    Log log;
    Trigger failing, other;

    auto fails = [](Trigger & t) -> Task<int> {
        co_await t.wait();
        throw TestError("edge failure");
    };

    auto waits = [](Log & log, Trigger & t) -> Task<int> {
        Guard guard{log, "other local"};
        co_await t.wait(&log, "other");
        co_return 1;
    };

    auto set = std::make_unique<TaskSet<int>>();
    set->push(waits(log, other));
    set->push(fails(failing));

    auto target = makeWakeTarget<CountingTarget>();
    auto next = set->next();
    EXPECT_EQ(next.poll(target->waker()), NIX_POLL_PENDING);

    failing.fire();
    EXPECT_EQ(next.poll(target->waker()), NIX_POLL_READY);
    EXPECT_THROW(next.takeResult(), TestError);

    /* The error removes only the task that threw. */
    EXPECT_EQ(set->size(), 1u);
    EXPECT_EQ(log, Log{});

    /* Destroying the set cancels the rest. */
    set.reset();
    EXPECT_EQ(log, (Log{"other leaf destroyed", "other local destroyed"}));
}

TEST(Task, runInThreadReturnsResult)
{
    auto caller = std::this_thread::get_id();
    auto task = runInThread<bool>([caller]() { return std::this_thread::get_id() != caller; });
    EXPECT_EQ(runBlocking(std::move(task)), true);
}

TEST(Task, runInThreadKeepsExceptionType)
{
    auto task = runInThread<void>([]() { throw DerivedTestError("from the thread"); });
    try {
        runBlocking(std::move(task));
        FAIL() << "expected an exception";
    } catch (TestError & e) {
        EXPECT_NE(dynamic_cast<DerivedTestError *>(&e), nullptr);
        EXPECT_EQ(e.info().msg.str(), "from the thread");
    }
}

TEST(Task, runInThreadIsLazy)
{
    std::atomic<bool> ran = false;
    {
        auto task = runInThread<void>([&]() { ran = true; });
    }
    EXPECT_EQ(ran.load(), false);
}

#ifndef _WIN32 // `checkInterrupt()` does nothing on Windows
TEST(Task, runInThreadDropInterruptsTheFunction)
{
    std::atomic<bool> started = false;
    std::atomic<bool> interrupted = false;

    auto task = runInThread<void>([&]() {
        started = true;
        try {
            while (true) {
                checkInterrupt();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } catch (Interrupted &) {
            interrupted = true;
            throw;
        }
    });

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    while (!started)
        std::this_thread::yield();

    /* The destruction waits for the thread, so the function has seen the
       interrupt when it returns. */
    task.reset();
    EXPECT_EQ(interrupted.load(), true);
    EXPECT_EQ(target->count, 0);
    /* The interrupt was for that thread only. */
    EXPECT_EQ(isInterrupted(), false);
}
#endif

TEST(Task, runInThreadDropCallsCancellationCallback)
{
    std::mutex mutex;
    std::condition_variable cv;
    bool registered = false;
    bool woken = false;

    /* A blocking wait that does not check for interrupts. Only the
       cancellation callback ends it. */
    auto task = runInThread<void>([&]() {
        auto callback = onThreadCancelled([&]() {
            std::lock_guard lock(mutex);
            woken = true;
            cv.notify_all();
        });
        std::unique_lock lock(mutex);
        registered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return woken; });
    });

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return registered; });
    }

    task.reset();
    EXPECT_EQ(woken, true);
}

#ifndef _WIN32 // No user interrupts on Windows
TEST(Task, waitForInterruptThrowsOnUserInterrupt)
{
    auto task = waitForInterrupt();
    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    unix::triggerInterrupt();
    EXPECT_EQ(target->count, 1);
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    EXPECT_THROW(task.takeResult(), Interrupted);
    setInterrupted(false);
}

TEST(Task, runBlockingInRunInThreadEndsWhenDropped)
{
    std::atomic<bool> started = false;
    std::atomic<bool> interrupted = false;

    /* A task that never completes, run with `runBlocking` on the thread of
       a `runInThread` task: only the drop ends it. */
    auto task = runInThread<void>([&]() {
        started = true;
        try {
            runBlocking(waitForInterrupt());
        } catch (Interrupted &) {
            interrupted = true;
            throw;
        }
    });

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    while (!started)
        std::this_thread::yield();

    task.reset();
    EXPECT_EQ(interrupted.load(), true);
    EXPECT_EQ(isInterrupted(), false);
}
#endif

TEST(Task, cancellationCallbackOutsideRunInThreadIsNotCalled)
{
    bool called = false;
    {
        auto callback = onThreadCancelled([&]() { called = true; });
    }
    EXPECT_EQ(called, false);
}

/* A waker implemented only in terms of the C ABI, as Rust would. */
namespace {

struct CWakerState
{
    int wakes = 0;
    int clones = 0;
    int drops = 0;
};

extern "C" {

static nix_waker cWakerClone(const void * data);

static void cWakerWake(const void * data)
{
    auto * s = (CWakerState *) data;
    s->wakes++;
    s->drops++;
}

static void cWakerWakeByRef(const void * data)
{
    ((CWakerState *) data)->wakes++;
}

static void cWakerDrop(const void * data)
{
    ((CWakerState *) data)->drops++;
}

static const nix_waker_vtable cWakerVtable = {
    .clone = cWakerClone,
    .wake = cWakerWake,
    .wake_by_ref = cWakerWakeByRef,
    .drop = cWakerDrop,
};

static nix_waker cWakerClone(const void * data)
{
    ((CWakerState *) data)->clones++;
    return nix_waker{data, &cWakerVtable};
}
}

} // namespace

TEST(Task, pollThroughCAbi)
{
    Trigger trigger;
    auto future = toCFuture([](Trigger & t) -> Task<std::int64_t> {
        co_await t.wait();
        co_return 99;
    }(trigger));

    CWakerState state;
    nix_waker waker{&state, &cWakerVtable};

    EXPECT_EQ(future.vtable->poll(future.self, &waker), NIX_POLL_PENDING);
    /* The leaf keeps a clone of the borrowed waker. */
    EXPECT_EQ(state.clones, 1);

    trigger.fire();
    EXPECT_EQ(state.wakes, 1);
    EXPECT_EQ(state.drops, 1);

    EXPECT_EQ(future.vtable->poll(future.self, &waker), NIX_POLL_READY);
    EXPECT_EQ(takeCFuture<std::int64_t>(future), 99);
    future.vtable->drop(future.self);

    /* Every clone was released. */
    EXPECT_EQ(state.clones, state.drops);
}

TEST(Task, awaitCFutureDropsItOnCancellation)
{
    Log log;
    Trigger trigger;

    auto inner = toCFuture([](Log & log, Trigger & t) -> Task<std::int64_t> {
        Guard guard{log, "foreign local"};
        co_await t.wait();
        co_return 5;
    }(log, trigger));

    auto outer = awaitCFuture<std::int64_t>(inner);
    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(outer.poll(target->waker()), NIX_POLL_PENDING);

    outer.reset();
    EXPECT_EQ(log, Log{"foreign local destroyed"});
}

TEST(Task, awaitCFutureTakesResult)
{
    Trigger trigger;
    trigger.fire();
    auto inner = toCFuture([](Trigger & t) -> Task<std::int64_t> {
        co_await t.wait();
        co_return 5;
    }(trigger));
    EXPECT_EQ(runBlocking(awaitCFuture<std::int64_t>(inner)), 5);
}

TEST(Task, awaitCFutureRethrowsWithDynamicType)
{
    auto inner = toCFuture([]() -> Task<void> {
        throw SysError(ENOENT, "opening a file");
        co_return;
    }());
    try {
        runBlocking(awaitCFuture<void>(inner));
        FAIL() << "the task did not throw";
    } catch (const SysError & e) {
        EXPECT_EQ(e.errNo, ENOENT);
    }
}

TEST(Task, rustErrorFromFactoryIsNixError)
{
    detail::registerRustErrorFactory();

    /* A stand-in for a Rust value: a counter of its drops. */
    static int drops = 0;
    drops = 0;
    nix_rust_value value{
        .kind = NIX_RUST_PANIC,
        .data = &drops,
        .drop = [](void * data) { ++*static_cast<int *>(data); },
        .owner = nullptr,
    };

    auto * exception = nix_exception_from_rust("boom", 4, value);
    try {
        rs::rethrowException(exception);
    } catch (const RustPanic & e) {
        EXPECT_EQ(e.info().msg.str(), "Rust panic: boom");
        /* A copy shares the value instead of dropping it twice. */
        try {
            e.throwClone();
        } catch (const RustPanic &) {
        }
        EXPECT_EQ(drops, 0);
    }
    /* The last exception that owned the value dropped it. */
    EXPECT_EQ(drops, 1);
}

} // namespace nix

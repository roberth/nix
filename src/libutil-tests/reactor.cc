#include <gtest/gtest.h>

#include "nix/util/error.hh"
#include "nix/util/reactor.hh"
#include "nix/util/task-thread.hh"
#include "nix/util/signals.hh"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#ifndef _WIN32
#  include <unistd.h>
#endif

namespace nix {

/* Names in this file are unique within the test program, because the
   unity build puts several test files into one translation unit. */

namespace {

struct ReactorTestLog
{
    mutable std::mutex mutex;
    mutable std::vector<std::string> events;

    void add(std::string event) const
    {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    std::vector<std::string> get() const
    {
        std::lock_guard lock(mutex);
        return events;
    }
};

struct ReactorTestError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

using ReactorClock = std::chrono::steady_clock;

/**
 * Run `f` on the reactor thread and wait for it.
 */
template<typename F>
auto onReactor(F f) -> decltype(f())
{
    std::packaged_task<decltype(f())()> task(std::move(f));
    auto result = task.get_future();
    postToReactor([&task]() { task(); });
    return result.get();
}

/**
 * Owned by a task frame; its destruction must wait `cleanup` for a
 * cleanup, like the drop of a leaf that owns a thread.
 */
struct ReactorTestSlowDrop
{
    const ReactorTestLog & log;
    std::chrono::milliseconds cleanup;
    CleanupScope scope;

    ~ReactorTestSlowDrop()
    {
        log.add("drop");
        if (scope)
            scope.defer([](const ReactorTestLog & log, std::chrono::milliseconds cleanup) -> Task<void> {
                co_await sleepFor(cleanup);
                log.add("cleanup done");
            }(log, cleanup));
        else {
            std::this_thread::sleep_for(cleanup);
            log.add("cleanup done");
        }
        log.add("drop returned");
    }
};

/**
 * A task that never completes, and whose drop waits `cleanup`. Its first
 * poll logs whether it runs in a cleanup scope.
 */
Task<void> reactorTestSlowDrop(const ReactorTestLog & log, std::chrono::milliseconds cleanup)
{
    auto scope = CleanupScope::current();
    log.add(scope ? "polled in a cleanup scope" : "polled without a cleanup scope");
    ReactorTestSlowDrop slow{log, cleanup, std::move(scope)};
    co_await callbackToTask<int>([](Callback<int>) {});
}

} // namespace

TEST(Reactor, runsATaskOnTheReactorThread)
{
    EXPECT_FALSE(onReactorThread());
    auto onReactor = runOnReactor([]() -> Task<bool> { co_return onReactorThread(); }());
    EXPECT_TRUE(onReactor);
}

TEST(Reactor, aSpawnedTaskRunsOnTheReactorAndTheAwaiterGetsItsResult)
{
    auto onReactor = runBlocking(spawnOnReactor([]() -> Task<bool> { co_return onReactorThread(); }()));
    EXPECT_TRUE(onReactor);
}

TEST(Reactor, aDroppedSpawnedTaskWaitsForTheCleanupsOfTheRun)
{
    ReactorTestLog log;
    /* The sleep wins; the drop of the spawned task cancels the slow
       future on the reactor, which defers its wait to the run, and the
       drop waits for the run. */
    runBlocking(race(
        spawnOnReactor(reactorTestSlowDrop(log, std::chrono::milliseconds(10))),
        sleepFor(std::chrono::milliseconds(100))));
    log.add("dropped");
    EXPECT_EQ(
        log.get(),
        (std::vector<std::string>{
            "polled in a cleanup scope",
            "drop",
            "drop returned",
            "cleanup done",
            "dropped",
        }));
}

TEST(Reactor, anExceptionKeepsItsType)
{
    EXPECT_THROW(
        runOnReactor([]() -> Task<int> {
            throw ReactorTestError("thrown on the reactor");
            co_return 1;
        }()),
        ReactorTestError);
}

TEST(Reactor, runningATaskOnTheReactorThreadFails)
{
    auto message = onReactor([]() -> std::string {
        try {
            runOnReactor([]() -> Task<void> { co_return; }());
            return "no error";
        } catch (Error & e) {
            return e.info().msg.str();
        }
    });
    EXPECT_EQ(message, "cannot block the reactor thread to run a task on it");
}

TEST(Reactor, sleepsUnderRunBlockingAndOnTheReactor)
{
    auto start = ReactorClock::now();
    runBlocking(sleepFor(std::chrono::milliseconds(30)));
    EXPECT_GE(ReactorClock::now() - start, std::chrono::milliseconds(30));

    start = ReactorClock::now();
    runOnReactor(sleepFor(std::chrono::milliseconds(30)));
    EXPECT_GE(ReactorClock::now() - start, std::chrono::milliseconds(30));
}

TEST(Reactor, sleepIsLazy)
{
    auto sleep = sleepFor(std::chrono::milliseconds(30));
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    auto start = ReactorClock::now();
    runBlocking(std::move(sleep));
    EXPECT_GE(ReactorClock::now() - start, std::chrono::milliseconds(30));
}

TEST(Reactor, theWatchdogCountsALongPoll)
{
    onReactor([]() {});
    auto before = reactorStats();

    /* Longer than the default threshold of 50 ms. */
    onReactor([]() { std::this_thread::sleep_for(std::chrono::milliseconds(80)); });

    auto after = reactorStats();
    EXPECT_EQ(after.slowPolls, before.slowPolls + 1);
    EXPECT_GE(after.longestPoll, std::chrono::milliseconds(80));
    EXPECT_GE(after.busy - before.busy, std::chrono::milliseconds(80));
    EXPECT_GE(after.busiestSecond, std::chrono::milliseconds(80));
    EXPECT_GE(after.polls, before.polls + 1);
}

TEST(Reactor, postedFunctionsRunInOrder)
{
    std::vector<int> order;
    for (int i = 0; i < 100; ++i)
        postToReactor([&order, i]() { order.push_back(i); });
    onReactor([]() {});
    std::vector<int> expected;
    for (int i = 0; i < 100; ++i)
        expected.push_back(i);
    EXPECT_EQ(order, expected);
}

TEST(Reactor, timerFiresOnceAndCanBeCancelled)
{
    std::atomic<int> fired = 0;
    std::shared_ptr<ReactorTimer> timer;
    onReactor([&]() {
        timer = std::make_shared<ReactorTimer>([&]() { fired++; });
        timer->set(std::chrono::milliseconds(10));
    });
    runBlocking(sleepFor(std::chrono::milliseconds(100)));
    EXPECT_EQ(fired, 1);

    onReactor([&]() {
        timer->set(std::chrono::milliseconds(20));
        timer->cancel();
    });
    runBlocking(sleepFor(std::chrono::milliseconds(100)));
    EXPECT_EQ(fired, 1);

    /* A later `set` replaces an earlier one. */
    onReactor([&]() {
        timer->set(std::chrono::milliseconds(10));
        timer->set(std::chrono::milliseconds(50));
    });
    runBlocking(sleepFor(std::chrono::milliseconds(30)));
    EXPECT_EQ(fired, 1);
    runBlocking(sleepFor(std::chrono::milliseconds(100)));
    EXPECT_EQ(fired, 2);

    onReactor([&]() { timer.reset(); });
}

#ifndef _WIN32

namespace {

struct ReactorTestPipe
{
    int fds[2];

    ReactorTestPipe()
    {
        if (pipe(fds) != 0)
            throw SysError("creating a pipe");
    }

    ~ReactorTestPipe()
    {
        close(fds[0]);
        close(fds[1]);
    }

    void write(std::string_view data)
    {
        if (::write(fds[1], data.data(), data.size()) != (ssize_t) data.size())
            throw SysError("writing to a pipe");
    }
};

} // namespace

TEST(Reactor, waitsForAFileDescriptor)
{
    ReactorTestPipe pipe;
    std::thread writer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        pipe.write("x");
    });
    auto ready = runOnReactor(waitFd(pipe.fds[0], true, false));
    writer.join();
    EXPECT_EQ(ready, (FdReadiness{.readable = true, .writable = false}));

    /* The write end is writable at once, also under `runBlocking`. */
    EXPECT_EQ(runBlocking(waitFd(pipe.fds[1], false, true)), (FdReadiness{.readable = false, .writable = true}));
}

TEST(Reactor, aCancelledWaitReleasesTheFileDescriptor)
{
    ReactorTestPipe pipe;
    auto winner = runOnReactor(race(waitFd(pipe.fds[0], true, false), sleepFor(std::chrono::milliseconds(10))));
    EXPECT_EQ(winner.index(), 1u);

    /* A second registration of the descriptor works, so the cancelled
       wait removed the first one. */
    pipe.write("x");
    EXPECT_EQ(runOnReactor(waitFd(pipe.fds[0], true, false)), (FdReadiness{.readable = true, .writable = false}));
}

TEST(Reactor, aWatchReportsReadinessLikeALevelTriggeredPoll)
{
    ReactorTestPipe pipe;
    pipe.write("abc");

    /* The callback reads one byte per call. tokio registers descriptors
       edge-triggered, so without the emulation of a level-triggered poll
       there would be one call only. */
    std::mutex mutex;
    std::string read;
    std::promise<void> done;
    std::unique_ptr<FdWatch> watch;
    onReactor([&]() {
        watch = std::make_unique<FdWatch>(pipe.fds[0], [&](bool readable, bool writable) {
            EXPECT_TRUE(onReactorThread());
            EXPECT_TRUE(readable);
            EXPECT_FALSE(writable);
            char c;
            if (::read(pipe.fds[0], &c, 1) != 1)
                return;
            std::lock_guard lock(mutex);
            read += c;
            if (read.size() == 3)
                done.set_value();
        });
        watch->setInterest(true, false);
    });
    done.get_future().get();

    /* No more calls while the pipe is empty. */
    runBlocking(sleepFor(std::chrono::milliseconds(50)));
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(read, "abc");
    }

    onReactor([&]() { watch.reset(); });
}

TEST(Reactor, aWatchCanBeDroppedByItsOwnCallback)
{
    ReactorTestPipe pipe;
    pipe.write("abc");
    std::atomic<int> calls = 0;
    std::unique_ptr<FdWatch> watch;
    onReactor([&]() {
        watch = std::make_unique<FdWatch>(pipe.fds[0], [&](bool, bool) {
            calls++;
            watch.reset();
        });
        watch->setInterest(true, false);
    });
    runBlocking(sleepFor(std::chrono::milliseconds(50)));
    EXPECT_EQ(calls, 1);
}

#endif

TEST(Reactor, aDropThatWaitsDefersItsWaitToTheRun)
{
    ReactorTestLog log;
    std::thread runner([&]() {
        /* The race cancels the slow future on the reactor thread. */
        runOnReactor(
            race(reactorTestSlowDrop(log, std::chrono::milliseconds(500)), sleepFor(std::chrono::milliseconds(10))));
        log.add("runOnReactor returned");
    });

    /* While the cleanup waits, the reactor thread runs other work. */
    while (log.get().size() < 3)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto start = ReactorClock::now();
    onReactor([&]() { log.add("reactor is free"); });
    auto latency = ReactorClock::now() - start;
    runner.join();

    EXPECT_LT(latency, std::chrono::milliseconds(250));
    EXPECT_EQ(
        log.get(),
        (std::vector<std::string>{
            "polled in a cleanup scope",
            "drop",
            "drop returned",
            "reactor is free",
            "cleanup done",
            "runOnReactor returned",
        }));
}

TEST(Reactor, aDropThatWaitsWaitsInPlaceUnderRunBlocking)
{
    ReactorTestLog log;
    runBlocking(race(reactorTestSlowDrop(log, std::chrono::milliseconds(10)), sleepFor(std::chrono::milliseconds(10))));
    EXPECT_EQ(
        log.get(),
        (std::vector<std::string>{
            "polled without a cleanup scope",
            "drop",
            "cleanup done",
            "drop returned",
        }));
}

#ifndef _WIN32

TEST(Reactor, anInterruptCancelsTheRunAndWaitsForItsCleanups)
{
    ReactorTestLog log;
    std::thread interrupter([&]() {
        while (log.get().empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        unix::triggerInterrupt();
    });

    bool interrupted = false;
    try {
        runOnReactor(reactorTestSlowDrop(log, std::chrono::milliseconds(50)));
    } catch (const Interrupted &) {
        interrupted = true;
    }
    log.add("runOnReactor returned");
    interrupter.join();
    setInterrupted(false);

    EXPECT_TRUE(interrupted);
    EXPECT_EQ(
        log.get(),
        (std::vector<std::string>{
            "polled in a cleanup scope",
            "drop",
            "drop returned",
            "cleanup done",
            "runOnReactor returned",
        }));
}

#endif

TEST(Reactor, aDroppedThreadTaskDefersItsJoinToTheRun)
{
    ReactorTestLog log;
    std::atomic<bool> started = false;

    /* The function takes 500 ms to stop after its cancellation, like a
       blocking call that checks rarely. */
    auto slowToStop = runInThread<void>([&]() {
        auto callback = onThreadCancelled([&]() { log.add("thread cancelled"); });
        started = true;
        while (log.get().empty())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        log.add("thread done");
    });
    auto whenStarted = [](const std::atomic<bool> & started) -> Task<void> {
        while (!started)
            co_await sleepFor(std::chrono::milliseconds(1));
    };

    std::thread runner([&]() {
        /* The race cancels the thread task on the reactor thread. */
        runOnReactor(race(std::move(slowToStop), whenStarted(started)));
        log.add("runOnReactor returned");
    });

    while (log.get().empty())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto start = ReactorClock::now();
    onReactor([&]() { log.add("reactor is free"); });
    auto latency = ReactorClock::now() - start;
    runner.join();

    EXPECT_LT(latency, std::chrono::milliseconds(250));
    EXPECT_EQ(
        log.get(),
        (std::vector<std::string>{
            "thread cancelled",
            "reactor is free",
            "thread done",
            "runOnReactor returned",
        }));
}

} // namespace nix

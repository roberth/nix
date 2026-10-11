#include <gtest/gtest.h>

#include "nix/util/fiber.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"

#include <string>
#include <vector>

namespace nix {

namespace {

using FiberLog = std::vector<std::string>;

/**
 * A leaf that completes when the test calls `open()`.
 */
struct Gate
{
    struct State
    {
        bool open = false;
        Waker waker;
    };

    std::shared_ptr<State> state = std::make_shared<State>();

    void open()
    {
        state->open = true;
        std::move(state->waker).wake();
    }

    bool waiting() const
    {
        return bool(state->waker);
    }

    struct Awaiter : LeafAwaiter
    {
        std::shared_ptr<State> state;
        FiberLog * log;

        Awaiter(std::shared_ptr<State> state, FiberLog * log)
            : state(std::move(state))
            , log(log)
        {
        }

        Awaiter(const Awaiter &) = delete;

        ~Awaiter()
        {
            state->waker.reset();
            if (log)
                log->push_back("gate leaf destroyed");
        }

        nix_poll pollLeaf(const Waker & waker) override
        {
            if (state->open)
                return NIX_POLL_READY;
            state->waker = waker;
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    Task<void> wait(FiberLog * log = nullptr)
    {
        co_await Awaiter(state, log);
    }

    template<typename T>
    Task<T> waitThen(T value)
    {
        co_await Awaiter(state, nullptr);
        co_return value;
    }
};

/**
 * A leaf that is pending on its first poll and wakes its waker at once.
 */
struct YieldOnce : LeafAwaiter
{
    bool yielded = false;

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (yielded)
            return NIX_POLL_READY;
        yielded = true;
        waker.wakeByRef();
        return NIX_POLL_PENDING;
    }

    void await_resume() noexcept {}
};

struct WakeCounter : WakeTarget
{
    int count = 0;

    void wake() noexcept override
    {
        count++;
    }
};

struct FiberGuard
{
    FiberLog & log;
    std::string name;

    ~FiberGuard()
    {
        log.push_back(name + " destroyed");
    }
};

MakeError(FiberTestError, Error);

void FiberTestError::anchor() {}

} // namespace

TEST(Fiber, suspendsAndResumes)
{
    FiberLog log;
    Fiber fiber([&]() {
        log.push_back("start");
        Fiber::suspend();
        log.push_back("resumed");
    });
    EXPECT_EQ(log, FiberLog{});
    EXPECT_FALSE(fiber.resume());
    EXPECT_EQ(log, FiberLog{"start"});
    EXPECT_TRUE(fiber.resume());
    EXPECT_EQ(log, (FiberLog{"start", "resumed"}));
    EXPECT_EQ(fiber.exception(), nullptr);
    EXPECT_EQ(Fiber::current(), nullptr);
}

TEST(Fiber, stacksReturnToThePool)
{
    auto before = fiberStackStats();
    {
        Fiber fiber([]() { Fiber::suspend(); });
        fiber.resume();
        EXPECT_EQ(fiberStackStats().inUse, before.inUse + 1);
        fiber.resume();
    }
    auto after = fiberStackStats();
    EXPECT_EQ(after.inUse, before.inUse);

    /* A second fiber reuses the stack of the first. */
    {
        Fiber fiber([]() {});
        fiber.resume();
    }
    EXPECT_EQ(fiberStackStats().allocated, after.allocated);
}

TEST(RunOnFiber, isLazyAndReturnsTheResult)
{
    FiberLog log;
    auto task = runOnFiber([&]() {
        log.push_back("body");
        return 42;
    });
    EXPECT_EQ(log, FiberLog{});
    EXPECT_EQ(runBlocking(std::move(task)), 42);
    EXPECT_EQ(log, FiberLog{"body"});
}

TEST(RunOnFiber, suspendsOnAPendingTaskAndForwardsTheWaker)
{
    Gate gate;
    FiberLog log;
    auto task = runOnFiber([&]() {
        log.push_back("before");
        auto v = awaitOnFiber(gate.waitThen(std::string("fetched")));
        log.push_back("after " + v);
        return v + "!";
    });

    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(log, FiberLog{"before"});
    EXPECT_TRUE(gate.waiting());

    /* A poll without a wake does not resume the code after the await. */
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(target->count, 0);

    gate.open();
    EXPECT_EQ(target->count, 1);
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(log, (FiberLog{"before", "after fetched"}));
    EXPECT_EQ(task.takeResult(), "fetched!");
}

TEST(RunOnFiber, anExceptionOfTheAwaitedTaskReachesTheFiberAsTheSameObject)
{
    std::exception_ptr thrown;
    const void * caught = nullptr;
    auto task = runOnFiber([&]() {
        try {
            awaitOnFiber([](std::exception_ptr & thrown) -> Task<void> {
                co_await YieldOnce{};
                try {
                    throw FiberTestError("from the task");
                } catch (...) {
                    thrown = std::current_exception();
                    throw;
                }
            }(thrown));
        } catch (FiberTestError & e) {
            caught = &e;
        }
    });
    runBlocking(std::move(task));
    ASSERT_TRUE(thrown);
    try {
        std::rethrow_exception(thrown);
    } catch (FiberTestError & e) {
        EXPECT_EQ(caught, &e);
    }
}

TEST(RunOnFiber, anExceptionOfTheFiberReachesTheAwaiterAsTheSameObject)
{
    const void * thrown = nullptr;
    auto task = runOnFiber([&]() {
        awaitOnFiber([]() -> Task<void> { co_await YieldOnce{}; }());
        FiberTestError e("from the fiber");
        try {
            throw e;
        } catch (FiberTestError & e2) {
            thrown = &e2;
            throw;
        }
    });
    try {
        runBlocking(std::move(task));
        FAIL() << "no exception";
    } catch (FiberTestError & e) {
        EXPECT_EQ(&e, thrown);
    }
}

TEST(RunOnFiber, dropCancelsTheFiberAndTheTaskItAwaits)
{
    Gate gate;
    FiberLog log;
    auto inUseBefore = fiberStackStats().inUse;
    {
        auto task = runOnFiber([&]() {
            FiberGuard guard{log, "frame"};
            try {
                awaitOnFiber(gate.wait(&log));
                log.push_back("not reached");
            } catch (Interrupted & e) {
                log.push_back("interrupted");
                throw;
            }
        });
        auto target = makeWakeTarget<WakeCounter>();
        EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
        EXPECT_EQ(fiberStackStats().inUse, inUseBefore + 1);
    }
    EXPECT_EQ(log, (FiberLog{"gate leaf destroyed", "interrupted", "frame destroyed"}));
    EXPECT_FALSE(gate.waiting());
    EXPECT_EQ(fiberStackStats().inUse, inUseBefore);
}

TEST(RunOnFiber, aCaughtCancellationThrowsAgainOnTheNextAwait)
{
    FiberLog log;
    Gate gate;
    {
        auto task = runOnFiber([&]() {
            try {
                awaitOnFiber(gate.wait());
            } catch (Interrupted &) {
                log.push_back("caught");
            }
            try {
                awaitOnFiber(gate.wait());
            } catch (Interrupted &) {
                log.push_back("caught again");
            }
        });
        auto target = makeWakeTarget<WakeCounter>();
        EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    }
    EXPECT_EQ(log, (FiberLog{"caught", "caught again"}));
}

TEST(RunOnFiber, twoFibersInterleaveOnOneThread)
{
    Gate gateA, gateB;
    FiberLog log;
    std::vector<Task<std::string>> tasks;
    tasks.push_back(runOnFiber([&]() {
        log.push_back("a waits");
        auto v = awaitOnFiber(gateA.waitThen(std::string("a")));
        log.push_back("a resumed");
        return v;
    }));
    tasks.push_back(runOnFiber([&]() {
        log.push_back("b waits");
        auto v = awaitOnFiber(gateB.waitThen(std::string("b")));
        log.push_back("b resumed");
        return v;
    }));
    auto all = whenAll(std::move(tasks));

    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(log, (FiberLog{"a waits", "b waits"}));
    gateB.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    gateA.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(log, (FiberLog{"a waits", "b waits", "b resumed", "a resumed"}));
    EXPECT_EQ(all.takeResult(), (std::vector<std::string>{"a", "b"}));
}

TEST(RunOnFiber, eachFiberHasItsOwnCurrentActivity)
{
    Gate gateA, gateB;
    std::vector<ActivityId> seen;
    auto outer = getCurActivity();
    std::vector<Task<void>> tasks;
    tasks.push_back(runOnFiber([&]() {
        PushActivity pa(1001);
        awaitOnFiber(gateA.wait());
        seen.push_back(getCurActivity());
    }));
    tasks.push_back(runOnFiber([&]() {
        PushActivity pa(1002);
        awaitOnFiber(gateB.wait());
        seen.push_back(getCurActivity());
    }));
    auto all = whenAll(std::move(tasks));
    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(getCurActivity(), outer);
    gateA.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    gateB.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(seen, (std::vector<ActivityId>{1001, 1002}));
    EXPECT_EQ(getCurActivity(), outer);
}

namespace {

thread_local int fiberTestTls = 0;

/**
 * Not inlined, so that the caller cannot know that it switches stacks.
 */
[[gnu::noinline]] int * fiberTestTlsAddress()
{
    return &fiberTestTls;
}

} // namespace

/**
 * A fiber that reads a `thread_local` before and after a suspension sees
 * the same variable, and the writes of another fiber on the same thread
 * in between. This is the case that `Fiber::resume` enforces, by refusing
 * to resume a fiber on another thread.
 */
TEST(RunOnFiber, threadLocalsAreStableAcrossASuspensionOnOneThread)
{
    Gate gateA, gateB;
    int * before = nullptr;
    int * after = nullptr;
    int valueAfter = 0;
    std::vector<Task<void>> tasks;
    tasks.push_back(runOnFiber([&]() {
        fiberTestTls = 1;
        before = &fiberTestTls;
        awaitOnFiber(gateA.wait());
        after = &fiberTestTls;
        valueAfter = fiberTestTls;
    }));
    tasks.push_back(runOnFiber([&]() {
        awaitOnFiber(gateB.wait());
        *fiberTestTlsAddress() = 2;
    }));
    auto all = whenAll(std::move(tasks));
    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    gateB.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_PENDING);
    gateA.open();
    EXPECT_EQ(all.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(before, fiberTestTlsAddress());
    EXPECT_EQ(after, fiberTestTlsAddress());
    EXPECT_EQ(valueAfter, 2);
}

TEST(AwaitOnFiber, blocksOutsideOfAFiber)
{
    EXPECT_EQ(
        awaitOnFiber([]() -> Task<int> {
            co_await YieldOnce{};
            co_return 7;
        }()),
        7);
}

TEST(AwaitOnFiber, blocksInACatchBlock)
{
    /* In a `catch` block, the fiber does not suspend: the first poll of
       the outer task completes. */
    auto task = runOnFiber([&]() {
        try {
            throw FiberTestError("handled");
        } catch (FiberTestError &) {
            return awaitOnFiber([]() -> Task<int> {
                co_await YieldOnce{};
                co_return 8;
            }());
        }
    });
    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(task.takeResult(), 8);
}

TEST(RunOnFiber, nestedFibers)
{
    Gate gate;
    auto task = runOnFiber(
        [&]() { return awaitOnFiber(runOnFiber([&]() { return awaitOnFiber(gate.waitThen(5)) + 1; })) + 1; });
    auto target = makeWakeTarget<WakeCounter>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
    gate.open();
    EXPECT_EQ(target->count, 1);
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    EXPECT_EQ(task.takeResult(), 7);
}

} // namespace nix

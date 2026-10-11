#include <gtest/gtest.h>

#include "nix/expr/tests/libexpr.hh"
#include "nix/expr/tests/gc.hh"
#include "nix/util/fiber.hh"

#include <array>
#include <vector>

#if NIX_USE_BOEHMGC

namespace nix {

namespace {

/**
 * A task that completes when the test calls `open()`.
 */
struct GcGate
{
    bool isOpen = false;
    Waker waker;

    struct Awaiter : LeafAwaiter
    {
        GcGate & gate;

        explicit Awaiter(GcGate & gate)
            : gate(gate)
        {
        }

        nix_poll pollLeaf(const Waker & w) override
        {
            if (gate.isOpen)
                return NIX_POLL_READY;
            gate.waker = w;
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    Task<void> wait()
    {
        co_await Awaiter(*this);
    }

    void open()
    {
        isOpen = true;
        std::move(waker).wake();
    }
};

struct NoWake : WakeTarget
{
    void wake() noexcept override {}
};

/**
 * Links that the collector clears when their objects are collected. The
 * links are in memory that the collector does not scan (`std::vector`
 * uses `malloc`), so they do not keep the objects alive.
 */
struct Links
{
    std::vector<void *> links;

    explicit Links(size_t n)
        : links(n, nullptr)
    {
    }

    void track(size_t i, Value * v)
    {
        links[i] = v;
        ASSERT_EQ(GC_general_register_disappearing_link(&links[i], v), GC_SUCCESS);
    }

    size_t alive() const
    {
        size_t n = 0;
        for (auto l : links)
            n += l != nullptr;
        return n;
    }

    ~Links()
    {
        for (auto & l : links)
            GC_unregister_disappearing_link(&l);
    }
};

/**
 * Allocate `n` Values that nothing refers to. With `links`, the links
 * track them.
 */
void allocateGarbage(EvalState & state, size_t n, Links * links = nullptr)
{
    for (size_t i = 0; i < n; ++i) {
        auto v = state.allocValue();
        v->mkInt(-1);
        if (links && i < links->links.size())
            links->track(i, v);
    }
}

constexpr size_t keptValues = 1000;

} // namespace

class FiberGCTest : public LibExprTest
{};

/**
 * A fiber allocates many Values and keeps 1000 of them, only on its own
 * stack. While it is suspended, the collector runs from outside. The
 * Values survive, keep their contents, and the memory of the garbage is
 * reused.
 */
TEST_F(FiberGCTest, valuesOnASuspendedFiberSurviveACollection)
{
    if (auto reason = fibersDisabledReason())
        GTEST_SKIP() << *reason;

    GcGate gate;
    Links kept(keptValues);
    std::vector<int64_t> seen;

    auto task = runOnFiber([&]() {
        std::array<Value *, keptValues> values;
        for (size_t i = 0; i < keptValues; ++i) {
            /* Garbage between the kept Values, so that the collector frees
               the memory around them. */
            allocateGarbage(state, 100);
            values[i] = state.allocValue();
            values[i]->mkInt(int64_t(i));
            kept.track(i, values[i]);
        }

        awaitOnFiber(gate.wait());

        for (auto v : values)
            seen.push_back(v->integer().value);
    });

    auto target = makeWakeTarget<NoWake>();
    ASSERT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    /* Values that nothing refers to, made on another thread so that no
       stack or register of this thread holds them. They show that the
       collections below collect. */
    Links control(100);
    runOnGCThread([&]() { allocateGarbage(state, control.links.size(), &control); });

    auto cyclesBefore = getGCCycles();
    GC_gcollect();
    /* Reuse the memory that the collection freed. */
    allocateGarbage(state, 200000);
    GC_gcollect();
    EXPECT_GE(getGCCycles(), cyclesBefore + 2);

    EXPECT_EQ(control.alive(), 0u);
    EXPECT_EQ(kept.alive(), keptValues);

    gate.open();
    ASSERT_EQ(task.poll(target->waker()), NIX_POLL_READY);
    task.takeResult();

    std::vector<int64_t> expected;
    for (size_t i = 0; i < keptValues; ++i)
        expected.push_back(int64_t(i));
    EXPECT_EQ(seen, expected);
}

/**
 * The collector runs while a fiber runs, from an allocation on the fiber.
 * A Value that only the stack of the thread refers to (the stack that the
 * thread left for the fiber) survives.
 */
TEST_F(FiberGCTest, valuesOnTheThreadStackSurviveACollectionOnAFiber)
{
    if (auto reason = fibersDisabledReason())
        GTEST_SKIP() << *reason;

    Links kept(keptValues);
    std::array<Value *, keptValues> values;
    for (size_t i = 0; i < keptValues; ++i) {
        values[i] = state.allocValue();
        values[i]->mkInt(int64_t(i));
        kept.track(i, values[i]);
    }

    auto cyclesBefore = getGCCycles();
    runBlocking(runOnFiber([&]() {
        GC_gcollect();
        allocateGarbage(state, 200000);
        GC_gcollect();
    }));
    EXPECT_GE(getGCCycles(), cyclesBefore + 2);

    EXPECT_EQ(kept.alive(), keptValues);
    std::vector<int64_t> seen, expected;
    for (size_t i = 0; i < keptValues; ++i) {
        seen.push_back(values[i]->integer().value);
        expected.push_back(int64_t(i));
    }
    EXPECT_EQ(seen, expected);
}

/**
 * A fiber that ended leaves its stack in the pool. The collector does not
 * scan it, so the Values that it referred to are collected.
 */
TEST_F(FiberGCTest, aFreeStackIsNotScanned)
{
    if (auto reason = fibersDisabledReason())
        GTEST_SKIP() << *reason;

    Links dead(100);
    runOnGCThread([&]() {
        runBlocking(runOnFiber([&]() {
            std::array<Value *, 100> values;
            for (size_t i = 0; i < values.size(); ++i) {
                values[i] = state.allocValue();
                values[i]->mkInt(int64_t(i));
                dead.track(i, values[i]);
            }
            /* Keep the array on the stack until the end. */
            asm volatile("" : : "r"(values.data()) : "memory");
        }));
    });
    EXPECT_GE(fiberStackStats().free, 1u);
    GC_gcollect();
    EXPECT_EQ(dead.alive(), 0u);
}

} // namespace nix

#endif

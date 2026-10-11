/**
 * Measure evaluations on fibers (`runOnFiber`).
 *
 * Usage:
 *
 *   nix-expr-fiber-bench memory N DEPTH
 *     Start N evaluations, each on its fiber, that suspend at a depth of
 *     DEPTH calls of a Nix function, and report the growth of the
 *     resident memory and of the number of memory mappings per fiber.
 *
 *   nix-expr-fiber-bench cancel N DEPTH
 *     N times: start one such evaluation, and measure how long the drop
 *     of its task takes, which unwinds the fiber.
 *
 *   nix-expr-fiber-bench switch N
 *     Suspend and resume one fiber N times, and compare the time per
 *     round trip with the same wait in a coroutine task.
 *
 * The evaluations suspend in a primop that awaits a task that the
 * program completes; there is no I/O.
 */

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/fiber.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>

#include <unistd.h>

using namespace nix;

namespace {

using Clock = std::chrono::steady_clock;

/**
 * Wakers of the evaluations that wait; `openAll()` completes them.
 */
struct BenchGate
{
    bool open = false;
    std::vector<Waker> wakers;

    struct Awaiter : LeafAwaiter
    {
        BenchGate & gate;

        explicit Awaiter(BenchGate & gate)
            : gate(gate)
        {
        }

        nix_poll pollLeaf(const Waker & waker) override
        {
            if (gate.open)
                return NIX_POLL_READY;
            gate.wakers.push_back(waker);
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    Task<void> wait()
    {
        co_await Awaiter(*this);
    }

    void openAll()
    {
        open = true;
        for (auto & w : wakers)
            std::move(w).wake();
        wakers.clear();
    }
};

/**
 * Pending on the first poll; wakes its waker at once.
 */
struct BenchYield : LeafAwaiter
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

struct NoopWake : WakeTarget
{
    void wake() noexcept override {}
};

size_t residentBytes()
{
    std::ifstream statm("/proc/self/statm");
    size_t size, resident;
    statm >> size >> resident;
    return resident * sysconf(_SC_PAGESIZE);
}

size_t mappings()
{
    std::ifstream maps("/proc/self/maps");
    size_t n = 0;
    std::string line;
    while (std::getline(maps, line))
        n++;
    return n;
}

struct Bench
{
    ref<Store> store = openStore("dummy://");
    fetchers::Settings fetchSettings;
    bool readOnlyMode = true;
    EvalSettings evalSettings = [this]() {
        EvalSettings settings{readOnlyMode};
        /* The call depth is a counter of the `EvalState`, which the
           suspended evaluations share: their depths add up. */
        settings.maxCallDepth = 1u << 30;
        return settings;
    }();
    EvalState state{{}, store, fetchSettings, evalSettings, nullptr};

    BenchGate gate;
    PrimOp waitPrimOp{
        .name = "wait",
        .arity = 1,
        .impl =
            [this](EvalState & st, CallSite, Value * const * args, Value & v) {
                awaitOnFiber(gate.wait());
                v.mkInt(0);
            },
    };
    Value * waitV = state.allocValue();
    Value * fn = state.allocValue();

    Bench()
    {
        waitV->mkPrimOp(&waitPrimOp);
        state.eval(
            state.parseExprFromString(
                "wait: depth: let f = n: if n == 0 then wait null else 1 + f (n - 1); in f depth",
                state.rootPath(CanonPath::root)),
            *fn);
    }

    /**
     * An evaluation that suspends at `depth` calls deep, and returns
     * `depth` when it resumes.
     */
    Task<NixInt::Inner> evaluation(int64_t depth)
    {
        return runOnFiber([this, depth]() {
            Value withWait;
            state.callFunction(*fn, *waitV, withWait, noPos);
            Value depthV;
            depthV.mkInt(depth);
            Value r;
            state.callFunction(withWait, depthV, r, noPos);
            state.forceValue(r, noPos);
            return r.integer().value;
        });
    }
};

int memory(size_t n, int64_t depth)
{
    Bench bench;
    auto waker = makeWakeTarget<NoopWake>()->waker();

    /* Warm up: the pool, the caches of the evaluator, the heap. */
    {
        auto t = bench.evaluation(depth);
        t.poll(waker);
        bench.gate.openAll();
        t.poll(waker);
        bench.gate.open = false;
    }
    GC_gcollect();

    auto rss0 = residentBytes();
    auto maps0 = mappings();
    auto stacks0 = fiberStackStats();
    std::vector<Task<NixInt::Inner>> tasks;
    for (size_t i = 0; i < n; ++i) {
        tasks.push_back(bench.evaluation(depth));
        if (tasks.back().poll(waker) != NIX_POLL_PENDING) {
            auto r = tasks.back().takeResult();
            throw Error("evaluation %d did not suspend, but returned %d", i, r);
        }
    }
    auto rss1 = residentBytes();
    auto maps1 = mappings();
    auto stacks1 = fiberStackStats();

    bench.gate.openAll();
    for (auto & t : tasks)
        if (t.poll(waker) != NIX_POLL_READY || t.takeResult() != depth)
            throw Error("an evaluation did not complete with %d", depth);

    std::cout << fmt(
        "%d fibers suspended at depth %d: resident memory +%d KiB (%d KiB per fiber), "
        "mappings +%d (%.1f per fiber), stacks allocated %d -> %d, virtual %d MiB per stack\n",
        n,
        depth,
        (rss1 - rss0) / 1024,
        (rss1 - rss0) / 1024 / n,
        maps1 - maps0,
        double(maps1 - maps0) / n,
        stacks0.allocated,
        stacks1.allocated,
        fiberStackSize / 1024 / 1024);
    return 0;
}

int cancel(size_t n, int64_t depth)
{
    Bench bench;
    auto waker = makeWakeTarget<NoopWake>()->waker();
    std::vector<double> us;
    for (size_t i = 0; i < n; ++i) {
        auto t = bench.evaluation(depth);
        if (t.poll(waker) != NIX_POLL_PENDING)
            throw Error("the evaluation did not suspend");
        auto start = Clock::now();
        t.reset();
        us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        bench.gate.wakers.clear();
    }
    std::sort(us.begin(), us.end());
    std::cout << fmt(
        "dropping a suspended evaluation at depth %d, %d times: min %.1f us, median %.1f us, max %.1f us\n",
        depth,
        n,
        us.front(),
        us[us.size() / 2],
        us.back());
    return 0;
}

int switches(size_t n)
{
    auto onFiber = [n]() {
        return runOnFiber([n]() {
            for (size_t i = 0; i < n; ++i)
                awaitOnFiber([]() -> Task<void> { co_await BenchYield{}; }());
        });
    };
    auto inTask = [n]() -> Task<void> {
        for (size_t i = 0; i < n; ++i)
            co_await []() -> Task<void> { co_await BenchYield{}; }();
    };

    auto time = [](Task<void> t) {
        auto waker = makeWakeTarget<NoopWake>()->waker();
        auto start = Clock::now();
        while (t.poll(waker) != NIX_POLL_READY)
            ;
        return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    };

    time(onFiber());
    auto fiberNs = time(onFiber());
    auto taskNs = time(inTask());
    std::cout << fmt(
        "%d suspensions: %.0f ns per round trip on a fiber, %.0f ns in a coroutine task\n", n, fiberNs / n, taskNs / n);
    return 0;
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        initLibStore();
        initGC();
        if (auto reason = fibersDisabledReason())
            throw Error("fibers are disabled: %s", *reason);

        std::vector<std::string> args(argv + 1, argv + argc);
        if (args.size() == 3 && args[0] == "memory")
            return memory(std::stoull(args[1]), std::stoll(args[2]));
        if (args.size() == 3 && args[0] == "cancel")
            return cancel(std::stoull(args[1]), std::stoll(args[2]));
        if (args.size() == 2 && args[0] == "switch")
            return switches(std::stoull(args[1]));
        std::cerr << "usage: nix-expr-fiber-bench memory N DEPTH | cancel N DEPTH | switch N\n";
        return 2;
    } catch (std::exception & e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

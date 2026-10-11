#include "nix/util/closure.hh"
#include "nix/util/error.hh"

#include <gtest/gtest.h>

#include <map>

namespace nix {

namespace {

std::map<std::string, std::set<std::string>> testGraph = {
    {"A", {"B", "C", "G"}},
    {"B", {"A"}}, // Loops back to A
    {"C", {"F"}}, // Indirect reference
    {"D", {"A"}}, // Not reachable, but has backreferences
    {"E", {}},    // Just not reachable
    {"F", {}},
    {"G", {"G"}}, // Self reference
};

using Log = std::vector<std::string>;

MakeError(ClosureTestError, Error);

void ClosureTestError::anchor() {}

/**
 * A waker that counts how often it is woken.
 */
struct ClosureWakeCounter : WakeTarget
{
    std::atomic<int> count = 0;

    void wake() noexcept override
    {
        count++;
    }
};

/**
 * Edges that complete only when the test releases them, so that the test
 * controls the order of completion and can see which edges run at the
 * same time.
 */
struct ManualEdges
{
    struct Pending
    {
        bool released = false;
        Waker waker;
    };

    Log & log;
    /**
     * Nodes whose edges throw instead of returning.
     */
    std::set<std::string> failing = {};
    std::map<std::string, std::shared_ptr<Pending>> pending = {};

    struct Leaf : LeafAwaiter
    {
        std::shared_ptr<Pending> p;
        Log & log;
        std::string node;

        Leaf(std::shared_ptr<Pending> p, Log & log, std::string node)
            : p(std::move(p))
            , log(log)
            , node(std::move(node))
        {
        }

        Leaf(const Leaf &) = delete;

        ~Leaf()
        {
            /* The edge no longer wakes anything. */
            p->waker.reset();
            if (!p->released)
                log.push_back(node + " cancelled");
        }

        nix_poll pollLeaf(const Waker & waker) override
        {
            if (p->released)
                return NIX_POLL_READY;
            p->waker = waker;
            return NIX_POLL_PENDING;
        }

        void await_resume() noexcept {}
    };

    GetEdgesTask<std::string> getEdges()
    {
        return [this](const std::string & node) -> Task<std::set<std::string>> {
            log.push_back(node + " started");
            auto p = std::make_shared<Pending>();
            pending.insert_or_assign(node, p);
            co_await Leaf(p, log, node);
            if (failing.contains(node))
                throw ClosureTestError("no edges for " + node);
            co_return testGraph.at(node);
        };
    }

    void release(const std::string & node)
    {
        auto & p = *pending.at(node);
        p.released = true;
        std::move(p.waker).wake();
    }
};

GetEdgesTask<std::string> immediateEdges()
{
    return [](const std::string & node) -> Task<std::set<std::string>> { co_return testGraph.at(node); };
}

} // namespace

TEST(closure, correctClosure)
{
    EXPECT_EQ(
        runBlocking(computeClosure<std::string>({"A"}, immediateEdges())),
        (std::set<std::string>{"A", "B", "C", "F", "G"}));
}

TEST(closure, emptyStart)
{
    EXPECT_EQ(runBlocking(computeClosure<std::string>({}, immediateEdges())), std::set<std::string>{});
}

TEST(closure, knownElementsAreNotTraversed)
{
    Log log;
    auto getEdges = [&](const std::string & node) -> Task<std::set<std::string>> {
        log.push_back(node);
        co_return testGraph.at(node);
    };

    /* C is known, so F is not reached through it. */
    EXPECT_EQ(
        runBlocking(computeClosure<std::string>({"A"}, getEdges, {"C"})), (std::set<std::string>{"A", "B", "C", "G"}));
    EXPECT_EQ(log, (Log{"A", "B", "G"}));
}

TEST(closure, boundedConcurrency)
{
    Log log;
    ManualEdges edges{log};
    auto task = computeClosure<std::string>({"A", "D", "E"}, edges.getEdges(), {}, 2);

    auto target = makeWakeTarget<ClosureWakeCounter>();
    auto waker = target->waker();

    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"A started", "D started"}));

    edges.release("D");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"A started", "D started", "E started"}));

    /* A finds B, C and G. Only B fits next to E. */
    edges.release("A");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"A started", "D started", "E started", "B started"}));

    edges.release("E");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("B");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("G");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("C");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("F");
    EXPECT_EQ(task.poll(waker), NIX_POLL_READY);

    EXPECT_EQ(task.takeResult(), (std::set<std::string>{"A", "B", "C", "D", "E", "F", "G"}));
    EXPECT_EQ(
        log,
        (Log{
            "A started",
            "D started",
            "E started",
            "B started",
            "C started",
            "G started",
            "F started",
        }));
}

TEST(closure, dropInTheMiddleCancelsRunningEdges)
{
    Log log;
    ManualEdges edges{log};
    auto task = computeClosure<std::string>({"A"}, edges.getEdges());

    auto target = makeWakeTarget<ClosureWakeCounter>();
    auto waker = target->waker();

    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("A");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"A started", "B started", "C started", "G started"}));

    edges.release("B");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);

    task.reset();
    EXPECT_EQ(log.size(), 6u);
    /* The order in which the set destroys its tasks is unspecified. */
    EXPECT_EQ(
        std::multiset<std::string>(log.begin() + 4, log.end()),
        (std::multiset<std::string>{"C cancelled", "G cancelled"}));

    /* Nothing runs after the drop. */
    auto before = target->count.load();
    edges.release("G");
    EXPECT_EQ(target->count, before);
}

TEST(closure, edgeErrorCancelsOtherEdges)
{
    Log log;
    ManualEdges edges{log, {"C"}};
    auto task = computeClosure<std::string>({"A"}, edges.getEdges());

    auto target = makeWakeTarget<ClosureWakeCounter>();
    auto waker = target->waker();

    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    edges.release("A");
    EXPECT_EQ(task.poll(waker), NIX_POLL_PENDING);
    EXPECT_EQ(log, (Log{"A started", "B started", "C started", "G started"}));

    edges.release("C");
    EXPECT_EQ(task.poll(waker), NIX_POLL_READY);
    EXPECT_EQ(log.size(), 6u);
    EXPECT_EQ(
        std::multiset<std::string>(log.begin() + 4, log.end()),
        (std::multiset<std::string>{"B cancelled", "G cancelled"}));

    try {
        task.takeResult();
        FAIL() << "expected an exception";
    } catch (ClosureTestError & e) {
        EXPECT_EQ(e.info().msg.str(), "no edges for C");
    }
}

TEST(closure, blockingErrorFromSynchronousEdges)
{
    size_t callCount = 0;
    auto getEdges = [&](const std::string & node) -> Task<std::set<std::string>> {
        if (callCount++ == 0)
            throw ClosureTestError("first edge fails");
        co_return testGraph.at(node);
    };

    EXPECT_THROW(runBlocking(computeClosure<std::string>({"A", "B"}, getEdges)), ClosureTestError);
    /* The first edge to run throws before the second one runs: the
       set polls its tasks in order and stops at the first that
       completes. */
    EXPECT_EQ(callCount, 1u);
}

} // namespace nix

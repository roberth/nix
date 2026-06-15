#include <gtest/gtest.h>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>

#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/expr/tracing-environment.hh"
#include "nix/expr/environment/system.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

/**
 * Environment wrapper that overrides specific env vars for testing.
 */
class OverrideEnvEnvironment : public Environment
{
    ref<Environment> inner;
    std::map<std::string, std::optional<std::string>> overrides;

public:
    OverrideEnvEnvironment(ref<Environment> inner, std::map<std::string, std::optional<std::string>> overrides)
        : inner(inner)
        , overrides(std::move(overrides))
    {
    }

    ref<SourceAccessor> fsRoot() override
    {
        return inner->fsRoot();
    }

    std::optional<std::string> getEnv(const std::string & name) override
    {
        auto it = overrides.find(name);
        if (it != overrides.end())
            return it->second;
        return inner->getEnv(name);
    }
};

/**
 * In-memory trace sink that collects JSON entries (still needed for the
 * JSON side of TracingWriter).
 */
class CollectingTraceSink : public TraceSink
{
public:
    std::vector<nlohmann::json> entries;

    void log(const nlohmann::json & entry) override
    {
        entries.push_back(entry);
    }
};

class TracingReplayTest : public LibStoreTest
{
protected:
    std::shared_ptr<EvalState> state;
    bool readOnlyMode = false;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};
    std::filesystem::path dbPath;
    std::shared_ptr<SystemEnvironment> defaultEnv;
    std::shared_ptr<CollectingTraceSink> replaySink = std::make_shared<CollectingTraceSink>();
    std::shared_ptr<TracingWriter> replayWriter = std::make_shared<TracingWriter>(*replaySink);

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    TracingReplayTest()
        : LibStoreTest(openStore("dummy://?read-only=false"))
    {
    }

    void SetUp() override
    {
        defaultEnv = make_ref<SystemEnvironment>(evalSettings, store);
        auto stateRef = make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
        state = stateRef;

        // Each test gets fresh temporary databases
        auto tmpDir = std::filesystem::temp_directory_path();
        auto suffix = std::to_string(getpid());
        dbPath = tmpDir / ("nix-test-trie-" + suffix + ".sqlite");
        std::filesystem::remove(dbPath);
    }

    void TearDown() override
    {
        std::filesystem::remove(dbPath);
    }

    ref<EvalState> makeState()
    {
        return make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
    }

    ref<EvalState> makeStateWithEnv(ref<Environment> env)
    {
        auto sysEnv = make_ref<SystemEnvironment>(evalSettings, store);
        return make_ref<EvalState>(LookupPath{}, fetchSettings, evalSettings, env, sysEnv);
    }

    /**
     * Record a trace into a TracingIndex by evaluating through TracingEvaluator
     * with trie recording enabled.
     */
    void recordToIndex(TracingIndex & index, std::function<void(Evaluator &)> work)
    {
        auto sink = std::make_shared<CollectingTraceSink>();
        TracingWriter writer(*sink, &index);
        auto interpreter = make_ref<Interpreter>(makeState());
        TracingEvaluator tracing(writer, interpreter);
        work(tracing);
    }

    /**
     * Record a trace using a custom Environment (wrapped in TracingEnvironment).
     */
    void recordToIndexWithEnv(TracingIndex & index, ref<Environment> env, std::function<void(Evaluator &)> work)
    {
        auto sink = std::make_shared<CollectingTraceSink>();
        TracingWriter writer(*sink, &index);
        auto tracingEnv = make_ref<TracingEnvironment>(env, writer);
        auto innerState = makeStateWithEnv(tracingEnv);
        auto interpreter = make_ref<Interpreter>(innerState);
        TracingEvaluator tracing(writer, interpreter);
        work(tracing);
    }

    /**
     * Create a TracingReplayEvaluator that replays from the given index.
     */
    ref<TracingReplayEvaluator> makeReplayEvaluator(TracingIndex & index)
    {
        auto interpreter = make_ref<Interpreter>(makeState());
        return make_ref<TracingReplayEvaluator>(interpreter, index, *defaultEnv, *replayWriter);
    }

    /**
     * Create a TracingReplayEvaluator with a custom Environment for validation.
     */
    ref<TracingReplayEvaluator> makeReplayEvaluatorWithEnv(TracingIndex & index, ref<Environment> env)
    {
        auto innerState = makeStateWithEnv(env);
        auto interpreter = make_ref<Interpreter>(innerState);
        return make_ref<TracingReplayEvaluator>(interpreter, index, *env, *replayWriter);
    }
};

TEST_F(TracingReplayTest, ReplayGetType)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootedPath(CanonPath::root));
        obj->getType();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootedPath(CanonPath::root));
    EXPECT_EQ(obj->getType(), nInt);
}

TEST_F(TracingReplayTest, ReplayGetInt)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootedPath(CanonPath::root));
        obj->getInt();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootedPath(CanonPath::root));
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayGetString)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("\"hello\"", state->rootedPath(CanonPath::root));
        obj->getStringIgnoreContext();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("\"hello\"", state->rootedPath(CanonPath::root));
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST_F(TracingReplayTest, ReplayGetBool)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("true", state->rootedPath(CanonPath::root));
        obj->getBool();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("true", state->rootedPath(CanonPath::root));
    EXPECT_TRUE(obj->getBool());
}

TEST_F(TracingReplayTest, ReplayGetAttr)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ x = 42; }", state->rootedPath(CanonPath::root));
        auto x = obj->maybeGetAttr("x");
        x->getInt();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ x = 42; }", state->rootedPath(CanonPath::root));
    auto x = obj->maybeGetAttr("x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayMissingAttr)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ }", state->rootedPath(CanonPath::root));
        obj->maybeGetAttr("nonexistent");
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ }", state->rootedPath(CanonPath::root));
    EXPECT_EQ(obj->maybeGetAttr("nonexistent"), nullptr);
}

TEST_F(TracingReplayTest, ReplayGetAttrNames)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ a = 1; b = 2; }", state->rootedPath(CanonPath::root));
        obj->getAttrNames();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ a = 1; b = 2; }", state->rootedPath(CanonPath::root));
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(TracingReplayTest, ReplayGetFloat)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("3.14", state->rootedPath(CanonPath::root));
        obj->getFloat();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("3.14", state->rootedPath(CanonPath::root));
    EXPECT_DOUBLE_EQ(obj->getFloat(), 3.14);
}

TEST_F(TracingReplayTest, ReplayGetListSize)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("[1 2 3]", state->rootedPath(CanonPath::root));
        obj->getListSize();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("[1 2 3]", state->rootedPath(CanonPath::root));
    EXPECT_EQ(obj->getListSize(), 3u);
}

TEST_F(TracingReplayTest, FallbackToInnerOnMiss)
{
    // Empty index — everything is a miss, so replay falls back to inner
    TracingIndex index(dbPath);

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootedPath(CanonPath::root));
    // evalExpr itself misses (no shortcut), so it delegates to inner evaluator
    // which returns a real Object, not a TracingReplayObject
    EXPECT_EQ(obj->getType(), nInt);
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST_F(TracingReplayTest, EnvVarValidationUsesEnvironment)
{
    // Set a real process env var, record a trace that includes it,
    // then replay with an Environment that overrides it to a different
    // value. Validation must go through Environment::getEnv — if it
    // uses std::getenv, it sees the unchanged process env and wrongly
    // considers the cache valid.
    TracingIndex index(dbPath);
    auto sysEnv = make_ref<SystemEnvironment>(evalSettings, store);

    // Set the real process env var so std::getenv returns "recorded"
    setenv("NIX_TEST_REPLAY_ENV", "recorded", 1);

    // Record: builtins.getEnv sees "recorded" via the Environment chain
    recordToIndexWithEnv(index, sysEnv, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("builtins.getEnv \"NIX_TEST_REPLAY_ENV\"", state->rootedPath(CanonPath::root));
        obj->getStringIgnoreContext();
    });

    // Replay with an Environment that overrides the var — but the
    // process env still has "recorded", so std::getenv would not
    // detect the change.
    auto replayEnv = make_ref<OverrideEnvEnvironment>(
        sysEnv, std::map<std::string, std::optional<std::string>>{{"NIX_TEST_REPLAY_ENV", "changed"}});

    auto replay = makeReplayEvaluatorWithEnv(index, replayEnv);
    auto obj = replay->evalExpr("builtins.getEnv \"NIX_TEST_REPLAY_ENV\"", state->rootedPath(CanonPath::root));

    // With the fix (validation via Environment): "changed" != "recorded"
    // → invalidation → inner evaluator returns "changed".
    // Without the fix (std::getenv): "recorded" == "recorded"
    // → cache hit → stale value "recorded".
    EXPECT_EQ(obj->getStringIgnoreContext(), "changed");

    unsetenv("NIX_TEST_REPLAY_ENV");
}

// -----------------------------------------------------------------------------
// Sets-based index tests
// -----------------------------------------------------------------------------

namespace {

TracingIndex::SetMember makeMember(std::string_view qSeed, std::string_view rSeed)
{
    return TracingIndex::SetMember{
        .queryHash = hashString(HashAlgorithm::SHA256, qSeed),
        .responseHash = hashString(HashAlgorithm::SHA256, rSeed),
    };
}

TracingIndex::SetMembers makeSorted(std::vector<TracingIndex::SetMember> members)
{
    std::sort(members.begin(), members.end());
    return members;
}

} // namespace

TEST_F(TracingReplayTest, SetsIsSubsetEmpty)
{
    TracingIndex::SetMembers empty;
    auto a = makeSorted({makeMember("q1", "r1")});
    EXPECT_TRUE(TracingIndex::isSubset(empty, empty));
    EXPECT_TRUE(TracingIndex::isSubset(empty, a));
    EXPECT_FALSE(TracingIndex::isSubset(a, empty));
}

TEST_F(TracingReplayTest, SetsIsSubsetExactAndProper)
{
    auto a = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto b = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2"), makeMember("q3", "r3")});
    EXPECT_TRUE(TracingIndex::isSubset(a, a));
    EXPECT_TRUE(TracingIndex::isSubset(a, b));
    EXPECT_FALSE(TracingIndex::isSubset(b, a));
}

TEST_F(TracingReplayTest, SetsIsSubsetResponseMismatch)
{
    auto a = makeSorted({makeMember("q1", "r1")});
    auto b = makeSorted({makeMember("q1", "r1-different")});
    EXPECT_FALSE(TracingIndex::isSubset(a, b));
    EXPECT_FALSE(TracingIndex::isSubset(b, a));
}

TEST_F(TracingReplayTest, SetsIntersectEmpty)
{
    TracingIndex::SetMembers empty;
    auto a = makeSorted({makeMember("q1", "r1")});
    EXPECT_EQ(TracingIndex::intersectSets(empty, empty), empty);
    EXPECT_EQ(TracingIndex::intersectSets(empty, a), empty);
    EXPECT_EQ(TracingIndex::intersectSets(a, empty), empty);
}

TEST_F(TracingReplayTest, SetsIntersectKeepsCommon)
{
    auto a = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto b = makeSorted({makeMember("q1", "r1"), makeMember("q3", "r3")});
    auto expected = makeSorted({makeMember("q1", "r1")});
    EXPECT_EQ(TracingIndex::intersectSets(a, b), expected);
    EXPECT_EQ(TracingIndex::intersectSets(b, a), expected);
}

TEST_F(TracingReplayTest, SetsIntersectDropsConflictingResponses)
{
    /* Same queryHash, different responseHash on each side — that's
       contradictory; intersection drops both. */
    auto a = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto b = makeSorted({makeMember("q1", "r1-other"), makeMember("q2", "r2")});
    auto expected = makeSorted({makeMember("q2", "r2")});
    EXPECT_EQ(TracingIndex::intersectSets(a, b), expected);
}

TEST_F(TracingReplayTest, SetsIntersectFullOverlap)
{
    auto a = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    EXPECT_EQ(TracingIndex::intersectSets(a, a), a);
}

TEST_F(TracingReplayTest, SetsIsSubsetDisjoint)
{
    auto a = makeSorted({makeMember("q1", "r1")});
    auto b = makeSorted({makeMember("q2", "r2")});
    EXPECT_FALSE(TracingIndex::isSubset(a, b));
    EXPECT_FALSE(TracingIndex::isSubset(b, a));
}

TEST_F(TracingReplayTest, SetsRoundTripPreconditionAndResponse)
{
    TracingIndex index(dbPath);

    auto members = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto setHash = index.insertPreconditionSet(members);
    auto responseHash = index.insertSetResponse("the-payload");

    TracingIndex::flushAllWriteQueues();

    auto roundtripMembers = index.getPreconditionSet(setHash);
    ASSERT_TRUE(roundtripMembers.has_value());
    EXPECT_EQ(*roundtripMembers, members);

    auto roundtripPayload = index.getSetResponse(responseHash);
    ASSERT_TRUE(roundtripPayload.has_value());
    EXPECT_EQ(*roundtripPayload, "the-payload");
}

TEST_F(TracingReplayTest, SetsIdempotentInsert)
{
    TracingIndex index(dbPath);

    auto members = makeSorted({makeMember("q1", "r1")});
    auto h1 = index.insertPreconditionSet(members);
    auto h2 = index.insertPreconditionSet(members);
    EXPECT_EQ(h1, h2);

    auto r1 = index.insertSetResponse("same");
    auto r2 = index.insertSetResponse("same");
    EXPECT_EQ(r1, r2);
}

TEST_F(TracingReplayTest, SetsLookupHitAndMiss)
{
    TracingIndex index(dbPath);

    auto qh = hashString(HashAlgorithm::SHA256, "queryHash-A");
    auto precondition = makeSorted({makeMember("q1", "r1")});
    auto preconditionHash = index.insertPreconditionSet(precondition);
    auto responseHash = index.insertSetResponse("cached-value");
    index.insertBinding(qh, preconditionHash, responseHash);

    TracingIndex::flushAllWriteQueues();

    auto current = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto hit = index.lookupSetsReplay(qh, current);
    ASSERT_TRUE(hit.has_value()) << "lookup should hit when precondition is a subset of current";
    EXPECT_EQ(*hit, "cached-value");

    auto missing = makeSorted({makeMember("q2", "r2")});
    auto miss = index.lookupSetsReplay(qh, missing);
    EXPECT_FALSE(miss.has_value());

    auto wrongQh = hashString(HashAlgorithm::SHA256, "queryHash-B");
    auto wrongMiss = index.lookupSetsReplay(wrongQh, current);
    EXPECT_FALSE(wrongMiss.has_value());
}

TEST_F(TracingReplayTest, SetsRecordingViaTracingEvaluatorProducesBinding)
{
    /* End-to-end: run a recording through TracingEvaluator (which uses
       TracingWriter under the hood) and verify that at least one
       Binding lands in the sets-based index for the recorded Query.
       The simplest case: evalExpr "42" produces one top-level Query
       (evalExpr) with no d>0 events, so its precondition is empty. */
    TracingIndex index(dbPath);

    {
        auto sink = std::make_shared<CollectingTraceSink>();
        TracingWriter writer(*sink, &index);
        auto interpreter = make_ref<Interpreter>(makeState());
        TracingEvaluator tracing(writer, interpreter);
        auto obj = tracing.evalExpr("42", state->rootedPath(CanonPath::root));
        obj->getInt();
        // Capture the queryHash of evalExpr "42" so we can probe Bindings for it.
        // The query's structure is fixed; reproduce its hash via the same path.
    }

    TracingIndex::flushAllWriteQueues();

    /* Probe: lookupSetsReplay for an empty current context should
       succeed for any Binding whose precondition is empty. Iterate
       all known queryHashes is awkward without the queryHash itself,
       so instead just verify Bindings table is non-empty by trying
       lookupSetsReplay on the queryHashes appearing in Shortcuts. */
    auto state(index.selectShortcuts(hashString(HashAlgorithm::SHA256, "nonexistent")));
    // (We don't know the recorded queryHashes here without intercepting
    // TracingWriter; instead, verify indirectly: SELECT COUNT(*) > 0.)
    // The simplest direct check is to query SQLite via getPreconditionSet
    // on the well-known empty-set hash.
    TracingIndex::SetMembers empty;
    auto emptyHash = TracingIndex::computePreconditionSetHash(empty);
    auto roundtrip = index.getPreconditionSet(emptyHash);
    EXPECT_TRUE(roundtrip.has_value()) << "evalExpr should have produced at least one Binding "
                                          "with an empty precondition (i.e. inserted the empty set)";
}

TEST_F(TracingReplayTest, SetsLearningPassNarrowsPreconditions)
{
    /* Two Bindings for the same queryHash with overlapping preconditions
       but the same Response. Learning pass should insert a third Binding
       whose precondition is the intersection — narrower, so a current
       context that contains only the intersection now hits the cache. */
    TracingIndex index(dbPath);

    auto qh = hashString(HashAlgorithm::SHA256, "queryHash-learning");

    auto p1 = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto p2 = makeSorted({makeMember("q1", "r1"), makeMember("q3", "r3")});
    auto p1h = index.insertPreconditionSet(p1);
    auto p2h = index.insertPreconditionSet(p2);
    auto rh = index.insertSetResponse("shared-answer");
    index.insertBinding(qh, p1h, rh);
    index.insertBinding(qh, p2h, rh);

    /* Give the async writer time to commit. flushAllWriteQueues is
       destructive (it joins the writer thread, after which any
       further enqueue is dead-letter), so the test uses a small
       sleep instead and reserves the single flush for the very end. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    /* Before learning: a context containing only q1=r1 wouldn't match
       either P1 or P2 (P1 needs q2 too, P2 needs q3 too). */
    auto onlyShared = makeSorted({makeMember("q1", "r1")});
    EXPECT_FALSE(index.lookupSetsReplay(qh, onlyShared).has_value())
        << "before learning, intersection-only context shouldn't hit";

    /* Run learning. */
    auto inserted = index.runLearningPass(qh);
    ASSERT_EQ(inserted, 1u) << "exactly one new intersected Binding expected";

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    /* After learning: the intersection (q1=r1) is its own Binding. */
    auto hit = index.lookupSetsReplay(qh, onlyShared);
    ASSERT_TRUE(hit.has_value()) << "after learning, intersection-only context should hit";
    EXPECT_EQ(*hit, "shared-answer");

    /* Idempotent: running the pass again inserts nothing new. */
    EXPECT_EQ(index.runLearningPass(qh), 0u);
}

TEST_F(TracingReplayTest, SetsLearningPassEvictsSubsumedBindings)
{
    /* When two Bindings share a Response and one's precondition is
       a strict subset of the other's, the wider Binding is
       redundant — any hit it would serve is already served by the
       narrower one. The learning pass evicts it. */
    TracingIndex index(dbPath);

    auto qh = hashString(HashAlgorithm::SHA256, "queryHash-evict");

    auto pWide = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto pNarrow = makeSorted({makeMember("q1", "r1")});

    auto pwh = index.insertPreconditionSet(pWide);
    auto pnh = index.insertPreconditionSet(pNarrow);
    auto rh = index.insertSetResponse("shared-answer");
    index.insertBinding(qh, pwh, rh);
    index.insertBinding(qh, pnh, rh);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ASSERT_EQ(index.countBindings(qh), 2u) << "two Bindings pre-compact";

    /* Learning shouldn't insert anything new — the intersection of
       pWide and pNarrow is pNarrow itself, already a Binding. But
       it should evict pWide as subsumed. */
    auto inserted = index.runLearningPass(qh);
    EXPECT_EQ(inserted, 0u) << "no new Bindings to learn — narrow already exists";

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(index.countBindings(qh), 1u) << "wider Binding should be evicted";

    /* Verify lookup still works — same currents that hit pWide
       before now hit pNarrow. */
    auto current = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto hit = index.lookupSetsReplay(qh, current);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "shared-answer");
}

TEST_F(TracingReplayTest, SetsLearningPassSkipsDifferentResponses)
{
    /* If the two Bindings have different Responses, no intersection
       is inserted — they're not evidence for the same precondition. */
    TracingIndex index(dbPath);

    auto qh = hashString(HashAlgorithm::SHA256, "queryHash-no-learning");

    auto p1 = makeSorted({makeMember("q1", "r1"), makeMember("q2", "r2")});
    auto p2 = makeSorted({makeMember("q1", "r1"), makeMember("q3", "r3")});
    auto p1h = index.insertPreconditionSet(p1);
    auto p2h = index.insertPreconditionSet(p2);
    auto r1 = index.insertSetResponse("answer-1");
    auto r2 = index.insertSetResponse("answer-2");
    index.insertBinding(qh, p1h, r1);
    index.insertBinding(qh, p2h, r2);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(index.runLearningPass(qh), 0u);
}

TEST_F(TracingReplayTest, SetsLookupPicksFirstValidBinding)
{
    TracingIndex index(dbPath);

    auto qh = hashString(HashAlgorithm::SHA256, "queryHash-multi");

    // Binding whose precondition matches current.
    auto p1 = makeSorted({makeMember("q1", "r1")});
    auto p1h = index.insertPreconditionSet(p1);
    auto r1 = index.insertSetResponse("answer-1");
    index.insertBinding(qh, p1h, r1);

    // Binding whose precondition is incompatible with current (same Query, different Response).
    auto p2 = makeSorted({makeMember("q1", "r1-different")});
    auto p2h = index.insertPreconditionSet(p2);
    auto r2 = index.insertSetResponse("answer-2");
    index.insertBinding(qh, p2h, r2);

    TracingIndex::flushAllWriteQueues();

    auto current = makeSorted({makeMember("q1", "r1")});
    auto hit = index.lookupSetsReplay(qh, current);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, "answer-1") << "must skip the binding whose precondition's response doesn't match";
}

} // namespace nix

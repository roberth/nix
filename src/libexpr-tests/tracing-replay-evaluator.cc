#include <gtest/gtest.h>
#include <memory>
#include <filesystem>

#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-writer.hh"
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
    std::filesystem::path hashCacheDbPath;
    std::shared_ptr<SystemEnvironment> defaultEnv;

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
        hashCacheDbPath = tmpDir / ("nix-test-hashcache-" + suffix + ".sqlite");
        std::filesystem::remove(dbPath);
        std::filesystem::remove(hashCacheDbPath);
    }

    void TearDown() override
    {
        std::filesystem::remove(dbPath);
        std::filesystem::remove(hashCacheDbPath);
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
        return make_ref<TracingReplayEvaluator>(interpreter, index, *defaultEnv, hashCacheDbPath);
    }

    /**
     * Create a TracingReplayEvaluator with a custom Environment for validation.
     */
    ref<TracingReplayEvaluator> makeReplayEvaluatorWithEnv(TracingIndex & index, ref<Environment> env)
    {
        auto innerState = makeStateWithEnv(env);
        auto interpreter = make_ref<Interpreter>(innerState);
        return make_ref<TracingReplayEvaluator>(interpreter, index, *env, hashCacheDbPath);
    }
};

TEST_F(TracingReplayTest, ReplayGetType)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootPath(CanonPath::root));
        obj->getType();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootPath(CanonPath::root));
    EXPECT_EQ(obj->getType(), nInt);
}

TEST_F(TracingReplayTest, ReplayGetInt)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootPath(CanonPath::root));
        obj->getInt();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootPath(CanonPath::root));
    EXPECT_EQ(obj->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayGetString)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("\"hello\"", state->rootPath(CanonPath::root));
        obj->getStringIgnoreContext();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("\"hello\"", state->rootPath(CanonPath::root));
    EXPECT_EQ(obj->getStringIgnoreContext(), "hello");
}

TEST_F(TracingReplayTest, ReplayGetBool)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("true", state->rootPath(CanonPath::root));
        obj->getBool();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("true", state->rootPath(CanonPath::root));
    EXPECT_TRUE(obj->getBool());
}

TEST_F(TracingReplayTest, ReplayGetAttr)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ x = 42; }", state->rootPath(CanonPath::root));
        auto x = obj->maybeGetAttr("x");
        x->getInt();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ x = 42; }", state->rootPath(CanonPath::root));
    auto x = obj->maybeGetAttr("x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayMissingAttr)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ }", state->rootPath(CanonPath::root));
        obj->maybeGetAttr("nonexistent");
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ }", state->rootPath(CanonPath::root));
    EXPECT_EQ(obj->maybeGetAttr("nonexistent"), nullptr);
}

TEST_F(TracingReplayTest, ReplayGetAttrNames)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ a = 1; b = 2; }", state->rootPath(CanonPath::root));
        obj->getAttrNames();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("{ a = 1; b = 2; }", state->rootPath(CanonPath::root));
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(TracingReplayTest, ReplayGetFloat)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("3.14", state->rootPath(CanonPath::root));
        obj->getFloat();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("3.14", state->rootPath(CanonPath::root));
    EXPECT_DOUBLE_EQ(obj->getFloat(), 3.14);
}

TEST_F(TracingReplayTest, ReplayGetListSize)
{
    TracingIndex index(dbPath);

    recordToIndex(index, [&](Evaluator & eval) {
        auto obj = eval.evalExpr("[1 2 3]", state->rootPath(CanonPath::root));
        obj->getListSize();
    });

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("[1 2 3]", state->rootPath(CanonPath::root));
    EXPECT_EQ(obj->getListSize(), 3u);
}

TEST_F(TracingReplayTest, FallbackToInnerOnMiss)
{
    // Empty index — everything is a miss, so replay falls back to inner
    TracingIndex index(dbPath);

    auto replay = makeReplayEvaluator(index);
    auto obj = replay->evalExpr("42", state->rootPath(CanonPath::root));
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
        auto obj = eval.evalExpr("builtins.getEnv \"NIX_TEST_REPLAY_ENV\"", state->rootPath(CanonPath::root));
        obj->getStringIgnoreContext();
    });

    // Replay with an Environment that overrides the var — but the
    // process env still has "recorded", so std::getenv would not
    // detect the change.
    auto replayEnv = make_ref<OverrideEnvEnvironment>(
        sysEnv, std::map<std::string, std::optional<std::string>>{{"NIX_TEST_REPLAY_ENV", "changed"}});

    auto replay = makeReplayEvaluatorWithEnv(index, replayEnv);
    auto obj = replay->evalExpr("builtins.getEnv \"NIX_TEST_REPLAY_ENV\"", state->rootPath(CanonPath::root));

    // With the fix (validation via Environment): "changed" != "recorded"
    // → invalidation → inner evaluator returns "changed".
    // Without the fix (std::getenv): "recorded" == "recorded"
    // → cache hit → stale value "recorded".
    EXPECT_EQ(obj->getStringIgnoreContext(), "changed");

    unsetenv("NIX_TEST_REPLAY_ENV");
}

} // namespace nix

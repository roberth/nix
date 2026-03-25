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
#include "nix/store/tests/libstore.hh"

namespace nix {

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
     * Create a TracingReplayEvaluator that replays from the given index.
     */
    ref<TracingReplayEvaluator> makeReplayEvaluator(TracingIndex & index)
    {
        auto interpreter = make_ref<Interpreter>(makeState());
        return make_ref<TracingReplayEvaluator>(interpreter, index, hashCacheDbPath);
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

} // namespace nix

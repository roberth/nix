#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

/**
 * In-memory trace sink that collects JSON entries for building a trace.
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
    }

    ref<EvalState> makeState()
    {
        return make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
    }

    /**
     * Record a trace by evaluating through TracingEvaluator,
     * then parse the trace entries into TraceEntry vector.
     */
    std::vector<trace::TraceEntry> recordTrace(std::function<void(Evaluator &)> work)
    {
        auto sink = std::make_shared<CollectingTraceSink>();
        TracingWriter writer(*sink);
        auto interpreter = make_ref<Interpreter>(makeState());
        TracingEvaluator tracing(writer, interpreter);
        work(tracing);

        // Parse the collected JSON entries into typed TraceEntry
        std::vector<trace::TraceEntry> trace;
        for (const auto & j : sink->entries) {
            if (auto entry = trace::parseTraceEntry(j))
                trace.push_back(std::move(*entry));
        }
        return trace;
    }

    /**
     * Create a TracingReplayObject from a trace for the given value handle.
     */
    std::shared_ptr<TracingReplayObject> makeReplayObject(
        const std::vector<trace::TraceEntry> & trace,
        const trace::QueryIndex & index,
        uint64_t valueNum)
    {
        return std::make_shared<TracingReplayObject>(
            *store, trace, index, valueNum, [this]() -> ref<Object> {
                throw Error("inner evaluator should not be called for cached values");
            });
    }
};

TEST_F(TracingReplayTest, ReplayGetType)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootPath(CanonPath::root));
        obj->getType();
    });

    trace::QueryIndex index(trace);

    // The evalExpr query should have produced v=0
    auto entry = index.lookup(trace::QueryExpr{"42", "/"});
    ASSERT_TRUE(entry.has_value());

    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_EQ(replay->getType(), nInt);
}

TEST_F(TracingReplayTest, ReplayGetInt)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("42", state->rootPath(CanonPath::root));
        obj->getInt();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"42", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_EQ(replay->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayGetString)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("\"hello\"", state->rootPath(CanonPath::root));
        obj->getStringIgnoreContext();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"\"hello\"", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_EQ(replay->getStringIgnoreContext(), "hello");
}

TEST_F(TracingReplayTest, ReplayGetBool)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("true", state->rootPath(CanonPath::root));
        obj->getBool();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"true", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_TRUE(replay->getBool());
}

TEST_F(TracingReplayTest, ReplayGetAttr)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ x = 42; }", state->rootPath(CanonPath::root));
        auto x = obj->maybeGetAttr("x");
        x->getInt();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"{ x = 42; }", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    auto x = replay->maybeGetAttr("x");
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->getInt().value, 42);
}

TEST_F(TracingReplayTest, ReplayMissingAttr)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ }", state->rootPath(CanonPath::root));
        obj->maybeGetAttr("nonexistent");
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"{ }", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_EQ(replay->maybeGetAttr("nonexistent"), nullptr);
}

TEST_F(TracingReplayTest, ReplayGetAttrNames)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("{ a = 1; b = 2; }", state->rootPath(CanonPath::root));
        obj->getAttrNames();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"{ a = 1; b = 2; }", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    auto names = replay->getAttrNames();
    EXPECT_EQ(names.size(), 2u);
}

TEST_F(TracingReplayTest, ReplayGetFloat)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("3.14", state->rootPath(CanonPath::root));
        obj->getFloat();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"3.14", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_DOUBLE_EQ(replay->getFloat(), 3.14);
}

TEST_F(TracingReplayTest, ReplayGetListSize)
{
    auto trace = recordTrace([&](Evaluator & eval) {
        auto obj = eval.evalExpr("[1 2 3]", state->rootPath(CanonPath::root));
        obj->getListSize();
    });

    trace::QueryIndex index(trace);
    auto entry = index.lookup(trace::QueryExpr{"[1 2 3]", "/"});
    ASSERT_TRUE(entry.has_value());
    auto * q = std::get_if<trace::Query<trace::QueryExpr>>(&trace[entry->queryIndex]);
    ASSERT_NE(q, nullptr);

    auto replay = makeReplayObject(trace, index, q->v);
    EXPECT_EQ(replay->getListSize(), 3u);
}

TEST_F(TracingReplayTest, FallbackToInnerOnMiss)
{
    // Empty trace — everything is a miss
    std::vector<trace::TraceEntry> trace;
    trace::QueryIndex index(trace);

    bool innerCalled = false;
    auto interpreter = make_ref<Interpreter>(makeState());
    auto replay = std::make_shared<TracingReplayObject>(
        *store, trace, index, 999, [&]() -> ref<Object> {
            innerCalled = true;
            return interpreter->evalExpr("42", state->rootPath(CanonPath::root));
        });

    auto type = replay->getType();
    EXPECT_TRUE(innerCalled);
    EXPECT_EQ(type, nInt);
}

} // namespace nix

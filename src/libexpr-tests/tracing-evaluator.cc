#include <gtest/gtest.h>
#include <memory>
#include <vector>

#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/interpreter.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"

namespace nix {

/**
 * In-memory trace sink that collects JSON entries for inspection.
 */
class MemoryTraceSink : public TraceSink
{
public:
    std::vector<nlohmann::json> entries;

    void log(const nlohmann::json & entry) override
    {
        entries.push_back(entry);
    }
};

class TracingEvaluatorTest : public LibStoreTest
{
protected:
    std::shared_ptr<EvalState> state;
    std::shared_ptr<MemoryTraceSink> sink;
    /* In-memory sqlite so the pool has real DB backing without a temp
       dir. Under the recursive-Selector migration, building a
       SelectorGetAttr / SelectorGetListElem requires interning the
       parent through the pool, which lives on the graph — no graph,
       no emission from TracingObject getters. */
    std::unique_ptr<TracingDecisionGraph> decisionGraph;
    std::unique_ptr<TracingWriter> writer;
    std::shared_ptr<TracingEvaluator> evaluator;
    bool readOnlyMode = false;
    fetchers::Settings fetchSettings{};
    EvalSettings evalSettings{readOnlyMode};

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    TracingEvaluatorTest()
        : LibStoreTest(openStore("dummy://?read-only=false"))
    {
    }

    void SetUp() override
    {
        auto stateRef = make_ref<EvalState>(LookupPath{}, store, fetchSettings, evalSettings, nullptr);
        state = stateRef;
        sink = std::make_shared<MemoryTraceSink>();
        decisionGraph = std::make_unique<TracingDecisionGraph>(":memory:");
        writer = std::make_unique<TracingWriter>(*sink, decisionGraph.get());
        auto interpreter = make_ref<Interpreter>(stateRef);
        evaluator = std::make_shared<TracingEvaluator>(*writer, interpreter);
    }
};

TEST_F(TracingEvaluatorTest, EvalExprTracesQueryAndResult)
{
    auto obj = evaluator->evalExpr("42", state->rootedPath(CanonPath::root));

    // Should have logged at least a query and result
    ASSERT_GE(sink->entries.size(), 2u);

    // First entry should be a query
    auto & first = sink->entries[0];
    EXPECT_TRUE(first.contains("query"));
    EXPECT_EQ(first.at("query").at("tag"), "expr");

    // Second entry should be a result
    auto & second = sink->entries[1];
    EXPECT_TRUE(second.contains("result"));
    EXPECT_EQ(second.at("result").at("type"), "int");
}

TEST_F(TracingEvaluatorTest, GetAttrTracesAccess)
{
    auto obj = evaluator->evalExpr("{ foo = 42; }", state->rootedPath(CanonPath::root));
    sink->entries.clear(); // Clear eval entries

    auto foo = obj->maybeGetAttr("foo");
    ASSERT_NE(foo, nullptr);

    /* Under cell-migration Phase C, evalExpr pre-populates the
       wrapper's cachedWHNF, so `whnf()` short-circuits without
       emitting an observation. Only the retrieval SelectorGetAttr
       fires. */
    ASSERT_GE(sink->entries.size(), 2u);
    auto & getAttrQuery = sink->entries[0];
    EXPECT_EQ(getAttrQuery.at("query").at("tag"), "getAttr");
    EXPECT_EQ(getAttrQuery.at("query").at("name"), "foo");
}

TEST_F(TracingEvaluatorTest, GetStringTracesValue)
{
    auto obj = evaluator->evalExpr("\"hello\"", state->rootedPath(CanonPath::root));

    /* Under cell-migration Phase C, evalExpr computes the WHNF eagerly
       (one-atomic-until-body-WHNF) and pre-populates the wrapper's
       cachedWHNF. Verify the emission via the evalExpr result entry
       BEFORE clearing, so we can still confirm the value round-tripped
       through the trace layer. */
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("value"), "hello");

    sink->entries.clear();
    auto s = obj->getStringIgnoreContext();
    EXPECT_EQ(s, "hello");
    /* Cached WHNF short-circuits — no additional entries. */
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetIntTracesValue)
{
    auto obj = evaluator->evalExpr("42", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("value"), 42);

    sink->entries.clear();
    auto i = obj->getInt();
    EXPECT_EQ(i.value, 42);
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetBoolTracesValue)
{
    auto obj = evaluator->evalExpr("true", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("value"), true);

    sink->entries.clear();
    auto b = obj->getBool();
    EXPECT_TRUE(b);
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetTypeTracesType)
{
    auto obj = evaluator->evalExpr("{ }", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("type"), "set");

    sink->entries.clear();
    auto type = obj->getType();
    EXPECT_EQ(type, nAttrs);
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetAttrNamesTracesNames)
{
    auto obj = evaluator->evalExpr("{ a = 1; b = 2; }", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("type"), "set");
    auto attrNames = evalResult.at("result").at("names").get<std::vector<std::string>>();
    EXPECT_EQ(attrNames.size(), 2u);

    sink->entries.clear();
    auto names = obj->getAttrNames();
    EXPECT_EQ(names.size(), 2u);
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetListSizeTracesSize)
{
    auto obj = evaluator->evalExpr("[1 2 3]", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("size"), 3u);

    sink->entries.clear();
    auto sz = obj->getListSize();
    EXPECT_EQ(sz, 3u);
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetListElemTracesAccess)
{
    auto obj = evaluator->evalExpr("[10 20 30]", state->rootedPath(CanonPath::root));
    sink->entries.clear();

    auto elem = obj->getListElem(1);
    ASSERT_NE(elem, nullptr);

    /* Under cell-migration Phase C, evalExpr's cachedWHNF short-circuits
       the parent whnf() call in getListElem — only SelectorGetListElem
       (retrieval) fires. */
    ASSERT_GE(sink->entries.size(), 2u);
    auto & getElemQuery = sink->entries[0];
    EXPECT_EQ(getElemQuery.at("query").at("tag"), "getListElem");
    EXPECT_EQ(getElemQuery.at("query").at("index"), 1u);
}

TEST_F(TracingEvaluatorTest, MissingAttrProjectedFromWHNF)
{
    auto obj = evaluator->evalExpr("{ }", state->rootedPath(CanonPath::root));
    /* Under cell-migration Phase C, the cachedWHNF is populated during
       evalExpr — verify the parent WHNF is an empty set BEFORE
       clearing so we can still assert on the projection. */
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_EQ(evalResult.at("result").at("type"), "set");
    EXPECT_EQ(evalResult.at("result").at("names").size(), 0u);

    sink->entries.clear();
    auto missing = obj->maybeGetAttr("nonexistent");
    EXPECT_EQ(missing, nullptr);
    /* Membership projected from cachedWHNF: no probe emitted. */
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, DefeatCacheDoesNotTrace)
{
    auto obj = evaluator->evalExpr("42", state->rootedPath(CanonPath::root));
    sink->entries.clear();

    obj->defeatCache();

    // defeatCache should not add trace entries
    EXPECT_EQ(sink->entries.size(), 0u);
}

TEST_F(TracingEvaluatorTest, GetFloatTracesValue)
{
    auto obj = evaluator->evalExpr("3.14", state->rootedPath(CanonPath::root));
    ASSERT_GE(sink->entries.size(), 2u);
    auto & evalResult = sink->entries[1];
    EXPECT_DOUBLE_EQ(evalResult.at("result").at("value").get<double>(), 3.14);

    sink->entries.clear();
    auto f = obj->getFloat();
    EXPECT_DOUBLE_EQ(f, 3.14);
    EXPECT_EQ(sink->entries.size(), 0u);
}

} // namespace nix

#include <gtest/gtest.h>

#include "nix/expr/outer-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/arg-cell.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/source-root.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/file-system.hh"

namespace nix {

/* Behavior tests for OuterObject's producer-composition invariants.
   Per-alternative shape variation is subsumed by the datatype-generic
   Selector serde — the tests here pin the outer-side composition:
   producer wiring, child Selector construction per method, argCell
   propagation. See #209 NON-goals list for what deliberately isn't
   covered here. */

namespace {

/** Minimal Object stub. `getType()` returns nNull, so
   computeWHNFFromObject reaches only the type-tag path (no other
   virtuals invoked). Every other method throws — the mock queryFn
   intercepts before delegation is reached, so throws indicate a
   test-scaffold bug rather than a legitimate flow. */
class StubObject : public Object
{
public:
    ObjectType getType() override { return nNull; }
    ObjectType getTypeLazy() override { return nNull; }

    /* OuterObject::maybeGetAttr / getListElem force the child through
       outerObj before snapshotting the producer (line 42, 193). Return
       a fresh stub so the pre-force succeeds; the mock queryFn drives
       the returned wrapper's identity. */
    std::shared_ptr<Object> maybeGetAttr(const std::string &) override
    { return std::make_shared<StubObject>(); }
    std::vector<std::string> getAttrNames() override
    { throw std::runtime_error("StubObject::getAttrNames should not fire"); }
    std::string getStringIgnoreContext() override
    { throw std::runtime_error("StubObject::getStringIgnoreContext"); }
    std::string getStringWithoutContext() override
    { throw std::runtime_error("StubObject::getStringWithoutContext"); }
    std::pair<std::string, NixStringContext> getStringWithContext() override
    { throw std::runtime_error("StubObject::getStringWithContext"); }
    RootedPath getPath() override
    { throw std::runtime_error("StubObject::getPath"); }
    bool getBool(std::string_view = "") override
    { throw std::runtime_error("StubObject::getBool"); }
    NixInt getInt(std::string_view = "") override
    { throw std::runtime_error("StubObject::getInt"); }
    NixFloat getFloat(std::string_view = "") override
    { throw std::runtime_error("StubObject::getFloat"); }
    size_t getListSize() override
    { throw std::runtime_error("StubObject::getListSize"); }
    /* Symmetric to maybeGetAttr: OuterObject::getListElem forces the
       child through outerObj too — return a fresh stub. */
    std::shared_ptr<Object> getListElem(size_t) override
    { return std::make_shared<StubObject>(); }
    RootValue defeatCache() override
    { throw std::runtime_error("StubObject::defeatCache"); }
    RootValue toValueOrProxy(EvalState &, std::shared_ptr<OuterResolver>) override
    { throw std::runtime_error("StubObject::toValueOrProxy"); }
    std::optional<FunctionInfo> getFunctionInfo() override { return std::nullopt; }
    PosIdx getPos() override { return noPos; }
    std::optional<std::vector<std::string>> getAttrPath() override { return std::nullopt; }
    std::shared_ptr<Object> queryApply(std::shared_ptr<Object>) override
    { throw std::runtime_error("StubObject::queryApply"); }
};

} // namespace

class OuterObjectTest : public ::testing::Test
{
protected:
    std::filesystem::path tempDir;
    std::filesystem::path dbPath;
    std::unique_ptr<TracingDecisionGraph> g;
    ref<SourceRoot> srcRoot = SourceRoot::make(
        make_ref<MemorySourceAccessor>().cast<SourceAccessor>(), SourceRootKind::Internal);

    void SetUp() override
    {
        tempDir = createTempDir();
        dbPath = tempDir / "index.sqlite";
        g = std::make_unique<TracingDecisionGraph>(dbPath);
    }

    void TearDown() override
    {
        g.reset();
        std::filesystem::remove_all(tempDir);
    }

    /** Intern a stable producer Selector (a SelectorImport, arbitrary
        content). Used as the outer proxy's identity in tests. */
    ref<const trace::Selector> makeProducer(const std::string & path = "/test.nix")
    {
        return g->selectorPool.intern(trace::SelectorImport{path});
    }
};

/* 1. Producer wiring — constructor stores producer; getSelector /
   getSelectorHashHex return matching values. */
TEST_F(OuterObjectTest, ProducerWiring)
{
    auto producer = makeProducer();
    auto outer = OuterObject(
        [producer]() { return producer; },
        std::make_shared<StubObject>(),
        [](std::shared_ptr<Object>, ref<const trace::Selector>, ref<const trace::Selector>,
           std::shared_ptr<const ArgCell>) -> OuterQueryResult
        { throw std::runtime_error("queryFn should not fire"); },
        srcRoot,
        g->selectorPool);

    auto sel = outer.getSelector();
    ASSERT_TRUE(sel.has_value());
    EXPECT_EQ(sel->get_ptr().get(), producer.get_ptr().get());
    EXPECT_EQ(*outer.getSelectorHashHex(),
              producer->cachedHash.toHex());
}

/* 2. maybeGetAttr — child's producer is SelectorGetAttr{name, parent=self.producer};
   cachedHash matches. Also verify argCell is inherited by nav children. */
TEST_F(OuterObjectTest, MaybeGetAttrChildProducer)
{
    auto producer = makeProducer();
    auto stubChild = std::make_shared<StubObject>();
    auto callerCell = ArgCell::make(nullptr, nullptr);

    OuterQueryFn queryFn = [stubChild](std::shared_ptr<Object>, ref<const trace::Selector>,
                                        ref<const trace::Selector>, std::shared_ptr<const ArgCell>) {
        return OuterQueryResult{trace::ResultWHNF{"set", trace::WHNFAttrs{{"x", "y"}}}, stubChild};
    };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, g->selectorPool);
    outer->withArgCell(callerCell);

    auto child = outer->maybeGetAttr("x");
    ASSERT_NE(child, nullptr);
    auto * childOuter = dynamic_cast<OuterObject *>(child.get());
    ASSERT_NE(childOuter, nullptr);

    auto childSel = childOuter->getSelector();
    ASSERT_TRUE(childSel.has_value());
    auto expected = g->selectorPool.intern(trace::SelectorGetAttr{"x", producer});
    EXPECT_EQ((*childSel)->cachedHash, expected->cachedHash);

    // Nav children share the parent's argCell.
    EXPECT_EQ(childOuter->getProxyArgCell().get(), callerCell.get());
}

/* 3. getListElem — child's producer is SelectorGetListElem{index, parent=self.producer};
   argCell inherited (independent code site from maybeGetAttr — each does its own
   withArgCell call, so needs its own coverage). */
TEST_F(OuterObjectTest, GetListElemChildProducer)
{
    auto producer = makeProducer();
    auto stubChild = std::make_shared<StubObject>();
    auto callerCell = ArgCell::make(nullptr, nullptr);

    OuterQueryFn queryFn = [stubChild](std::shared_ptr<Object>, ref<const trace::Selector>,
                                        ref<const trace::Selector>, std::shared_ptr<const ArgCell>) {
        return OuterQueryResult{trace::ResultWHNF{"list", trace::WHNFList{3}}, stubChild};
    };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, g->selectorPool);
    outer->withArgCell(callerCell);

    auto child = outer->getListElem(1);
    ASSERT_NE(child, nullptr);
    auto * childOuter = dynamic_cast<OuterObject *>(child.get());
    ASSERT_NE(childOuter, nullptr);

    auto childSel = childOuter->getSelector();
    ASSERT_TRUE(childSel.has_value());
    auto expected = g->selectorPool.intern(trace::SelectorGetListElem{1, producer});
    EXPECT_EQ((*childSel)->cachedHash, expected->cachedHash);

    // Nav children share the parent's argCell — asserted per-method since
    // maybeGetAttr and getListElem are independent withArgCell sites.
    EXPECT_EQ(childOuter->getProxyArgCell().get(), callerCell.get());
}

/* 4. getFunctionInfo — produces SelectorGetFunctionInfo{parent=self.producer}
   via queryFn. The method returns FunctionInfo (or nullopt), not an Object,
   so we assert the queryFn was invoked with the expected Selector shape. */
TEST_F(OuterObjectTest, GetFunctionInfoQuerySelector)
{
    auto producer = makeProducer();
    std::optional<TracingHash> capturedSelectorHash;

    OuterQueryFn queryFn = [&capturedSelectorHash](std::shared_ptr<Object>, ref<const trace::Selector> q,
                                                    ref<const trace::Selector>, std::shared_ptr<const ArgCell>) {
        capturedSelectorHash = q->cachedHash;
        return OuterQueryResult{trace::ResultFunctionInfo{true, {{"a", false}}, false}, nullptr};
    };

    auto outer = OuterObject(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, g->selectorPool);

    auto info = outer.getFunctionInfo();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->formals.size(), 1u);

    auto expected = g->selectorPool.intern(trace::SelectorGetFunctionInfo{producer});
    ASSERT_TRUE(capturedSelectorHash.has_value());
    EXPECT_EQ(*capturedSelectorHash, expected->cachedHash);
}

/* 5. queryApply — result wrapper's producer is SelectorApply{parent=fnProducer}.
   Verify applyFn RECEIVES the fn's producer (upstream wiring) AND the returned
   wrapper's producer callable resolves to the SelectorApply. */
TEST_F(OuterObjectTest, QueryApplyResultProducer)
{
    auto fnProducer = makeProducer("/fn.nix");
    auto argStub = std::make_shared<StubObject>();

    // Result-wrapper producer callable: returns SelectorApply{parent=fnProducer}.
    auto applyResultProducer = g->selectorPool.intern(trace::SelectorApply{fnProducer});

    std::optional<TracingHash> capturedFnProducerHash;
    OuterApplyFn applyFn = [applyResultProducer, &capturedFnProducerHash](
        std::shared_ptr<Object>, ref<const trace::Selector> fnProducerArg,
        std::shared_ptr<Object>, std::shared_ptr<const ArgCell>) {
        capturedFnProducerHash = fnProducerArg->cachedHash;
        return OuterApplyResult{
            std::make_shared<StubObject>(),
            [applyResultProducer]() { return applyResultProducer; },
        };
    };
    OuterQueryFn queryFn = [](std::shared_ptr<Object>, ref<const trace::Selector>, ref<const trace::Selector>,
                              std::shared_ptr<const ArgCell>) -> OuterQueryResult
    { throw std::runtime_error("queryFn should not fire for apply"); };

    auto outer = std::make_shared<OuterObject>(
        [fnProducer]() { return fnProducer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, g->selectorPool, applyFn);

    auto result = outer->queryApply(argStub);
    ASSERT_NE(result, nullptr);
    auto * resultOuter = dynamic_cast<OuterObject *>(result.get());
    ASSERT_NE(resultOuter, nullptr);

    auto resultSel = resultOuter->getSelector();
    ASSERT_TRUE(resultSel.has_value());
    EXPECT_EQ((*resultSel)->cachedHash, applyResultProducer->cachedHash);

    // Upstream wiring: applyFn received the outer's producer as fnProducer.
    ASSERT_TRUE(capturedFnProducerHash.has_value());
    EXPECT_EQ(*capturedFnProducerHash, fnProducer->cachedHash);
}

/* 6. argCell propagation — nav children inherit parent's argCell;
   applies open a fresh cell rooted at the caller's cell. */
TEST_F(OuterObjectTest, ArgCellPropagation)
{
    auto producer = makeProducer();
    auto callerCell = ArgCell::make(nullptr, nullptr);
    auto argStub = std::make_shared<StubObject>();

    // Capture the applyCell passed to applyFn to verify it's a fresh cell
    // rooted at callerCell (parent=callerCell), distinct from callerCell.
    std::shared_ptr<const ArgCell> capturedApplyCell;
    OuterApplyFn applyFn = [&capturedApplyCell, producer](
        std::shared_ptr<Object>, ref<const trace::Selector>,
        std::shared_ptr<Object>, std::shared_ptr<const ArgCell> applyCell) {
        capturedApplyCell = applyCell;
        return OuterApplyResult{
            std::make_shared<StubObject>(),
            [producer]() { return producer; },
        };
    };
    OuterQueryFn queryFn = [](std::shared_ptr<Object>, ref<const trace::Selector>, ref<const trace::Selector>,
                              std::shared_ptr<const ArgCell>) -> OuterQueryResult
    { throw std::runtime_error("queryFn should not fire"); };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, g->selectorPool, applyFn);
    outer->withArgCell(callerCell);

    auto result = outer->queryApply(argStub);
    ASSERT_NE(capturedApplyCell.get(), nullptr);
    // applyCell is a fresh cell (distinct from callerCell) whose parent
    // is callerCell — "one cell per apply, rooted at caller scope".
    EXPECT_NE(capturedApplyCell.get(), callerCell.get());
    EXPECT_EQ(capturedApplyCell->parent.get(), callerCell.get());
    // Apply-result wrapper carries the caller's scope, not the fresh applyCell —
    // downstream navigation on the result attributes back into caller scope.
    auto * resultOuter = dynamic_cast<OuterObject *>(result.get());
    ASSERT_NE(resultOuter, nullptr);
    EXPECT_EQ(resultOuter->getProxyArgCell().get(), callerCell.get());
}

} // namespace nix

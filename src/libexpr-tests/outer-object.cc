#include <gtest/gtest.h>

#include "nix/expr/outer-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/arg-cell.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/source-root.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/store/tests/libstore.hh"
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

class OuterObjectTest : public LibStoreTest
{
protected:
    std::filesystem::path tempDir;
    std::filesystem::path dbPath;
    std::unique_ptr<TracingDecisionGraph> g;
    /* EvalState is a required OuterObject ctor arg (for identifier
       stamping via stableRootIdentifier). None of these tests reach
       code that would call it, but the reference must be valid. */
    fetchers::Settings fetchSettings{};
    bool readOnlyMode = false;
    EvalSettings evalSettings{readOnlyMode};
    std::shared_ptr<EvalState> state;
    ref<SourceRoot> srcRoot = SourceRoot::make(
        make_ref<MemorySourceAccessor>().cast<SourceAccessor>(), SourceRootKind::Internal);

    static void SetUpTestSuite()
    {
        LibStoreTest::SetUpTestSuite();
        initGC();
    }

    OuterObjectTest()
        : LibStoreTest(openStore("dummy://?read-only=false"))
    {
    }

    void SetUp() override
    {
        tempDir = createTempDir();
        dbPath = tempDir / "index.sqlite";
        g = std::make_unique<TracingDecisionGraph>(dbPath);
        state = make_ref<EvalState>(
            LookupPath{}, store, fetchSettings, evalSettings, nullptr).get_ptr();
    }

    void TearDown() override
    {
        g.reset();
        state.reset();
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
           std::shared_ptr<ArgCell>) -> OuterQueryResult
        { throw std::runtime_error("queryFn should not fire"); },
        srcRoot,
        *state,
        g->selectorPool,
        nullptr);

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
    auto callerCell = RegularArgCell::make(nullptr, nullptr);

    OuterQueryFn queryFn = [stubChild](std::shared_ptr<Object>, ref<const trace::Selector>,
                                        ref<const trace::Selector>, std::shared_ptr<ArgCell>) {
        return OuterQueryResult{trace::ResultWHNF{"set", trace::WHNFAttrs{{"x", "y"}}}, stubChild};
    };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, *state, g->selectorPool, callerCell);

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
    auto callerCell = RegularArgCell::make(nullptr, nullptr);

    OuterQueryFn queryFn = [stubChild](std::shared_ptr<Object>, ref<const trace::Selector>,
                                        ref<const trace::Selector>, std::shared_ptr<ArgCell>) {
        return OuterQueryResult{trace::ResultWHNF{"list", trace::WHNFList{3}}, stubChild};
    };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, *state, g->selectorPool, callerCell);

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
                                                    ref<const trace::Selector>, std::shared_ptr<ArgCell>) {
        capturedSelectorHash = q->cachedHash;
        return OuterQueryResult{trace::ResultFunctionInfo{true, {{"a", false}}, false}, nullptr};
    };

    auto outer = OuterObject(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, *state, g->selectorPool, nullptr);

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
        std::shared_ptr<Object>, std::shared_ptr<ArgCell>) {
        capturedFnProducerHash = fnProducerArg->cachedHash;
        return OuterApplyResult{
            std::make_shared<StubObject>(),
            [applyResultProducer]() { return applyResultProducer; },
        };
    };
    OuterQueryFn queryFn = [](std::shared_ptr<Object>, ref<const trace::Selector>, ref<const trace::Selector>,
                              std::shared_ptr<ArgCell>) -> OuterQueryResult
    { throw std::runtime_error("queryFn should not fire for apply"); };

    auto outer = std::make_shared<OuterObject>(
        [fnProducer]() { return fnProducer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, *state, g->selectorPool, nullptr, applyFn);

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
   OuterObject::queryApply hands applyFn the caller's scope so the
   applyFn creates the concrete apply cell itself (#261: cell-kind
   choice belongs with the applyFn, not the caller). */
TEST_F(OuterObjectTest, ArgCellPropagation)
{
    auto producer = makeProducer();
    auto callerCell = RegularArgCell::make(nullptr, nullptr);
    auto argStub = std::make_shared<StubObject>();

    // Capture the callerScope handed to applyFn — under the lift it is
    // literally the caller's cell, not a fresh cell parented to it.
    std::shared_ptr<ArgCell> capturedCallerScope;
    OuterApplyFn applyFn = [&capturedCallerScope, producer](
        std::shared_ptr<Object>, ref<const trace::Selector>,
        std::shared_ptr<Object>, std::shared_ptr<ArgCell> callerScope) {
        capturedCallerScope = callerScope;
        return OuterApplyResult{
            std::make_shared<StubObject>(),
            [producer]() { return producer; },
        };
    };
    OuterQueryFn queryFn = [](std::shared_ptr<Object>, ref<const trace::Selector>, ref<const trace::Selector>,
                              std::shared_ptr<ArgCell>) -> OuterQueryResult
    { throw std::runtime_error("queryFn should not fire"); };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn, srcRoot, *state, g->selectorPool, callerCell, applyFn);

    auto result = outer->queryApply(argStub);
    // applyFn receives callerCell directly — cell creation is its own job.
    EXPECT_EQ(capturedCallerScope.get(), callerCell.get());
    // Apply-result wrapper carries the caller's scope so downstream
    // navigation on the result attributes into that scope.
    auto * resultOuter = dynamic_cast<OuterObject *>(result.get());
    ASSERT_NE(resultOuter, nullptr);
    EXPECT_EQ(resultOuter->getProxyArgCell().get(), callerCell.get());
}

/* SourceRoot round-trip through OuterObject::getPath. Pins Phase 1
   of the SourceRoot integration: the wrap layer must not substitute
   a stand-in SourceRoot for the path's actual admission.

   Without the fix, `getPath` returned `RootedPath{outerRootFSRoot,
   ...}` regardless of the payload's origin; two different
   SourceRoots crossed via the wrap would collapse to the same
   root on the far side. */
TEST_F(OuterObjectTest, GetPathReconstructsStampedSourceRoot)
{
    /* Admit a Copyable-kinded SourceRoot with a stamped unpinnedId —
       the shape a fetcher would produce (`<url>#<n>` identifier). */
    auto accessor = make_ref<MemorySourceAccessor>().cast<SourceAccessor>();
    auto stampedRoot = state->getOrCreateRoot(accessor, SourceRootKind::Copyable, "test://source-root-preservation");

    auto producer = makeProducer();
    /* queryFn returns a pre-stamped WHNFPath — same shape
       `computeWHNFFromObject` would produce for a path Value whose
       SourceRoot is `stampedRoot`. */
    OuterQueryFn queryFn = [this, &stampedRoot](
                               std::shared_ptr<Object>,
                               ref<const trace::Selector>,
                               ref<const trace::Selector>,
                               std::shared_ptr<ArgCell>) -> OuterQueryResult {
        return {trace::ResultWHNF{
                    "path",
                    trace::WHNFPath{"/", state->stableRootIdentifier(*stampedRoot)},
                },
                nullptr};
    };

    /* srcRoot in the fixture is a different, unrelated Internal
       SourceRoot — passing it as `outerRootFSRoot` is what the pre-
       fix substitution would have used. The test's point is that
       Phase 1d ignores this pinned fallback in favour of the
       identifier-driven lookup. */
    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn,
        srcRoot, *state, g->selectorPool, nullptr);

    auto rp = outer->getPath();
    EXPECT_EQ(rp.root.get(), stampedRoot.get());
}

/* Symmetric: an anonymous SourceRoot (unpinnedId=nullopt) gets an
   `anon#<n>` identifier via stableRootIdentifier and round-trips
   within-process. Cross-process is fragile (see the docstring on
   EvalState::anonymousRootIds) but same-session cold+warm works. */
TEST_F(OuterObjectTest, GetPathReconstructsAnonymousSourceRoot)
{
    auto accessor = make_ref<MemorySourceAccessor>().cast<SourceAccessor>();
    auto anonRoot = state->getOrCreateRoot(accessor, SourceRootKind::Copyable);
    /* Sanity: the identifier we're about to round-trip through the
       wire really is an anon#<n>, not a URL-derived one. */
    ASSERT_EQ(state->stableRootIdentifier(*anonRoot).value_or(""), "anon#0");

    auto producer = makeProducer();
    OuterQueryFn queryFn = [this, &anonRoot](
                               std::shared_ptr<Object>,
                               ref<const trace::Selector>,
                               ref<const trace::Selector>,
                               std::shared_ptr<ArgCell>) -> OuterQueryResult {
        return {trace::ResultWHNF{
                    "path",
                    trace::WHNFPath{"/", state->stableRootIdentifier(*anonRoot)},
                },
                nullptr};
    };

    auto outer = std::make_shared<OuterObject>(
        [producer]() { return producer; },
        std::make_shared<StubObject>(), queryFn,
        srcRoot, *state, g->selectorPool, nullptr);

    auto rp = outer->getPath();
    EXPECT_EQ(rp.root.get(), anonRoot.get());
}

} // namespace nix

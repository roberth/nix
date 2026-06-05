#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/source-path.hh"

namespace nix {

class ExprConcatStringsTest : public LibExprTest
{};

TEST_F(ExprConcatStringsTest, preservesFirstPathAccessorOnConcat)
{
    auto accessor = make_ref<MemorySourceAccessor>();

    Value vPath;
    /* Test fixture: pretend the in-memory accessor was admitted as
       System (matching the rootFS pathway today's concat preserves). */
    auto testRoot = make_ref<SourceRoot>(accessor.cast<SourceAccessor>(), SourceRootKind::System);
    vPath.mkPath(RootedPath{testRoot, CanonPath("/dir")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/sub\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    state.callFunction(vLambda, vPath, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nPath);
    EXPECT_EQ(&*vResult.path().accessor, &*accessor);
    EXPECT_EQ(vResult.path().path.abs(), "/dir/sub");
}

TEST_F(ExprConcatStringsTest, rootFSPathLiteralStillConcatsOnRootFS)
{
    auto * expr = state.parseExprFromString("/some/absolute + \"/leaf\"", state.rootedPath(CanonPath::root));
    Value v;
    state.eval(expr, v);
    state.forceValue(v, noPos);

    ASSERT_EQ(v.type(), nPath);
    EXPECT_EQ(&*v.path().accessor, &*state.rootFS);
    EXPECT_EQ(v.path().path.abs(), "/some/absolute/leaf");
}

/* Multi-string concat pins the capture-once/use-once contract: the
   first path operand's accessor is captured at iter 1 of the concat
   loop and used at the join site, regardless of how many string
   operands appear after it. */
TEST_F(ExprConcatStringsTest, preservesFirstPathAccessorThroughMultiStringConcat)
{
    auto accessor = make_ref<MemorySourceAccessor>();

    Value vPath;
    auto testRoot = make_ref<SourceRoot>(accessor.cast<SourceAccessor>(), SourceRootKind::System);
    vPath.mkPath(RootedPath{testRoot, CanonPath("/dir")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/a\" + \"/b\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    state.callFunction(vLambda, vPath, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nPath);
    EXPECT_EQ(&*vResult.path().accessor, &*accessor);
    EXPECT_EQ(vResult.path().path.abs(), "/dir/a/b");
}

} // namespace nix

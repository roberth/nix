#include <gtest/gtest.h>

#include "nix/expr/eval.hh"
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
    vPath.mkPath(SourcePath{accessor, CanonPath("/dir")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/sub\"", state.rootPath(CanonPath::root));
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
    auto * expr = state.parseExprFromString("/some/absolute + \"/leaf\"", state.rootPath(CanonPath::root));
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
    vPath.mkPath(SourcePath{accessor, CanonPath("/dir")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/a\" + \"/b\"", state.rootPath(CanonPath::root));
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

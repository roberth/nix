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
    auto testRoot = SourceRoot::make(accessor.cast<SourceAccessor>(), SourceRootKind::System);
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
    auto testRoot = SourceRoot::make(accessor.cast<SourceAccessor>(), SourceRootKind::System);
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

/* `fetchTreeResult + "/sub"` (Copyable first, string second) stays
   valid. The first operand sets the result's root; no walk is
   needed because the second operand is a string. The result is a
   Copyable-rooted path at the joined subpath -- read via the
   accessor without materialising the whole tree. */
TEST_F(ExprConcatStringsTest, copyableFirstWithStringSecondIsAllowed)
{
    Value vPath;
    auto acc = make_ref<MemorySourceAccessor>();
    auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    vPath.mkPath(RootedPath{root, CanonPath("/dir")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/sub\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    state.callFunction(vLambda, vPath, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nPath);
    EXPECT_EQ(&*vResult.path().accessor, &*acc);
    EXPECT_EQ(vResult.path().path.abs(), "/dir/sub");
}

/* `./foo + fetchTreeResult` — the second operand is Copyable. Same
   rejection: the Copyable operand would be stringified via the
   walking arm, and the result spliced as a storepath substring
   into the system-rooted prefix. The grandfathered System+System
   form (`/var/lib + /var/log`) is unchanged. */
TEST_F(ExprConcatStringsTest, rejectsCopyableAsSecondPath)
{
    Value vCopyable;
    auto acc = make_ref<MemorySourceAccessor>();
    auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    vCopyable.mkPath(RootedPath{root, CanonPath("/x")}, state.mem);

    auto * lambda = state.parseExprFromString("p: /tmp + p", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    ASSERT_THROW(
        {
            state.callFunction(vLambda, vCopyable, vResult, noPos);
            state.forceValue(vResult, noPos);
        },
        EvalError);
}

/* Part 1: lexical strict-`..` check for Copyable. `p + "/../escape"`
   where `p` is rooted on a Copyable accessor and the `..` would
   pop past root: rejected lexically (no I/O), distinct from the
   read-time symlink-aware check in Part 2. */
TEST_F(ExprConcatStringsTest, copyableConcatRejectsLexicalEscape)
{
    Value vPath;
    auto acc = make_ref<MemorySourceAccessor>();
    auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    /* Path at the accessor root: any `..` in the suffix immediately
       escapes. */
    vPath.mkPath(RootedPath{root, CanonPath::root}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/../escape\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    ASSERT_THROW(
        {
            state.callFunction(vLambda, vPath, vResult, noPos);
            state.forceValue(vResult, noPos);
        },
        EvalError);
}

/* Part 1 negative control: `..` that stays within a Copyable
   tree is fine. `/sub + "/../sibling"` lexically resolves to
   `/sibling` — depth never goes negative. */
TEST_F(ExprConcatStringsTest, copyableConcatAllowsInTreeParent)
{
    Value vPath;
    auto acc = make_ref<MemorySourceAccessor>();
    auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    vPath.mkPath(RootedPath{root, CanonPath("/sub")}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/../sibling\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    state.callFunction(vLambda, vPath, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nPath);
    EXPECT_EQ(vResult.path().path.abs(), "/sibling");
}

/* System keeps historical silent-clamp. `/foo + "/../bar"` lands
   at `/bar` on rootFS, no throw. Pins the asymmetry. */
TEST_F(ExprConcatStringsTest, systemConcatSilentlyClampsEscape)
{
    /* Literal absolute paths in the source are rootFS-rooted (System). */
    auto * expr = state.parseExprFromString("/some + \"/../bar\"", state.rootedPath(CanonPath::root));
    Value v;
    state.eval(expr, v);
    state.forceValue(v, noPos);

    ASSERT_EQ(v.type(), nPath);
    EXPECT_EQ(v.path().path.abs(), "/bar");
}

/* Lexical-only check — no I/O. A Copyable path whose suffix
   contains a regular name (no literal `..`) is NOT rejected by
   Part 1; the read-time check in Part 2 would catch actual
   symlink escapes. Pins the boundary between the two layers. */
TEST_F(ExprConcatStringsTest, copyableConcatNoCheckForNonDotDotSuffix)
{
    Value vPath;
    auto acc = make_ref<MemorySourceAccessor>();
    auto root = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    vPath.mkPath(RootedPath{root, CanonPath::root}, state.mem);

    auto * lambda = state.parseExprFromString("p: p + \"/some/regular/path\"", state.rootedPath(CanonPath::root));
    Value vLambda;
    state.eval(lambda, vLambda);

    Value vResult;
    state.callFunction(vLambda, vPath, vResult, noPos);
    state.forceValue(vResult, noPos);

    ASSERT_EQ(vResult.type(), nPath);
    EXPECT_EQ(vResult.path().path.abs(), "/some/regular/path");
}

} // namespace nix

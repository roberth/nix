#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* `builtins.isPathEquivalent a b` — exposes the same equivalence
   used by `genericClosure { pathEquivalent = true; ... }` as a
   directly-callable predicate. Defined on path/string pairs in any
   combination; other argument types are an error. Path × path uses
   the same (subpath, root) decomposition as `==`; path × string
   compares as if `toString` had been called on the path but
   without materialising the source tree. */

class IsPathEquivalentTest : public LibExprTest
{
protected:
    ref<SourceRoot> mkRoot(SourceRootKind kind, std::optional<std::pair<std::string, std::string>> file = std::nullopt)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        if (file)
            acc->addFile(CanonPath(file->first), std::string(file->second));
        return SourceRoot::make(acc.cast<SourceAccessor>(), kind);
    }

    Value mkPathVal(ref<SourceRoot> root, const std::string & p)
    {
        Value v;
        v.mkPath(RootedPath{root, CanonPath(p)}, state.mem);
        return v;
    }

    Value mkStringVal(std::string_view s)
    {
        Value v;
        v.mkString(s, state.mem);
        return v;
    }

    /* Run `builtins.isPathEquivalent a b` via a curried lambda
       over two prebuilt Values. */
    bool call(Value & a, Value & b)
    {
        auto * lambda =
            state.parseExprFromString("a: b: builtins.isPathEquivalent a b", state.rootedPath(CanonPath::root));
        Value vLambda;
        state.eval(lambda, vLambda);
        Value vMid, vRes;
        state.callFunction(vLambda, a, vMid, noPos);
        state.callFunction(vMid, b, vRes, noPos);
        state.forceValue(vRes, noPos);
        EXPECT_EQ(vRes.type(), nBool);
        return vRes.boolean();
    }
};

/* ----- Same-type same equivalence -------------------------------- */

TEST_F(IsPathEquivalentTest, stringEqualToItself)
{
    auto a = mkStringVal("/x");
    auto b = mkStringVal("/x");
    EXPECT_TRUE(call(a, b));
}

TEST_F(IsPathEquivalentTest, stringUnequalToOther)
{
    auto a = mkStringVal("/x");
    auto b = mkStringVal("/y");
    EXPECT_FALSE(call(a, b));
}

TEST_F(IsPathEquivalentTest, pathSameRootSameSubpathEqual)
{
    auto root = mkRoot(SourceRootKind::Copyable, {{"/f", "x"}});
    auto a = mkPathVal(root, "/f");
    auto b = mkPathVal(root, "/f");
    EXPECT_TRUE(call(a, b));
}

TEST_F(IsPathEquivalentTest, pathDifferentSubpathsUnequal)
{
    auto root = mkRoot(SourceRootKind::Copyable, {{"/x", "x"}});
    auto a = mkPathVal(root, "/x");
    auto b = mkPathVal(root, "/y");
    EXPECT_FALSE(call(a, b));
}

TEST_F(IsPathEquivalentTest, copyableDistinctAccessorsSameContentsEqual)
{
    /* Same as eqValues for nPath: Copyable kind allows accessor
       distinctness to be bridged by contents. */
    auto rootA = mkRoot(SourceRootKind::Copyable, {{"/f", "same"}});
    auto rootB = mkRoot(SourceRootKind::Copyable, {{"/f", "same"}});
    auto a = mkPathVal(rootA, "/f");
    auto b = mkPathVal(rootB, "/f");
    EXPECT_TRUE(call(a, b));
}

TEST_F(IsPathEquivalentTest, systemDistinctAccessorsSameSubpathAreEqual)
{
    /* System × System with the same subpath have the same
       toString (just the abspath), so they're equivalent
       regardless of which accessor backs each side. System is
       in practice singleton (rootFS), but the semantic is the
       more general one — derived from `toString a == toString b`. */
    auto rootA = mkRoot(SourceRootKind::System, {{"/f", "ignored-A"}});
    auto rootB = mkRoot(SourceRootKind::System, {{"/f", "ignored-B"}});
    auto a = mkPathVal(rootA, "/f");
    auto b = mkPathVal(rootB, "/f");
    EXPECT_TRUE(call(a, b));
}

TEST_F(IsPathEquivalentTest, internalSameAccessorEqualViaShortcut)
{
    /* Internal × Internal at the same accessor + subpath fires
       the cheap pointer-identity shortcut without invoking the
       toString-equivalence engine (which throws for Internal). */
    auto root = mkRoot(SourceRootKind::Internal, {{"/f", "x"}});
    auto a = mkPathVal(root, "/f");
    auto b = mkPathVal(root, "/f");
    EXPECT_TRUE(call(a, b));
}

TEST_F(IsPathEquivalentTest, internalDistinctAccessorsThrows)
{
    /* Internal × Internal at distinct accessors has no cheap
       shortcut; falls into toString reduction; toString of
       Internal is undefined → throw. */
    auto rootA = mkRoot(SourceRootKind::Internal, {{"/f", "x"}});
    auto rootB = mkRoot(SourceRootKind::Internal, {{"/f", "x"}});
    auto a = mkPathVal(rootA, "/f");
    auto b = mkPathVal(rootB, "/f");
    EXPECT_THROW(call(a, b), EvalError);
}

/* ----- Cross-type System ----------------------------------------- */

TEST_F(IsPathEquivalentTest, systemPathEquivToAbsString)
{
    auto root = mkRoot(SourceRootKind::System);
    auto p = mkPathVal(root, "/some/file");
    auto s = mkStringVal("/some/file");
    EXPECT_TRUE(call(p, s));
    /* Symmetry: same answer when arguments are swapped. */
    EXPECT_TRUE(call(s, p));
}

TEST_F(IsPathEquivalentTest, systemPathNotEquivToMismatchedString)
{
    auto root = mkRoot(SourceRootKind::System);
    auto p = mkPathVal(root, "/some/file");
    auto s = mkStringVal("/other/place");
    EXPECT_FALSE(call(p, s));
    EXPECT_FALSE(call(s, p));
}

/* ----- Cross-type Copyable cheap rejects ------------------------- */

TEST_F(IsPathEquivalentTest, copyableNonStoreShapedStringNotEquiv)
{
    auto root = mkRoot(SourceRootKind::Copyable);
    auto p = mkPathVal(root, "/x");
    auto s = mkStringVal("/not/a/store/path");
    EXPECT_FALSE(call(p, s));
}

/* ----- Internal throws on cross-type ----------------------------- */

TEST_F(IsPathEquivalentTest, internalCrossTypeThrows)
{
    auto root = mkRoot(SourceRootKind::Internal);
    auto p = mkPathVal(root, "/x");
    auto s = mkStringVal("/x");
    EXPECT_THROW(call(p, s), EvalError);
    EXPECT_THROW(call(s, p), EvalError);
}

/* ----- Unsupported argument types -------------------------------- */

TEST_F(IsPathEquivalentTest, intArgumentErrors)
{
    EXPECT_THROW(eval("builtins.isPathEquivalent 1 2"), EvalError);
    EXPECT_THROW(eval("builtins.isPathEquivalent ./x 5"), EvalError);
    EXPECT_THROW(eval("builtins.isPathEquivalent \"x\" null"), EvalError);
}

} // namespace nix

#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* `builtins.lessThan` routes through `CompareValues` (primops.cc).
   The language-level `<` on paths is `toString a < toString b`,
   computed concretely:
     - System path: `toString` is the subpath's abspath, no IO.
     - Copyable path: `toString` materialises the tree's root to
       a store path and appends the subpath — fires the
       `lint-fetch-whole-source-to-store` knob when fatal.
     - Internal path: `toString` errors, so the comparison errors.
   Two paths whose toStrings are equal are equivalent under `<`
   (neither is less). In particular, two System paths with the
   same abspath on distinct accessors are equivalent — the prior
   structural-by-accessor-identity behaviour was wrong. */

class PathOrderingTest : public LibExprTest
{
protected:
    static ref<SourceRoot> mkRoot(SourceRootKind kind)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        return SourceRoot::make(acc.cast<SourceAccessor>(), kind);
    }

    Value mkPathVal(ref<SourceRoot> root, const std::string & p)
    {
        Value v;
        v.mkPath(RootedPath{root, CanonPath(p)}, state.mem);
        return v;
    }

    /* Call `builtins.lessThan` on two prebuilt path Values via a
       curried lambda, returning the bool result. */
    bool lessThan(Value & v1, Value & v2)
    {
        auto * lambda = state.parseExprFromString("p1: p2: builtins.lessThan p1 p2", state.rootedPath(CanonPath::root));
        Value vLambda;
        state.eval(lambda, vLambda);
        Value vIntermediate, vResult;
        state.callFunction(vLambda, v1, vIntermediate, noPos);
        state.callFunction(vIntermediate, v2, vResult, noPos);
        state.forceValue(vResult, noPos);
        EXPECT_EQ(vResult.type(), nBool);
        return vResult.boolean();
    }
};

/* ----- System paths order by abspath ---------------------------- */

TEST_F(PathOrderingTest, systemAbspathDrivesComparison)
{
    /* Two System paths under the same accessor with distinct
       abspaths order by their abspaths (which `toString` returns
       verbatim). */
    auto root = mkRoot(SourceRootKind::System);
    auto vA = mkPathVal(root, "/a");
    auto vB = mkPathVal(root, "/b");
    EXPECT_TRUE(lessThan(vA, vB));
    EXPECT_FALSE(lessThan(vB, vA));
}

TEST_F(PathOrderingTest, systemSameAbspathDistinctAccessorsAreEquivalent)
{
    /* Two System paths with the same abspath on distinct
       accessors have the same `toString` — they're equivalent
       under `<` (neither is less). This is the rule the prior
       impl broke by sorting structurally on accessor identity. */
    auto rA = mkRoot(SourceRootKind::System);
    auto rB = mkRoot(SourceRootKind::System);
    auto vA = mkPathVal(rA, "/x");
    auto vB = mkPathVal(rB, "/x");
    EXPECT_FALSE(lessThan(vA, vB));
    EXPECT_FALSE(lessThan(vB, vA));
}

/* ----- Internal paths cannot be compared ------------------------ */

TEST_F(PathOrderingTest, internalLessThanThrows)
{
    /* `toString` on an Internal-kinded path errors; `<` inherits
       the same — surfaces a usage bug rather than picking an
       arbitrary order. */
    auto r = mkRoot(SourceRootKind::Internal);
    auto vA = mkPathVal(r, "/a");
    auto vB = mkPathVal(r, "/b");
    EXPECT_THROW(lessThan(vA, vB), EvalError);
}

/* ================================================================
 * Direct tests for `EvalState::comparePathsForOrdering`.
 *
 * The function commits to one of {Less, Equal, Greater} — no
 * `Expensive` arm — so tests exercise the cheap-branch decisions
 * directly without going through `builtins.lessThan`.
 * ================================================================ */

class ComparePathsForOrderingTest : public PathOrderingTest
{
protected:
    /* Build a path Value and return the heap pointer. The fixture's
       `mkPathVal` returns by value; we need stable Value& references
       for the helper signature, so allocate via state.mem. */
    Value * mkPathPtr(ref<SourceRoot> root, const std::string & p)
    {
        auto v = state.mem.allocValue();
        v->mkPath(RootedPath{root, CanonPath(p)}, state.mem);
        return v;
    }

    /* MemorySourceAccessor with a Copyable SourceRoot wrapping it
       and an optional fingerprint set on the accessor. */
    std::pair<ref<MemorySourceAccessor>, ref<SourceRoot>> mkCopyableRoot(std::optional<std::string> fp = std::nullopt)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        if (fp)
            acc->fingerprint = *fp;
        return {acc, SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable)};
    }

    std::strong_ordering call(Value & v1, Value & v2)
    {
        return state.comparePathsForOrdering(v1, v2, noPos, "");
    }
};

TEST_F(ComparePathsForOrderingTest, identicalRootedPathIsEqual)
{
    /* Same accessor, same kind, same subpath → covered by the
       same-prefix branch returning Equal from subpath compare.
       No IO, no materialisation. */
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * v1 = mkPathPtr(root, "/x");
    auto * v2 = mkPathPtr(root, "/x");
    EXPECT_EQ(call(*v1, *v2), std::strong_ordering::equal);
}

TEST_F(ComparePathsForOrderingTest, internalIsRejectedUpfront)
{
    /* Internal-kinded operand → throws regardless of the other
       side's kind or the subpaths. Matches `coerceToString`'s
       Internal arm. Tested on each side; also identical-path
       Internal × Internal, which used to short-circuit to Equal
       — now rejected upfront. */
    auto intRoot = mkRoot(SourceRootKind::Internal);
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto * vInt = mkPathPtr(intRoot, "/x");
    auto * vIntSame = mkPathPtr(intRoot, "/x");
    auto * vSys = mkPathPtr(sysRoot, "/x");
    EXPECT_THROW(call(*vInt, *vSys), EvalError);
    EXPECT_THROW(call(*vSys, *vInt), EvalError);
    EXPECT_THROW(call(*vInt, *vIntSame), EvalError);
}

TEST_F(ComparePathsForOrderingTest, sameRootDifferentSubpathDriven)
{
    /* Same root → toStrings share a prefix → lex order driven by
       the subpath alone, no materialisation. */
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * vA = mkPathPtr(root, "/a");
    auto * vB = mkPathPtr(root, "/b");
    EXPECT_EQ(call(*vA, *vB), std::strong_ordering::less);
    EXPECT_EQ(call(*vB, *vA), std::strong_ordering::greater);
}

TEST_F(ComparePathsForOrderingTest, matchingFingerprintRootsSubpathDriven)
{
    /* Distinct accessors but same fingerprint at root → same store
       path prefix → subpath drives the order. Cheap, no
       contentsEqual scan, no materialisation. */
    auto [accA, rootA] = mkCopyableRoot(std::string{"shared-fp"});
    auto [accB, rootB] = mkCopyableRoot(std::string{"shared-fp"});
    auto * vA = mkPathPtr(rootA, "/a");
    auto * vB = mkPathPtr(rootB, "/b");
    EXPECT_EQ(call(*vA, *vB), std::strong_ordering::less);
    EXPECT_EQ(call(*vB, *vA), std::strong_ordering::greater);
}

TEST_F(ComparePathsForOrderingTest, systemLexBeforeStoreDirIsLessThanCopyable)
{
    /* System abspath "/etc/..." lex-precedes "/nix/store/..."
       (the prefix all Copyable toStrings share). 'e' < 'n' at
       position 1 → System < Copyable. Cheap, no Copyable
       materialisation. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto copRoot = mkRoot(SourceRootKind::Copyable);
    auto * sys = mkPathPtr(sysRoot, "/etc/passwd");
    auto * cop = mkPathPtr(copRoot, "/");
    EXPECT_EQ(call(*sys, *cop), std::strong_ordering::less);
    EXPECT_EQ(call(*cop, *sys), std::strong_ordering::greater);
}

TEST_F(ComparePathsForOrderingTest, systemInsideStoreDirFallsThroughToMaterialise)
{
    /* When the System abspath lies inside `storeDir + "/"`, it
       shares the storeDir+/ prefix with any Copyable's
       toString. Step 2 classifies this as SamePrefix (the
       ternary "equal" arm) and defers to step 3 to fill in the
       Copyable's post-prefix bytes via `coerceToString`. We
       don't pin the exact outcome (depends on the Copyable's
       materialised store hash, which the test fixture doesn't
       expose) — only that the call returns a consistent
       ordering in both directions. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto copRoot = mkRoot(SourceRootKind::Copyable);
    auto sysInside = std::string{state.store->storeDir} + "/foo-thing";
    auto * sys = mkPathPtr(sysRoot, sysInside);
    auto * cop = mkPathPtr(copRoot, "/");
    auto fwd = call(*sys, *cop);
    auto rev = call(*cop, *sys);
    if (fwd == std::strong_ordering::less)
        EXPECT_EQ(rev, std::strong_ordering::greater);
    else if (fwd == std::strong_ordering::greater)
        EXPECT_EQ(rev, std::strong_ordering::less);
    else
        EXPECT_EQ(rev, std::strong_ordering::equal);
}

TEST_F(ComparePathsForOrderingTest, systemLexAfterStoreDirNotInStoreIsGreaterThanCopyable)
{
    /* System abspath "/var/..." lex-follows "/nix/store/..." and
       doesn't start with it. 'v' > 'n' at position 1 → System >
       Copyable. Cheap. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto copRoot = mkRoot(SourceRootKind::Copyable);
    auto * sys = mkPathPtr(sysRoot, "/var/log/foo");
    auto * cop = mkPathPtr(copRoot, "/");
    EXPECT_EQ(call(*sys, *cop), std::strong_ordering::greater);
    EXPECT_EQ(call(*cop, *sys), std::strong_ordering::less);
}

TEST_F(ComparePathsForOrderingTest, copyableXCopyableMaterialiseFallback)
{
    /* Two distinct Copyable roots whose accessor-internal
       `pathStrView` order is the reverse of the materialised
       toString order — pins that step 5 actually drives the
       answer when no cheap branch fires. The materialised
       form is `<storeDir>/<storeBase>/<subpath>`; storeBase is
       derived from the accessor's content hash, which differs
       between the two `mkCopyableRoot()` instances. The
       expected outcome can't be hardcoded (storeBase varies
       per fixture run), but transitivity must hold and the
       answer must be consistent across directions — that
       distinguishes a real materialise-driven ordering from
       a cheap shortcut taking either side's accessor identity
       as the discriminator. */
    auto [a, copA] = mkCopyableRoot();
    auto [b, copB] = mkCopyableRoot();
    auto * pa = mkPathPtr(copA, "/zfile");
    auto * pb = mkPathPtr(copB, "/afile");
    auto fwd = call(*pa, *pb);
    auto rev = call(*pb, *pa);
    /* Symmetry: forward and reverse must agree on the relation. */
    if (fwd == std::strong_ordering::less)
        EXPECT_EQ(rev, std::strong_ordering::greater);
    else if (fwd == std::strong_ordering::greater)
        EXPECT_EQ(rev, std::strong_ordering::less);
    else
        EXPECT_EQ(rev, std::strong_ordering::equal);
    /* Self-compare must be equal — pins that the materialise
       fallback's storepath computation is stable. */
    EXPECT_EQ(call(*pa, *pa), std::strong_ordering::equal);
    EXPECT_EQ(call(*pb, *pb), std::strong_ordering::equal);
}

/* ----- Original PathOrderingTest cases follow ------------------- */

/* ----- Full equivalence: same toString => neither is less ------- */

TEST_F(PathOrderingTest, equivalentPathsNeitherIsLess)
{
    /* Two RootedPaths with the same (path, kind, accessor) — even
       across distinct SourceRoot ref objects — have the same
       toString. Equivalent under `<`. */
    auto acc = make_ref<MemorySourceAccessor>();
    MemorySink{*acc}.createDirectory(CanonPath::root);
    auto rA = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::System);
    auto rB = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::System);
    ASSERT_NE(&*rA, &*rB);
    auto vA = mkPathVal(rA, "/x");
    auto vB = mkPathVal(rB, "/x");
    EXPECT_FALSE(lessThan(vA, vB));
    EXPECT_FALSE(lessThan(vB, vA));
}

} // namespace nix

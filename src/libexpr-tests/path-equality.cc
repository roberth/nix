#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* Two path Values compare equal iff `toString a == toString b`
   would have held — but computed without ever invoking toString
   on a Copyable (which would hash the whole tree). The same
   semantic applies in every direction (eqValues on nPath ×
   nPath, builtins.isPathEquivalent on any path/string pair).

   Operationally:
   - Cheap pre-shortcut: same accessor pointer + same subpath →
     true. Works for every kind, including Internal (where
     toString is otherwise undefined).
   - Reduce each operand to its "eager string" if possible:
     nString uses its bytes, nPath with kind System uses its
     subpath's abspath, nPath with kind Copyable is "lazy"
     (computing it would defeat the point), nPath with kind
     Internal throws.
   - Both reduced → string equality.
   - One reduced (the other Copyable) → check the eager string
     is store-path-shaped, its subpath matches the Copyable's
     CanonPath, then srcToStore lookup or copyPathToStore on the
     Copyable root to confirm the storePath matches.
   - Both Copyable → subpaths match AND accessorsEquivalent on
     the roots.

   System is *not* "by accessor identity"; two System paths with
   the same abspath are equivalent regardless of which accessor
   backs them (System is assumed singleton in practice anyway —
   rootFS — but the semantic is the more general one). Internal
   is by identity for the cheap shortcut and otherwise throws,
   matching toString. */

class PathEqualityTest : public LibExprTest
{
protected:
    /* Make a memory accessor with a single root directory and an
       optional file. */
    static std::pair<ref<MemorySourceAccessor>, ref<SourceRoot>> makeRoot(
        SourceRootKind kind = SourceRootKind::Copyable,
        std::optional<std::pair<std::string, std::string>> file = std::nullopt)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        if (file)
            acc->addFile(CanonPath(file->first), std::string(file->second));
        return {acc, SourceRoot::make(acc.cast<SourceAccessor>(), kind)};
    }

    /* Two-arg overload kept for old call sites that don't care
       about kind. */
    static std::pair<ref<MemorySourceAccessor>, ref<SourceRoot>>
    makeRoot(std::optional<std::pair<std::string, std::string>> file)
    {
        return makeRoot(SourceRootKind::Copyable, std::move(file));
    }

    /* mkPath helper to keep tests focused on the assertion. */
    Value mkPathVal(ref<SourceRoot> root, const std::string & p)
    {
        Value v;
        v.mkPath(RootedPath{root, CanonPath(p)}, state.mem);
        return v;
    }
};

TEST_F(PathEqualityTest, sameAccessorSamePathIsEqual)
{
    /* The pointer-identity shortcut that the old impl had. Keep it
       working under the new contents-based impl. */
    auto [acc, root] = makeRoot({{"/f", "x"}});
    auto v1 = mkPathVal(root, "/f");
    auto v2 = mkPathVal(root, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, differentAccessorsSameContentsAreEqual)
{
    /* The new behaviour: two accessors that happen to hold the same
       tree compare equal even though their pointers differ. The old
       impl would have said "unequal". */
    auto [accA, rootA] = makeRoot({{"/f", "shared"}});
    auto [accB, rootB] = makeRoot({{"/f", "shared"}});
    auto v1 = mkPathVal(rootA, "/f");
    auto v2 = mkPathVal(rootB, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, differentAccessorsDifferentContentsAreUnequal)
{
    auto [accA, rootA] = makeRoot({{"/f", "A"}});
    auto [accB, rootB] = makeRoot({{"/f", "B"}});
    auto v1 = mkPathVal(rootA, "/f");
    auto v2 = mkPathVal(rootB, "/f");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, fingerprintMatchSkipsTheWalk)
{
    /* When both accessors expose the same fingerprint the equality
       short-circuit fires. We make the contents intentionally
       differ; if the function were walking content it would say no.
       The test passes iff the fingerprint shortcut runs. */
    auto [accA, rootA] = makeRoot({{"/f", "differs A"}});
    accA->fingerprint = "shared-fp";
    auto [accB, rootB] = makeRoot({{"/f", "differs B"}});
    accB->fingerprint = "shared-fp";
    auto v1 = mkPathVal(rootA, "/f");
    auto v2 = mkPathVal(rootB, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, samePathStringDifferentAccessorsUnequalContents)
{
    auto [accA, rootA] = makeRoot({{"/x", "A"}});
    auto [accB, rootB] = makeRoot({{"/x", "B"}});
    auto v1 = mkPathVal(rootA, "/x");
    auto v2 = mkPathVal(rootB, "/x");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

/* ----- Subpath dominance ---------------------------------------- */

TEST_F(PathEqualityTest, differentSubpathsOnSameRootAreUnequal)
{
    /* Same root (so same accessor, same kind), but distinct
       subpaths. The (subpath, root) decomposition makes these
       unequal up front — no contents walk happens. */
    auto [acc, root] = makeRoot({{"/x", "same"}});
    acc->addFile(CanonPath("/y"), "same");
    auto v1 = mkPathVal(root, "/x");
    auto v2 = mkPathVal(root, "/y");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, differentSubpathsAcrossAccessorsAreUnequal)
{
    /* Different accessors AND different subpaths. The subpath
       mismatch fires before any root comparison. */
    auto [accA, rootA] = makeRoot({{"/x", "same"}});
    auto [accB, rootB] = makeRoot({{"/y", "same"}});
    auto v1 = mkPathVal(rootA, "/x");
    auto v2 = mkPathVal(rootB, "/y");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

/* ----- Cross-kind: derived from toString comparison ------------- */

TEST_F(PathEqualityTest, systemAndCopyableAtSameSubpathUnequalUnlessStringsAlign)
{
    /* System × Copyable at the same subpath /f:
         toString_System   = "/f"
         toString_Copyable = "<storeDir>/<hash>-name/f"
       These can never be string-equal (different prefixes), so
       the comparison is false. The reason isn't "kinds disagree"
       — it's that their toStrings disagree. (A System path
       whose abspath *does* happen to be a store path-shaped
       string could match a Copyable that materialises to that
       same store path; that case isn't covered here for setup
       cost but is the natural consequence of the unified
       semantic.) */
    auto acc = make_ref<MemorySourceAccessor>();
    MemorySink{*acc}.createDirectory(CanonPath::root);
    acc->addFile(CanonPath("/f"), "same");
    auto rSys = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::System);
    auto rCop = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto v1 = mkPathVal(rSys, "/f");
    auto v2 = mkPathVal(rCop, "/f");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

/* ----- System: equivalence is abspath-driven, not by-identity --- */

TEST_F(PathEqualityTest, systemDistinctAccessorsSameSubpathAreEqual)
{
    /* Two System-kinded roots over distinct accessors. Same
       subpath => same `toString` (which is just the subpath's
       abspath) => equivalent. The previous behaviour pinned them
       as unequal, which was a structural-identity hangover from
       before we unified eqValues with the toString semantic. */
    auto [accA, rootA] = makeRoot(SourceRootKind::System, {{"/f", "anything"}});
    auto [accB, rootB] = makeRoot(SourceRootKind::System, {{"/f", "different-contents-don't-matter"}});
    auto v1 = mkPathVal(rootA, "/f");
    auto v2 = mkPathVal(rootB, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, systemDistinctSubpathsUnequal)
{
    /* Sanity: same kind, distinct subpaths → different
       toStrings → unequal. */
    auto [accA, rootA] = makeRoot(SourceRootKind::System, {{"/a", "x"}});
    auto [accB, rootB] = makeRoot(SourceRootKind::System, {{"/b", "x"}});
    auto v1 = mkPathVal(rootA, "/a");
    auto v2 = mkPathVal(rootB, "/b");
    EXPECT_FALSE(state.eqValues(v1, v2, noPos, ""));
}

/* ----- Internal: cheap identity preserved, otherwise undefined - */

TEST_F(PathEqualityTest, internalSameAccessorIsEqualViaShortcut)
{
    /* Internal × Internal at the same accessor + subpath fires
       the cheap accessor-pointer shortcut without invoking the
       toString-equivalence engine — which would otherwise throw
       (toString is undefined on Internal). */
    auto [acc, root] = makeRoot(SourceRootKind::Internal, {{"/f", "x"}});
    auto v1 = mkPathVal(root, "/f");
    auto v2 = mkPathVal(root, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

TEST_F(PathEqualityTest, internalDistinctAccessorsThrows)
{
    /* Internal × Internal at distinct accessors can't use the
       cheap shortcut and falls into the toString-equivalence
       reduction; toString of Internal is undefined, so the
       comparison throws. Pin the throw so a future "silently
       return false" regression is caught. */
    auto [accA, rootA] = makeRoot(SourceRootKind::Internal, {{"/f", "same"}});
    auto [accB, rootB] = makeRoot(SourceRootKind::Internal, {{"/f", "same"}});
    auto v1 = mkPathVal(rootA, "/f");
    auto v2 = mkPathVal(rootB, "/f");
    EXPECT_THROW(state.eqValues(v1, v2, noPos, ""), EvalError);
}

TEST_F(PathEqualityTest, sameNonCopyableAccessorIsEqual)
{
    /* The same pointer-identical System accessor is equal to
       itself — the accessor-identity shortcut still fires. */
    auto [acc, root] = makeRoot(SourceRootKind::System, {{"/f", "x"}});
    auto v1 = mkPathVal(root, "/f");
    auto v2 = mkPathVal(root, "/f");
    EXPECT_TRUE(state.eqValues(v1, v2, noPos, ""));
}

} // namespace nix

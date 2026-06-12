#include "nix/util/source-root.hh"
#include "nix/util/memory-source-accessor.hh"

#include <gtest/gtest.h>

/* SourceRootKind values live in libexpr (intentionally — libutil
   doesn't know what they mean). Libutil-side tests synthesise their
   own opaque values via the enum's known uint8_t underlying type. */
namespace nix {
enum class SourceRootKind : std::uint8_t {};
} // namespace nix

namespace nix {

namespace {

ref<MemorySourceAccessor> mkAcc()
{
    auto a = make_ref<MemorySourceAccessor>();
    MemorySink{*a}.createDirectory(CanonPath::root);
    return a;
}

constexpr auto k0 = static_cast<SourceRootKind>(0);
constexpr auto k1 = static_cast<SourceRootKind>(1);

} // namespace

/* =================================================================
 * RootedPath comparison.
 *
 * Comparison is structural on the triple (path, kind, accessor),
 * in that order. Equality means the three fields agree; ordering is
 * lex on the same triple. The accessor is compared by its `number`
 * (the SourceAccessor's own equality/ordering).
 *
 * Important: identity of the SourceRoot instance is NOT what
 * matters. Two distinct `ref<SourceRoot>`s with the same accessor
 * and kind designate the same root and compare equal.
 * =================================================================
 */

/* ----- Equality --------------------------------------------------- */

TEST(RootedPathEqual, sameTripleEqual)
{
    auto acc = mkAcc();
    auto r1 = SourceRoot::make(acc, k0);
    auto r2 = SourceRoot::make(acc, k0);
    /* Distinct SourceRoot instances; identical (accessor, kind). */
    EXPECT_NE(&*r1, &*r2);
    EXPECT_EQ(RootedPath{r1}, RootedPath{r2});
}

TEST(RootedPathEqual, differingPathUnequal)
{
    auto acc = mkAcc();
    acc->addFile(CanonPath("/x"), "");
    acc->addFile(CanonPath("/y"), "");
    auto r = SourceRoot::make(acc, k0);
    EXPECT_NE(RootedPath(r, CanonPath("/x")), RootedPath(r, CanonPath("/y")));
}

TEST(RootedPathEqual, differingKindUnequal)
{
    auto acc = mkAcc();
    auto r0 = SourceRoot::make(acc, k0);
    auto r1 = SourceRoot::make(acc, k1);
    EXPECT_NE(RootedPath{r0}, RootedPath{r1});
}

TEST(RootedPathEqual, differingAccessorUnequal)
{
    auto accA = mkAcc();
    auto accB = mkAcc();
    auto rA = SourceRoot::make(accA, k0);
    auto rB = SourceRoot::make(accB, k0);
    EXPECT_NE(RootedPath{rA}, RootedPath{rB});
}

/* ----- Ordering: lex by (path, kind, accessor) -------------------- */

TEST(RootedPathOrder, pathDominatesKind)
{
    /* Two roots that disagree on kind. If we compare them at two
       paths whose lex order is `/a` < `/b`, then no matter the
       relative ordering of the kinds, the path /a side must come
       first. Pin "path before kind". */
    auto acc = mkAcc();
    acc->addFile(CanonPath("/a"), "");
    acc->addFile(CanonPath("/b"), "");
    auto rLow = SourceRoot::make(acc, k0);
    auto rHigh = SourceRoot::make(acc, k1);

    /* /a with the higher kind vs /b with the lower kind — path wins. */
    EXPECT_LT(RootedPath(rHigh, CanonPath("/a")), RootedPath(rLow, CanonPath("/b")));
}

TEST(RootedPathOrder, kindDominatesAccessor)
{
    /* Same path on both sides. The kind difference must win over the
       accessor difference (since accessors come last). */
    auto accA = mkAcc();
    auto accB = mkAcc();
    /* Distinct accessors; one is "lower" than the other by number.
       We don't care which; pick the explicit pairing so kind drives
       the order. */
    auto rLowKind = SourceRoot::make(accB, k0);  /* "higher" accessor + lower kind */
    auto rHighKind = SourceRoot::make(accA, k1); /* "lower" accessor + higher kind */

    EXPECT_LT(RootedPath{rLowKind}, RootedPath{rHighKind});
}

TEST(RootedPathOrder, accessorBreaksTies)
{
    /* Path equal, kind equal — accessor identity is the final
       tiebreaker. */
    auto accA = mkAcc();
    auto accB = mkAcc();
    auto rA = SourceRoot::make(accA, k0);
    auto rB = SourceRoot::make(accB, k0);

    auto expected = (accA->number < accB->number) ? std::strong_ordering::less : std::strong_ordering::greater;
    EXPECT_EQ((RootedPath{rA} <=> RootedPath{rB}), expected);
}

TEST(RootedPathOrder, equalGivesEquivalent)
{
    auto acc = mkAcc();
    auto r1 = SourceRoot::make(acc, k0);
    auto r2 = SourceRoot::make(acc, k0);
    EXPECT_EQ((RootedPath{r1} <=> RootedPath{r2}), std::strong_ordering::equal);
}

/* =================================================================
 * unpinnedId — the per-source identifier used by the eval cache as a
 * SourceRoot identity that is stable across versions of the same
 * source-of-truth. Populated by producers (fetchTree, mountInput) with
 * `Input::toUnpinnedURL`. Internal-kinded roots and producers that
 * don't know an identity leave it as `nullopt`.
 *
 * Important: the identifier is metadata. Equality of RootedPath stays
 * structural on (path, kind, accessor) only; two SourceRoots with the
 * same accessor+kind but different `unpinnedId` still compare equal.
 * Otherwise a producer accidentally setting nullopt for an already-
 * registered accessor would silently shadow the registered identity.
 * =================================================================
 */

TEST(SourceRootUnpinnedId, defaultsToNullopt)
{
    auto acc = mkAcc();
    auto r = SourceRoot::make(acc, k0);
    EXPECT_EQ(r->unpinnedId, std::nullopt);
}

TEST(SourceRootUnpinnedId, setViaFactory)
{
    auto acc = mkAcc();
    auto r = SourceRoot::make(acc, k0, "github:NixOS/nixpkgs");
    ASSERT_TRUE(r->unpinnedId.has_value());
    EXPECT_EQ(*r->unpinnedId, "github:NixOS/nixpkgs");
}

TEST(SourceRootUnpinnedId, notPartOfRootedPathEquality)
{
    /* Two roots wrapping the same (accessor, kind) but with distinct
       unpinnedIds — including a "stamped vs. unstamped" pair — must
       still compare equal as RootedPaths. */
    auto acc = mkAcc();
    auto rUnstamped = SourceRoot::make(acc, k0);
    auto rStampedA = SourceRoot::make(acc, k0, "github:NixOS/nixpkgs");
    auto rStampedB = SourceRoot::make(acc, k0, "git+https://example.com/foo");
    EXPECT_EQ(RootedPath{rUnstamped}, RootedPath{rStampedA});
    EXPECT_EQ(RootedPath{rStampedA}, RootedPath{rStampedB});
}

} // namespace nix

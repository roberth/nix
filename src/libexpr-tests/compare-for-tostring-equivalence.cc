#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"

namespace nix {

/* `compareForToStringEquivalence` is a total preorder on
   path-or-string Values whose equivalence classes match
   toString-equivalence. Used as the comparator for sorted
   dedup over heterogeneous keys (`PathEquivalentDedup`'s
   `otherMap`); the ordering itself does NOT have to match the
   language-level `toString a < toString b` lex order — just
   any consistent total preorder whose equivalence agrees with
   `toString` equality.

   Layered ordering:
     1. Camp partition.
        - Camp A: byte-for-byte canonical store-path-shape,
          `storeDir + "/" + 32-nix32-hash + "-source"` optionally
          followed by a canonical subpath. Copyable nPath is
          always Camp A (its would-be toString is canonical by
          construction). System nPath whose abspath matches the
          shape, or nString matching the shape, also Camp A.
        - Camp B: everything else. Includes non-canonical
          store-path strings (trailing slash, `/.`, `/..`,
          shorter hashes, names other than `source`) — Camp A
          admits ONLY what `toString` could produce verbatim.
        - Camp B < Camp A.
     2. Subpath compare.
        - Camp A: byte lex on the parsed subpath after
          `hash-source`.
        - Camp B: byte lex on the full bytes (System abspath /
          string).
     3. (later) Source root equivalence class — for cases where
        step 2 returns equal within Camp A. */

class CompareForToStringEquivalenceTest : public LibExprTest
{
protected:
    static ref<SourceRoot> mkRoot(SourceRootKind kind)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        return SourceRoot::make(acc.cast<SourceAccessor>(), kind);
    }

    Value * mkPathVal(ref<SourceRoot> root, const std::string & p)
    {
        auto v = state.mem.allocValue();
        v->mkPath(RootedPath{root, CanonPath(p)}, state.mem);
        return v;
    }

    Value * mkStringVal(std::string_view s)
    {
        auto v = state.mem.allocValue();
        v->mkString(s, state.mem);
        return v;
    }

    /* Synthesize a canonical store-path string with a 32-char
       nix32 hash + the name `source` + optional canonical
       subpath (must start with `/` if present, no trailing
       slash, no `.` / `..` components). */
    Value * mkStoreString(std::string_view hash, std::string_view subpath = "")
    {
        std::string s = std::string{state.store->storeDir} + "/" + std::string{hash} + "-source";
        if (!subpath.empty())
            s += subpath;
        return mkStringVal(s);
    }

    std::strong_ordering call(Value & a, Value & b, PathEquivalenceContext * ctx = nullptr)
    {
        return state.compareForToStringEquivalence(a, b, noPos, "", ctx);
    }

    /* Mount a memory accessor at the given store path in
       `storeFS`, so that `storeFS->getMount(canonStorePath)`
       resolves to it. Step 3 of the comparator bridges Camp A
       strings / System-in-store paths to an accessor via this
       call. */
    void mountAtStorePath(std::string_view hash, ref<MemorySourceAccessor> accessor)
    {
        auto pathStr = std::string{state.store->storeDir} + "/" + std::string{hash} + "-source";
        auto sp = state.store->parseStorePath(pathStr);
        state.storeFS->mount(
            CanonPath(state.store->printStorePath(sp)), [accessor]() { return accessor.cast<SourceAccessor>(); });
    }

    /* Build an in-memory accessor with a single marker file at
       its root, so two such accessors with distinct markers
       have distinct content (and so `contentsEqual` returns
       false between them). */
    ref<MemorySourceAccessor> mkAccessorWithMarker(std::string_view marker)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink sink{*acc};
        sink.createDirectory(CanonPath::root);
        sink.createRegularFile(CanonPath("/marker"), [&](auto & f) { f(marker); });
        return acc;
    }

    /* 32-char nix32 hashes for synthetic store paths in tests. */
    static constexpr auto h1 = "00000000000000000000000000000000";
    static constexpr auto h2 = "11111111111111111111111111111111";
};

/* ============================================================
 *  Camp partition: A vs B
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, nonStoreStringLessThanStoreShapedString)
{
    /* "/etc/passwd" can never be `toString` of a Copyable; Camp B.
       A canonical store-shape string is Camp A. Camp B < Camp A. */
    auto * b = mkStringVal("/etc/passwd");
    auto * a = mkStoreString(h1, "/file");
    EXPECT_EQ(call(*b, *a), std::strong_ordering::less);
    EXPECT_EQ(call(*a, *b), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, nonStoreSystemLessThanCopyable)
{
    /* A System path with an abspath outside storeDir is Camp B.
       A Copyable is always Camp A. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto copRoot = mkRoot(SourceRootKind::Copyable);
    auto * b = mkPathVal(sysRoot, "/etc/passwd");
    auto * a = mkPathVal(copRoot, "/");
    EXPECT_EQ(call(*b, *a), std::strong_ordering::less);
    EXPECT_EQ(call(*a, *b), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, nonStoreSystemLessThanStoreShapedString)
{
    /* Cross-type: Camp B System path vs Camp A store-shaped
       string. Camp ordering decides regardless of type. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto * b = mkPathVal(sysRoot, "/etc/foo");
    auto * a = mkStoreString(h1, "/file");
    EXPECT_EQ(call(*b, *a), std::strong_ordering::less);
    EXPECT_EQ(call(*a, *b), std::strong_ordering::greater);
}

/* ============================================================
 *  Camp B internal: byte lex on the underlying bytes
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, campBStringsByByteOrder)
{
    auto * a = mkStringVal("/aaa");
    auto * b = mkStringVal("/bbb");
    EXPECT_EQ(call(*a, *b), std::strong_ordering::less);
    EXPECT_EQ(call(*b, *a), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, campBSystemPathsByByteOrder)
{
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto * a = mkPathVal(sysRoot, "/aaa");
    auto * b = mkPathVal(sysRoot, "/bbb");
    EXPECT_EQ(call(*a, *b), std::strong_ordering::less);
    EXPECT_EQ(call(*b, *a), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, campBStringAndSystemSameBytesEquivalent)
{
    /* String "/etc/passwd" and System path "/etc/passwd"
       produce identical `toString` output → equivalent under
       this relation. Both Camp B; same underlying bytes. */
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto * sys = mkPathVal(sysRoot, "/etc/passwd");
    auto * str = mkStringVal("/etc/passwd");
    EXPECT_EQ(call(*sys, *str), std::strong_ordering::equal);
    EXPECT_EQ(call(*str, *sys), std::strong_ordering::equal);
}

/* ============================================================
 *  Camp A admission: must be byte-for-byte `toString` output
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, trailingSlashFallsToCampB)
{
    /* `toString` of a Copyable never produces a trailing
       slash; a string with one cannot be Camp A. Falls to
       Camp B, compares less than any Camp A value. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source/");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, dotComponentFallsToCampB)
{
    /* `toString` never emits `/.`; non-canonical. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source/.");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, dotDotComponentFallsToCampB)
{
    /* `toString` never emits `/foo/..`; non-canonical. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source/foo/..");
    auto * canonA = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, doubleSlashFallsToCampB)
{
    /* `toString` never emits `//`; non-canonical. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source//foo");
    auto * canonA = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, shortHashFallsToCampB)
{
    /* `storeDir + "/hi"` isn't a valid store-path shape (hash too
       short). Whole thing is just bytes; Camp B. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/hi");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, nonSourceNameFallsToCampB)
{
    /* Copyable always materialises under name `source`; a store
       path with a different name can't be equivalent to any
       Copyable. Treated as Camp B for our purposes. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-foo");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, campAAcceptsCanonicalRootOnly)
{
    /* No subpath, just `storeDir/hash-source` — canonical
       Copyable toString for the root. Should land in Camp A,
       so compares greater than any Camp B value. */
    auto * canonA = mkStoreString(h1);
    auto * b = mkStringVal("/etc/passwd");
    EXPECT_EQ(call(*canonA, *b), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, campAAcceptsCanonicalWithSubpath)
{
    auto * canonA = mkStoreString(h1, "/some/file");
    auto * b = mkStringVal("/etc/passwd");
    EXPECT_EQ(call(*canonA, *b), std::strong_ordering::greater);
}

/* ----- Camp A admission edge cases (parser helpers) ------------- */

TEST_F(CompareForToStringEquivalenceTest, hashWithExcludedNix32CharFallsToCampB)
{
    /* The nix32 alphabet omits `e`, `o`, `t`, `u`. A hash slot
       containing any of those can't be a real Copyable's hash,
       so the input isn't a canonical toString. Pin each excluded
       char individually so a regression in the alphabet check
       (e.g. accidentally accepting all hex+lowercase) is
       caught. */
    auto storeDir = std::string{state.store->storeDir};
    auto * canonA = mkStoreString(h1);
    for (char c : std::string_view{"eotu"}) {
        std::string hash(32, '0');
        hash[15] = c;
        auto * nonCanon = mkStringVal(storeDir + "/" + hash + "-source");
        EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less)
            << "expected hash containing '" << c << "' to fall to Camp B";
    }
}

TEST_F(CompareForToStringEquivalenceTest, hashTooLongFallsToCampB)
{
    /* 33-char hash slot: even if every char is in the nix32
       alphabet, the length disqualifies. */
    auto storeDir = std::string{state.store->storeDir};
    std::string tooLong(33, '0');
    auto * nonCanon = mkStringVal(storeDir + "/" + tooLong + "-source");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, dotComponentInMiddleFallsToCampB)
{
    /* `/./foo` and `/foo/./bar` would canonicalise to `/foo` and
       `/foo/bar` respectively — neither is what `toString` would
       emit, so Camp B. Tests the in-middle case, distinct from
       the `/.` at the trailing edge already covered. */
    auto storeDir = std::string{state.store->storeDir};
    auto * canonA = mkStoreString(h1, "/foo");
    auto * leading = mkStringVal(storeDir + "/" + h1 + "-source/./foo");
    auto * middle = mkStringVal(storeDir + "/" + h1 + "-source/foo/./bar");
    EXPECT_EQ(call(*leading, *canonA), std::strong_ordering::less);
    EXPECT_EQ(call(*middle, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, dotComponentAtEndAloneFallsToCampB)
{
    /* `/.` at the very end (no trailing `/`) is also rejected;
       complements `/foo/..` and the trailing-slash cases. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source/foo/.");
    auto * canonA = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

TEST_F(CompareForToStringEquivalenceTest, bareTrailingSlashSubpathFallsToCampB)
{
    /* Subpath of exactly `/` (no name after the slash) is a
       trailing-slash form `toString` never emits — Copyable's
       root toString has no trailing slash at all. */
    auto storeDir = std::string{state.store->storeDir};
    auto * nonCanon = mkStringVal(storeDir + "/" + h1 + "-source/");
    auto * canonA = mkStoreString(h1);
    EXPECT_EQ(call(*nonCanon, *canonA), std::strong_ordering::less);
}

/* ============================================================
 *  Camp A subpath compare: subpath drives ordering regardless
 *  of the hash slot (hash → step 3, deferred)
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, sameHashSubpathOrder)
{
    auto * a = mkStoreString(h1, "/a");
    auto * b = mkStoreString(h1, "/b");
    EXPECT_EQ(call(*a, *b), std::strong_ordering::less);
    EXPECT_EQ(call(*b, *a), std::strong_ordering::greater);
}

TEST_F(CompareForToStringEquivalenceTest, subpathDrivesOrderRegardlessOfHash)
{
    /* h2 > h1 lex, but subpath compare comes first in the
       layered ordering. "/a" < "/b" decides without ever
       looking at the hash slot. */
    auto * h2a = mkStoreString(h2, "/a");
    auto * h1b = mkStoreString(h1, "/b");
    EXPECT_EQ(call(*h2a, *h1b), std::strong_ordering::less);
    EXPECT_EQ(call(*h1b, *h2a), std::strong_ordering::greater);
}

/* ============================================================
 *  Failure modes
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, internalPathRejected)
{
    /* `toString` is undefined for Internal; the comparator
       throws upfront (mirrors `coerceToString`'s Internal arm). */
    auto intRoot = mkRoot(SourceRootKind::Internal);
    auto sysRoot = mkRoot(SourceRootKind::System);
    auto * intP = mkPathVal(intRoot, "/foo");
    auto * sysP = mkPathVal(sysRoot, "/foo");
    EXPECT_THROW(call(*intP, *sysP), EvalError);
    EXPECT_THROW(call(*sysP, *intP), EvalError);
}

TEST_F(CompareForToStringEquivalenceTest, nonPathOrStringRejected)
{
    /* Only path and string are accepted. Other types throw. */
    auto * num = state.mem.allocValue();
    num->mkInt(42);
    auto * str = mkStringVal("foo");
    EXPECT_THROW(call(*num, *str), EvalError);
    EXPECT_THROW(call(*str, *num), EvalError);
}

/* ============================================================
 *  Step 3: source root equivalence class.
 *  Only fires when step 2 returns equal within Camp A. The
 *  comparator looks up an equivalence-class id per side:
 *    - Copyable nPath: `classOfAccessor` on its accessor.
 *    - String / System-in-store: parse to a StorePath and
 *      `storeFS->getMount` to obtain an accessor, then
 *      `classOfAccessor`. Throws if not mounted.
 *  Step 3 only fires when a `PathEquivalenceContext *` is
 *  passed; without ctx, step 2's equal verdict is taken as
 *  equivalent.
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, withCtx_sameHashSubpathEquivalent)
{
    /* Two strings referencing the same store path resolve to
       the same mounted accessor → same classId → Equivalent. */
    PathEquivalenceContext ctx{state};
    mountAtStorePath(h1, mkAccessorWithMarker("h1-content"));
    auto * a = mkStoreString(h1, "/foo");
    auto * b = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*a, *b, &ctx), std::strong_ordering::equal);
}

TEST_F(CompareForToStringEquivalenceTest, withCtx_differentHashSameSubpathDistinct)
{
    /* Same subpath puts them in step 3; different hashes resolve
       to different mounted accessors with different content →
       distinct classIds → not equivalent, with a stable order. */
    PathEquivalenceContext ctx{state};
    mountAtStorePath(h1, mkAccessorWithMarker("h1-content"));
    mountAtStorePath(h2, mkAccessorWithMarker("h2-content"));
    auto * a = mkStoreString(h1, "/foo");
    auto * b = mkStoreString(h2, "/foo");
    auto cmp = call(*a, *b, &ctx);
    EXPECT_NE(cmp, std::strong_ordering::equal);
    /* Symmetry: a < b ⟺ b > a. */
    EXPECT_EQ(call(*b, *a, &ctx), 0 <=> cmp);
}

TEST_F(CompareForToStringEquivalenceTest, withCtx_copyableSameAccessorEquivalent)
{
    /* Two Copyable paths wrapping the same accessor share the
       same classId via `classOfAccessor`'s cached lookup — no
       storeFS bridge needed for the Copyable side. */
    PathEquivalenceContext ctx{state};
    auto acc = mkAccessorWithMarker("shared-content");
    auto r1 = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto r2 = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto * a = mkPathVal(r1, "/foo");
    auto * b = mkPathVal(r2, "/foo");
    EXPECT_EQ(call(*a, *b, &ctx), std::strong_ordering::equal);
}

TEST_F(CompareForToStringEquivalenceTest, withCtx_copyableDistinctContentDistinct)
{
    /* Two Copyable paths wrapping distinct accessors with
       distinct content → classOfAccessor's contentsEqual scan
       puts them in different classes → distinct. */
    PathEquivalenceContext ctx{state};
    auto a_acc = mkAccessorWithMarker("a-content");
    auto b_acc = mkAccessorWithMarker("b-content");
    auto rA = SourceRoot::make(a_acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto rB = SourceRoot::make(b_acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto * a = mkPathVal(rA, "/foo");
    auto * b = mkPathVal(rB, "/foo");
    auto cmp = call(*a, *b, &ctx);
    EXPECT_NE(cmp, std::strong_ordering::equal);
    EXPECT_EQ(call(*b, *a, &ctx), 0 <=> cmp);
}

TEST_F(CompareForToStringEquivalenceTest, withCtx_copyableMatchesMountedStringEquivalent)
{
    /* Cross-representation: a Copyable nPath and a Camp A
       string both mapping to the same underlying accessor —
       string via the storeFS mount, Copyable directly via its
       root accessor — should share a classId. The mount uses
       the same accessor object the Copyable is rooted on, so
       `classOfAccessor` lookups on either resolve to the same
       cached id. */
    PathEquivalenceContext ctx{state};
    auto acc = mkAccessorWithMarker("shared");
    mountAtStorePath(h1, acc);
    auto copRoot = SourceRoot::make(acc.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto * copPath = mkPathVal(copRoot, "/foo");
    auto * mountedString = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*copPath, *mountedString, &ctx), std::strong_ordering::equal);
    EXPECT_EQ(call(*mountedString, *copPath, &ctx), std::strong_ordering::equal);
}

TEST_F(CompareForToStringEquivalenceTest, withCtx_unmountedStorePathThrows)
{
    /* Step 3 reached, but neither string's parsed store path is
       mounted in storeFS nor present in the underlying store →
       throws `InvalidPath` via `Store::requireStoreObjectAccessor`,
       the genuinely-undecidable case per the never-return-wrong-
       answers contract. The comparator decorates the error so
       the user sees what operation was attempted; pin both. */
    PathEquivalenceContext ctx{state};
    auto * a = mkStoreString(h1, "/foo");
    auto * b = mkStoreString(h2, "/foo");
    try {
        call(*a, *b, &ctx);
        FAIL() << "expected InvalidPath throw";
    } catch (InvalidPath & e) {
        std::string msg{e.what()};
        EXPECT_THAT(msg, ::testing::HasSubstr("is not a valid store path"));
        EXPECT_THAT(msg, ::testing::HasSubstr("equivalence against a fetched source"));
    }
}

TEST_F(CompareForToStringEquivalenceTest, withoutCtx_sameHashSubpathTakenAsEquivalent)
{
    /* Without a ctx, step 3 doesn't fire and step 2's equal
       verdict stands as the equivalence outcome. Pin so future
       changes don't accidentally throw or wedge on this case. */
    auto * a = mkStoreString(h1, "/foo");
    auto * b = mkStoreString(h1, "/foo");
    EXPECT_EQ(call(*a, *b), std::strong_ordering::equal);
}

/* ============================================================
 *  `CompareValues` (the comparator backing
 *  `PathEquivalentDedup`'s otherMap) — strict-weak-order
 *  properties.
 *
 *  Calling `compareForToStringEquivalence` directly tests only
 *  the inner comparator (which is internally consistent). The
 *  bug lived in `CompareValues` *mixing* schemes — byte compare
 *  for same-type strings vs classId compare for cross-type —
 *  which made `<` non-transitive. To pin that, we have to
 *  invoke `CompareValues` directly via `EvalState::compareValues`.
 * ============================================================ */

TEST_F(CompareForToStringEquivalenceTest, comparatorIsTransitiveAcrossMixedRepresentations)
{
    /* Pin the `<` transitivity violation that lived in the
       comparator when same-type strings used byte compare and
       cross-type path × string used classId. Three Camp A values
       sharing the same subpath:

         X = string at h1 (mounted to acc_alpha).
         Y = string at h2 (mounted to acc_beta).
         Z = Copyable nPath with acc_gamma.

       All accessors content-distinct so classification gives
       three distinct classIds; h1 < h2 byte-wise so the
       string × string byte compare puts X < Y.

       Call order forces a specific classId assignment:
         compareValues(X, Y) — classifies X first (id 1), then Y
           (id 2). Pre-fix: byte compare, h1 < h2 → TRUE.
           Post-fix: classId compare, 1 < 2 → TRUE.
         compareValues(Y, Z) — Y cached (id 2), Z classified
           (id 3). Both pre and post fix: classId 2 < 3 → TRUE.
         compareValues(X, Z) — X cached (id 1), Z cached (id 3).
           Both pre and post fix: classId 1 < 3 → TRUE.

       Strict weak order requires `X < Y && Y < Z → X < Z`. The
       fix makes all three comparisons share one comparator
       (classId), so this holds.

       *Pre-fix counterexample (different call order)*: if
       compareValues(Y, Z) is invoked *before* compareValues(X, Y),
       Y gets classId 1 and Z gets classId 2; then
       compareValues(X, Y) under byte gives TRUE (h1 < h2), but
       compareValues(X, Z) under classId classifies X as id 3 →
       3 < 2 → FALSE. X < Y AND Y < Z but NOT(X < Z) — the
       strict-weak-order break. */
    PathEquivalenceContext ctx{state};
    mountAtStorePath(h1, mkAccessorWithMarker("alpha"));
    mountAtStorePath(h2, mkAccessorWithMarker("beta"));
    auto acc_gamma = mkAccessorWithMarker("gamma");
    auto rootZ = SourceRoot::make(acc_gamma.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto * Z = mkPathVal(rootZ, "/foo");
    auto * Y = mkStoreString(h2, "/foo");
    auto * X = mkStoreString(h1, "/foo");

    /* Call order that classifies Y first, then Z, then X —
       exposes the pre-fix mixed-comparator break. */
    bool YlessZ = state.compareValues(*Y, *Z, noPos, "", &ctx);
    bool XlessY = state.compareValues(*X, *Y, noPos, "", &ctx);
    bool XlessZ = state.compareValues(*X, *Z, noPos, "", &ctx);

    /* If the comparator is a strict weak order, X < Y AND Y < Z
       imply X < Z. */
    if (XlessY && YlessZ) {
        EXPECT_TRUE(XlessZ) << "< transitivity violated: X<Y && Y<Z but NOT(X<Z)";
    }
}

} // namespace nix

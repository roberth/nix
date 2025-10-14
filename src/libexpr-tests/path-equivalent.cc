#include <gtest/gtest.h>
#include "nix/expr/environment/system.hh"

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/store/store-api.hh"
#include "nix/util/file-system.hh"
#include "nix/util/memory-source-accessor.hh"
#include "nix/util/mounted-source-accessor.hh"

namespace nix {

/* `builtins.genericClosure` with `pathEquivalent = true` enables
   cross-type dedup between path keys and string keys, treating them
   as equal when `toString path == string` would have held. The
   opt-in exists to let the nixpkgs module system stop calling
   `toString` on lazy paths (which would force materialisation).

   The dedup uses three layers:
   - same-type string keys: regular string equality
   - same-type path keys: structural RootedPath compare (unchanged)
   - cross-type path × string: kind-dispatched
       System: P.path.abs() == S
       Copyable: S must be /<storeDir>/<storeBase>[/subpath] with
                 subpath matching P's path; then srcToStore lookup
                 or copyPathToStore on P's root to confirm the
                 storePath matches
       Internal: never equivalent (toString errors on Internal) */

class PathEquivalentTest : public LibExprTest
{
protected:
    ref<SourceRoot> mkRoot(SourceRootKind kind)
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

    /* Build an attrset { key = K; } that genericClosure accepts as
       an element of startSet. The operator returns an empty list,
       so the closure equals the startSet up to dedup. */
    Value * mkElem(Value * key)
    {
        auto bb = state.buildBindings(1);
        bb.insert(state.s.key, key);
        auto v = state.mem.allocValue();
        v->mkAttrs(bb.finish());
        return v;
    }

    /* Build a list Value from prebuilt element Values. Used to
       wrap path/string Values inside a nested complex key. */
    Value * mkListVal(const std::vector<Value *> & elems)
    {
        auto list = state.buildList(elems.size());
        for (const auto & [n, e] : enumerate(elems))
            list[n] = e;
        auto v = state.mem.allocValue();
        v->mkList(list);
        return v;
    }

    /* Build a single-attribute attrset. Used to wrap path/string
       Values inside a nested complex key. */
    Value * mkAttrsValOne(std::string_view name, Value * value)
    {
        auto bb = state.buildBindings(1);
        bb.insert(state.symbols.create(name), value);
        auto v = state.mem.allocValue();
        v->mkAttrs(bb.finish());
        return v;
    }

    /* Synthesize a canonical store-path string with a 32-char
       nix32 hash + the canonical name `source` + optional
       subpath. Matches what `toString` would emit on a Copyable
       materialising to that store path. */
    Value * mkStoreString(std::string_view hash, std::string_view subpath = "")
    {
        std::string s = std::string{state.systemEnvironment->store->storeDir} + "/" + std::string{hash} + "-source";
        if (!subpath.empty())
            s += subpath;
        return mkStringVal(s);
    }

    /* Mount a memory accessor at the given store path in
       `storeFS`, so step 3 of the layered comparator can bridge
       a Camp A string / System-in-store path to that
       accessor via `storeFS->getMount`. */
    void mountAtStorePath(std::string_view hash, ref<MemorySourceAccessor> accessor)
    {
        auto pathStr = std::string{state.systemEnvironment->store->storeDir} + "/" + std::string{hash} + "-source";
        auto sp = state.systemEnvironment->store->parseStorePath(pathStr);
        state.systemEnvironment->storeFS->mount(
            CanonPath(state.systemEnvironment->store->printStorePath(sp)),
            [accessor]() { return accessor.cast<SourceAccessor>(); });
    }

    /* In-memory accessor with a single marker file so that two
       such accessors with distinct markers are content-distinct
       (and `accessorsEquivalent` would compute different
       storePaths for them), and two with the same marker compare
       equal. */
    ref<MemorySourceAccessor> mkAccessorWithMarker(std::string_view marker)
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink sink{*acc};
        sink.createDirectory(CanonPath::root);
        sink.createRegularFile(CanonPath("/marker"), [&](auto & f) { f(marker); });
        return acc;
    }

    /* 32-char nix32 hashes for synthetic store paths. */
    static constexpr auto h1 = "00000000000000000000000000000000";
    static constexpr auto h2 = "11111111111111111111111111111111";

    /* Run genericClosure on a startSet of pre-built elements.
       Returns the result list's size — the dedup count. */
    size_t runClosureSize(const std::vector<Value *> & elems, bool pathEquivalent)
    {
        auto list = state.buildList(elems.size());
        for (const auto & [n, e] : enumerate(elems))
            list[n] = e;
        auto vStart = state.mem.allocValue();
        vStart->mkList(list);

        /* operator: x: []  — i.e. a function that always returns []. */
        Value * vOp = state.mem.allocValue();
        Expr * eOp = state.parseExprFromString("x: []", state.rootedPath(CanonPath::root));
        state.eval(eOp, *vOp);

        auto bb = state.buildBindings(3);
        bb.insert(state.s.startSet, vStart);
        bb.insert(state.s.operator_, vOp);
        if (pathEquivalent) {
            Value * vTrue = state.mem.allocValue();
            vTrue->mkBool(true);
            bb.insert(state.symbols.create("pathEquivalent"), vTrue);
        }
        Value * vArg = state.mem.allocValue();
        vArg->mkAttrs(bb.finish());

        /* Call builtins.genericClosure on our arg. */
        auto * lambda =
            state.parseExprFromString("arg: builtins.genericClosure arg", state.rootedPath(CanonPath::root));
        Value vLambda;
        state.eval(lambda, vLambda);
        Value vResult;
        state.callFunction(vLambda, *vArg, vResult, noPos);
        state.forceList(vResult, noPos, "");
        return vResult.listSize();
    }
};

/* ----- Marker for the feature ----------------------------------- */

TEST_F(PathEquivalentTest, isPathEquivalentPresentAsMarker)
{
    /* The `pathEquivalent = true` option on genericClosure ships
       alongside `builtins.isPathEquivalent`; consumers probe via
       `builtins ? isPathEquivalent` to decide whether the
       optimisation is available. */
    auto v = eval("builtins ? isPathEquivalent");
    ASSERT_EQ(v.type(), nBool);
    EXPECT_TRUE(v.boolean());
}

/* ----- Sanity: same-type dedup unchanged ------------------------- */

TEST_F(PathEquivalentTest, stringDedupUnchanged)
{
    auto * a = mkElem(mkStringVal("/foo"));
    auto * b = mkElem(mkStringVal("/foo"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, pathDedupUnchanged)
{
    auto root = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkPathVal(root, "/foo"));
    auto * b = mkElem(mkPathVal(root, "/foo"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

/* ----- System cross-type: path.abs() == string ------------------- */

TEST_F(PathEquivalentTest, systemPathEquivToMatchingAbsString)
{
    auto root = mkRoot(SourceRootKind::System);
    auto * pathElem = mkElem(mkPathVal(root, "/some/file"));
    auto * stringElem = mkElem(mkStringVal("/some/file"));
    EXPECT_EQ(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, systemPathNotEquivToMismatchedString)
{
    auto root = mkRoot(SourceRootKind::System);
    auto * pathElem = mkElem(mkPathVal(root, "/some/file"));
    auto * stringElem = mkElem(mkStringVal("/other/place"));
    EXPECT_EQ(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), 2u);
}

TEST_F(PathEquivalentTest, systemEquivalenceIsSymmetric)
{
    /* Path inserted first, then string — and vice versa. Both
       orderings must produce a dedup. */
    auto root = mkRoot(SourceRootKind::System);
    auto stringFirst = runClosureSize({mkElem(mkStringVal("/some/file")), mkElem(mkPathVal(root, "/some/file"))}, true);
    auto pathFirst = runClosureSize({mkElem(mkPathVal(root, "/some/file")), mkElem(mkStringVal("/some/file"))}, true);
    EXPECT_EQ(stringFirst, 1u);
    EXPECT_EQ(pathFirst, 1u);
}

/* ----- Path × path dedup respects toString-equivalence ---------- */

TEST_F(PathEquivalentTest, systemDistinctAccessorsSameSubpathDedup)
{
    /* Two System paths with the same subpath on distinct
       accessors have the same toString (the abspath) → they're
       equivalent. PathEquivalentDedup must collapse them. The
       previous impl keyed on the accessor's monotonic `number`,
       which is identity and so missed this case. */
    auto rootA = mkRoot(SourceRootKind::System);
    auto rootB = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkPathVal(rootA, "/x"));
    auto * b = mkElem(mkPathVal(rootB, "/x"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, copyableDistinctAccessorsContentsEqualDedup)
{
    /* Two Copyable accessors with NAR-equal roots materialise to
       the same store path, so their toStrings (storeBase +
       subpath) agree. They share a toString-equivalence class
       under PathEquivalentDedup's classification and dedup
       collapses them. Without this, every fetched-tree copy
       with identical contents would survive the closure
       separately — defeating the point of pathEquivalent for
       any cross-input dedup. */
    auto accA = make_ref<MemorySourceAccessor>();
    MemorySink{*accA}.createDirectory(CanonPath::root);
    accA->addFile(CanonPath("/f"), "same contents");
    auto rootA = SourceRoot::make(accA.cast<SourceAccessor>(), SourceRootKind::Copyable);

    auto accB = make_ref<MemorySourceAccessor>();
    MemorySink{*accB}.createDirectory(CanonPath::root);
    accB->addFile(CanonPath("/f"), "same contents");
    auto rootB = SourceRoot::make(accB.cast<SourceAccessor>(), SourceRootKind::Copyable);

    auto * a = mkElem(mkPathVal(rootA, "/f"));
    auto * b = mkElem(mkPathVal(rootB, "/f"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, copyableDistinctAccessorsContentsDifferStaySeparate)
{
    /* Same shape but with diverging contents. Different NAR
       hashes → different toString prefixes → distinct
       equivalence classes → kept separate. Pins that the
       contents-equality check is the discriminator (not e.g.
       same-subpath-alone). */
    auto accA = make_ref<MemorySourceAccessor>();
    MemorySink{*accA}.createDirectory(CanonPath::root);
    accA->addFile(CanonPath("/f"), "A");
    auto rootA = SourceRoot::make(accA.cast<SourceAccessor>(), SourceRootKind::Copyable);

    auto accB = make_ref<MemorySourceAccessor>();
    MemorySink{*accB}.createDirectory(CanonPath::root);
    accB->addFile(CanonPath("/f"), "B");
    auto rootB = SourceRoot::make(accB.cast<SourceAccessor>(), SourceRootKind::Copyable);

    auto * a = mkElem(mkPathVal(rootA, "/f"));
    auto * b = mkElem(mkPathVal(rootB, "/f"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 2u);
}

/* ----- Internal: throws when compared cross-type ----------------- */

TEST_F(PathEquivalentTest, internalThrowsOnCrossTypeCompare)
{
    /* `toString` on an Internal-kinded path errors (see
       coerceToString's Internal arm). Semantic-toString equivalence
       inherits that: a comparison against a string is undefined,
       and silently saying "not equivalent" would mask a usage bug.
       Pin that the comparison throws — and that the error
       references the offending path and string. */
    auto root = mkRoot(SourceRootKind::Internal);
    auto * pathElem = mkElem(mkPathVal(root, "/anything"));
    auto * stringElem = mkElem(mkStringVal("/anything"));
    EXPECT_THROW(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), EvalError);
}

TEST_F(PathEquivalentTest, internalThrowsEvenAlone)
{
    /* Two Internal paths in the startSet (without any string).
       The single-map dedup routes every comparison through
       `compareForToStringEquivalence`, which refuses Internal at
       classify() — Internal has no toString, so toString-
       equivalence is undefined for it even on the same accessor.
       Internal-as-pathEquivalent-key is a usage error; throwing
       surfaces it instead of silently dedup'ing under a relation
       (toString equivalence) that's undefined for Internal. */
    auto root = mkRoot(SourceRootKind::Internal);
    auto * a = mkElem(mkPathVal(root, "/x"));
    auto * b = mkElem(mkPathVal(root, "/x"));
    EXPECT_THROW(runClosureSize({a, b}, /*pathEquivalent=*/true), EvalError);
}

/* ----- Copyable: shape-reject when string isn't a store path ----- */

TEST_F(PathEquivalentTest, copyableRejectsNonStorePathShape)
{
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * pathElem = mkElem(mkPathVal(root, "/some/file.nix"));
    /* Random absolute path that isn't under the store dir. */
    auto * stringElem = mkElem(mkStringVal("/etc/passwd"));
    EXPECT_EQ(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), 2u);
}

TEST_F(PathEquivalentTest, copyableRejectsMismatchedSubpath)
{
    /* String IS store-shaped, but its subpath inside the store path
       doesn't match the lazy path's CanonPath. Cheap reject (no I/O
       on the store side). */
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * pathElem = mkElem(mkPathVal(root, "/modules/foo.nix"));
    auto storeDir = state.systemEnvironment->store->storeDir;
    /* Synthesize a syntactically valid store path with a bogus
       subpath. Use a plausible-shape hash + name. We don't need
       the on-disk path to exist for the *reject* branch — that
       branch returns early on subpath mismatch. */
    auto * stringElem = mkElem(mkStringVal(storeDir + "/00000000000000000000000000000000-fake/modules/bar.nix"));
    EXPECT_EQ(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), 2u);
}

/* ----- Nested path-equivalence inside lists / attrsets ---------- */

TEST_F(PathEquivalentTest, listOfPathDedupsWithListOfEquivalentString)
{
    /* The genericClosure key is a list whose lone element is a
       path on one side and the toString-equivalent string on
       the other. Under `pathEquivalent = true` they should
       collapse: the ctx-aware comparator recurses into the
       lists, sees a path × string pair at the same position,
       and the sorted otherMap puts them in the same key bucket
       via the "neither less" Equivalence — same as if the
       path/string were the top-level key. */
    auto root = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkListVal({mkPathVal(root, "/x")}));
    auto * b = mkElem(mkListVal({mkStringVal("/x")}));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, attrsetOfPathDedupsWithAttrsetOfEquivalentString)
{
    /* Same idea inside an attrset. CompareValues has no native
       attrset ordering, but under ctx the default arm falls back
       to eqValues for equivalence (which DOES walk attrsets and
       sees the path × string equivalence at the leaf) — the
       sorted otherMap puts the two attrsets in the same key
       bucket via the "neither less" Equivalence. */
    auto root = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkAttrsValOne("file", mkPathVal(root, "/m.nix")));
    auto * b = mkElem(mkAttrsValOne("file", mkStringVal("/m.nix")));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
}

TEST_F(PathEquivalentTest, nestedNonEquivalentStaysSeparate)
{
    /* Mirror case: the path and string at the same position do
       NOT toString-equivalently match. The dedup must keep both
       entries — pin that the relaxation is "treat equivalent
       as equal" not "treat any path × string as equal". */
    auto root = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkListVal({mkPathVal(root, "/x")}));
    auto * b = mkElem(mkListVal({mkStringVal("/y")}));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 2u);
}

TEST_F(PathEquivalentTest, nestedPathStringWithoutOptInThrows)
{
    /* Without `pathEquivalent = true`, the otherMap uses the
       plain `CompareValues` which throws on path × string. Pin
       so the opt-in is the only switch that enables the
       cross-type tolerance — `==` / `sort` / etc. outside of
       genericClosure pathEquivalent stay strict. */
    auto root = mkRoot(SourceRootKind::System);
    auto * a = mkElem(mkListVal({mkPathVal(root, "/x")}));
    auto * b = mkElem(mkListVal({mkStringVal("/x")}));
    EXPECT_THROW(runClosureSize({a, b}, /*pathEquivalent=*/false), EvalError);
}

/* ----- Backward compat: without opt-in, mixing types throws ------ */

/* ----- Step-3 wire-up: transitivity across representations ------ */

TEST_F(PathEquivalentTest, transitivityAcrossRepresentations)
{
    /* Pin issue 2: the comparator's equivalence relation must be
       transitive. With cross-type routed through
       `compareForToStringEquivalence` (issue 1 fix) but same-type
       still using language `<` semantics, three values can be
       pairwise equivalent under cross-type but not under string ×
       string byte-compare — non-transitivity that `std::map`'s
       tree invariants rely on.

       Setup: three lists wrapping three representations of the
       same content X — a Copyable nPath, and two Camp A strings
       at different hashes that bridge (via storeFS mount) to
       content-equal accessors.

       Pairwise equivalence under the current (post-issue-1)
       comparator:
         A vs B: cross-type with ctx → step 3 same class → ≡.
         A vs C: cross-type with ctx → step 3 same class → ≡.
         B vs C: string × string at leaf → byte compare on the
                 store-path bytes → DIFFERENT bytes → ≢.

       Non-transitive. Insertion order [B, C, A] reveals it:
       B and C are byte-distinct → both inserted; A is then
       deduped against B and the equivalence with C is missed.
       Size 2 instead of 1. */
    auto acc1 = mkAccessorWithMarker("shared");
    auto acc2 = mkAccessorWithMarker("shared");
    mountAtStorePath(h1, acc1);
    mountAtStorePath(h2, acc2);

    auto rootA = SourceRoot::make(acc1.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto mkA = [&]() { return mkElem(mkListVal({mkPathVal(rootA, "/foo")})); };
    auto mkB = [&]() { return mkElem(mkListVal({mkStoreString(h1, "/foo")})); };
    auto mkC = [&]() { return mkElem(mkListVal({mkStoreString(h2, "/foo")})); };

    /* All three pairwise equivalent; all permutations must dedup
       to the same single bucket. */
    EXPECT_EQ(runClosureSize({mkA(), mkB(), mkC()}, /*pathEquivalent=*/true), 1u);
    EXPECT_EQ(runClosureSize({mkB(), mkC(), mkA()}, /*pathEquivalent=*/true), 1u);
    EXPECT_EQ(runClosureSize({mkC(), mkA(), mkB()}, /*pathEquivalent=*/true), 1u);
}

/* ----- Step-3 wire-up: cross-type with different store paths ---- */

TEST_F(PathEquivalentTest, nestedCrossTypeDifferentStorePathsStaysDistinct)
{
    /* Pin issue 1: the `CompareValues` cross-type path × string arm
       must pass `ctx` so that `compareForToStringEquivalence`'s
       step 3 actually fires. Without ctx, two Camp A leaves with the
       same subpath but referencing different store paths wrongly
       dedup at step 2's "subpath equal" verdict.

       Setup: a Copyable nPath that materialises (via the mount) to
       store path h1, and a Camp A string that resolves to h2. Both
       hashes have content-distinct accessors, so step 3 should put
       them in different equivalence classes → distinct keys. */
    auto acc1 = mkAccessorWithMarker("h1-content");
    auto acc2 = mkAccessorWithMarker("h2-content");
    mountAtStorePath(h1, acc1);
    mountAtStorePath(h2, acc2);

    auto root1 = SourceRoot::make(acc1.cast<SourceAccessor>(), SourceRootKind::Copyable);
    auto * pathH1 = mkPathVal(root1, "/foo");
    auto * stringH2 = mkStoreString(h2, "/foo");

    /* Keys are lists, forcing the cross-type comparison through
       `otherMap`'s `CompareValues` recursion at the leaf rather
       than the top-level pathMap/stringMap cross-arm. */
    auto * a = mkElem(mkListVal({pathH1}));
    auto * b = mkElem(mkListVal({stringH2}));

    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 2u);
}

TEST_F(PathEquivalentTest, withoutOptInPathStringMixedThrows)
{
    /* Today's behaviour: CompareValues throws on type mismatch and
       genericClosure propagates. Pin so the new opt-in doesn't
       silently change the default. */
    auto root = mkRoot(SourceRootKind::System);
    auto * pathElem = mkElem(mkPathVal(root, "/x"));
    auto * stringElem = mkElem(mkStringVal("/x"));
    EXPECT_THROW(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/false), EvalError);
}

/* ----- Copyable hint heuristic over a real on-disk tree ---------- */

class PathEquivalentCopyableFSTest : public PathEquivalentTest
{
protected:
    std::filesystem::path tmpDir;
    std::unique_ptr<AutoDelete> delTmpDir;

    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = std::make_unique<AutoDelete>(tmpDir, true);
    }

    void TearDown() override
    {
        delTmpDir.reset();
    }
};

TEST_F(PathEquivalentCopyableFSTest, copyableUnmountedStorePathThrows)
{
    /* A string side that names a store path which isn't mounted
       in `storeFS` and isn't present in the underlying store
       (the all-zeros hash). Equivalence is genuinely undecidable
       — short of IFD-style materialisation that returns a
       content-addressed answer — and the never-return-wrong-
       answers contract demands a throw rather than a guess in
       either direction. `EvalState::storePathAccessor` surfaces
       this as `InvalidPath`; `compareForToStringEquivalence`
       decorates it with the operation context. */
    auto storeDir = state.systemEnvironment->store->storeDir;
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * pathElem = mkElem(mkPathVal(root, "/modules/foo.nix"));
    auto * stringElem = mkElem(mkStringVal(storeDir + "/00000000000000000000000000000000-source/modules/foo.nix"));
    EXPECT_THROW(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), InvalidPath);
}

} // namespace nix

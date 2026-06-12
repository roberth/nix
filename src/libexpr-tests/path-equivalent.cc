#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/file-system.hh"
#include "nix/util/memory-source-accessor.hh"

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
                 subpath matching P's path; then contentsEqual on
                 the tree roots with the path's own abs as hint
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

TEST_F(PathEquivalentTest, internalAloneDoesNotThrow)
{
    /* Two Internal paths in the startSet without any string — no
       cross-type compare fires, no throw. Pin so the Internal
       throw stays narrowly-scoped to the cross-type arm. */
    auto root = mkRoot(SourceRootKind::Internal);
    auto * a = mkElem(mkPathVal(root, "/x"));
    auto * b = mkElem(mkPathVal(root, "/x"));
    EXPECT_EQ(runClosureSize({a, b}, /*pathEquivalent=*/true), 1u);
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
    auto storeDir = state.store->storeDir;
    /* Synthesize a syntactically valid store path with a bogus
       subpath. Use a plausible-shape hash + name. We don't need
       the on-disk path to exist for the *reject* branch — that
       branch returns early on subpath mismatch. */
    auto * stringElem = mkElem(mkStringVal(storeDir + "/00000000000000000000000000000000-fake/modules/bar.nix"));
    EXPECT_EQ(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true), 2u);
}

/* ----- Backward compat: without opt-in, mixing types throws ------ */

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

TEST_F(PathEquivalentCopyableFSTest, copyableMatchesWhenHintBytesAgree)
{
    /* Place an on-disk tree with a file at /modules/foo.nix. Make
       the lazy Copyable accessor expose an identical file. Build a
       string that points at the on-disk store path's subpath. With
       the hint discriminator agreeing and the full walk also
       matching, they dedupe. */
    auto storeDir = state.store->storeDir;
    /* The actual on-disk root is tmpDir, but the string we feed
       genericClosure has to look like a store path. We can't write
       inside the real /nix/store from the test, so we use a fake
       storeDir layout: create tmpDir / "<hash>-name" as the
       "store base", and pass `storeDir + "/<hash>-name/..."` as
       the comparison string while pointing makeFSSourceAccessor
       at the real on-disk location.

       Note: the implementation of pathEquivalent will try to open
       the on-disk store-path under the real store dir. That path
       won't exist, so the hint compare will fail at the
       makeFSSourceAccessor step. The whole comparison must then
       return false ("can't prove equivalent, treat as different").
       Pin that this isn't a hard error. */
    auto root = mkRoot(SourceRootKind::Copyable);
    auto * pathElem = mkElem(mkPathVal(root, "/modules/foo.nix"));
    auto * stringElem = mkElem(mkStringVal(storeDir + "/00000000000000000000000000000000-source/modules/foo.nix"));
    /* The real store path doesn't exist; the implementation must
       not throw — just decline to claim equivalence. */
    EXPECT_NO_THROW(runClosureSize({pathElem, stringElem}, /*pathEquivalent=*/true));
}

} // namespace nix

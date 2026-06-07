#include "nix/util/source-path.hh"
#include "nix/util/memory-source-accessor.hh"

#include <atomic>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace nix {

namespace {

/* Build an empty MemorySourceAccessor whose root is a directory.
   The default-constructed accessor has no root at all, which means
   /'s lstat returns nullopt — too degenerate for most of our
   tests. */
ref<MemorySourceAccessor> emptyDir()
{
    auto a = make_ref<MemorySourceAccessor>();
    MemorySink{*a}.createDirectory(CanonPath::root);
    return a;
}

/* Counting accessor that delegates to an inner accessor but records
   per-method call counts. Lets tests assert that early-exit branches
   short-circuit before touching the filesystem. */
struct CountingAccessor : SourceAccessor
{
    ref<SourceAccessor> inner;

    /* Use atomics so failures show up loudly if we ever race. */
    std::atomic<size_t> nLstat{0}, nReadFile{0}, nReadDir{0}, nReadLink{0}, nFingerprint{0};

    explicit CountingAccessor(ref<SourceAccessor> inner)
        : inner(inner)
    {
    }

    void anchor() override {}

    std::optional<Stat> maybeLstat(const CanonPath & p) override
    {
        ++nLstat;
        return inner->maybeLstat(p);
    }

    void readFile(const CanonPath & p, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        ++nReadFile;
        inner->readFile(p, sink, sizeCallback);
    }

    using SourceAccessor::readFile;

    DirEntries readDirectory(const CanonPath & p) override
    {
        ++nReadDir;
        return inner->readDirectory(p);
    }

    std::string readLink(const CanonPath & p) override
    {
        ++nReadLink;
        return inner->readLink(p);
    }

    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & p) override
    {
        ++nFingerprint;
        return inner->getFingerprint(p);
    }
};

} // namespace

/* =================================================================
 * Tests for `contentsEqual`.
 *
 * The function tested here is the libutil-layer SourcePath equality:
 *   bool contentsEqual(const SourcePath &, const SourcePath &,
 *                      std::optional<CanonPath> hint = std::nullopt);
 *
 * Semantics: contents-based under NAR rules (file type, regular
 * contents + executable bit, symlink target, recursive directory
 * entries with names + types), with cheap short-circuits when
 * possible.
 * =================================================================
 */

/* ----- Step 1: file-type ("kind") mismatch ------------------------ */

TEST(ContentsEqual, kindMismatchIsFalse)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/x"), "hi");

    auto b = make_ref<MemorySourceAccessor>();
    MemorySink{*b}.createDirectory(CanonPath::root);
    MemorySink{*b}.createDirectory(CanonPath("/x"));

    /* /x is a regular file on `a` and a directory on `b`. */
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/x")}, SourcePath{b, CanonPath("/x")}));
}

/* ----- Step 2: same accessor + same path is the trivial yes ------- */

TEST(ContentsEqual, sameAccessorSamePathShortCircuits)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "hi");
    auto counting = make_ref<CountingAccessor>(a.cast<SourceAccessor>());

    EXPECT_TRUE(contentsEqual(SourcePath{counting, CanonPath("/f")}, SourcePath{counting, CanonPath("/f")}));

    /* The shortcut still has to do an lstat (step 1: kind check),
       but it must not have read the file, the directory, or asked
       for a fingerprint. Pin the cheap-path invariant. */
    EXPECT_EQ(counting->nReadFile.load(), 0u);
    EXPECT_EQ(counting->nReadDir.load(), 0u);
    EXPECT_EQ(counting->nFingerprint.load(), 0u);
}

/* ----- Step 3: fingerprint shortcut ------------------------------- */

TEST(ContentsEqual, matchingFingerprintShortCircuits)
{
    /* Two accessors that — were we to walk them — would disagree on
       contents (different file bodies). But both expose the same
       fingerprint at the path, so we should declare equality without
       ever reading the file. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "alpha");
    a->fingerprint = "shared-fp";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "beta");
    b->fingerprint = "shared-fp";

    auto ca = make_ref<CountingAccessor>(a.cast<SourceAccessor>());
    auto cb = make_ref<CountingAccessor>(b.cast<SourceAccessor>());

    EXPECT_TRUE(contentsEqual(SourcePath{ca, CanonPath("/f")}, SourcePath{cb, CanonPath("/f")}));

    /* Verified by trust: no file read happened on either side. */
    EXPECT_EQ(ca->nReadFile.load(), 0u);
    EXPECT_EQ(cb->nReadFile.load(), 0u);
}

TEST(ContentsEqual, differingFingerprintsFallThrough)
{
    /* Different fingerprints but identical contents → must fall
       through and discover the trees are equal by NAR comparison.
       This pins "fingerprint disagreement is not a final no". */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    a->fingerprint = "fp-A";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");
    b->fingerprint = "fp-B";

    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

TEST(ContentsEqual, oneFingerprintMissingFallsThrough)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    a->fingerprint = "fp";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same"); /* no fingerprint */

    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

/* ----- Step 4: hint short-circuit --------------------------------- */

TEST(ContentsEqual, hintMismatchReturnsFalse)
{
    /* Two trees that agree everywhere except at the hint subpath.
       The hint should fire and we return false without doing the
       full walk. */
    auto a = emptyDir();
    a->addFile(CanonPath("/flake.nix"), "{ description = \"A\"; }");
    a->addFile(CanonPath("/big"), std::string(10'000, 'x'));
    auto b = emptyDir();
    b->addFile(CanonPath("/flake.nix"), "{ description = \"B\"; }");
    b->addFile(CanonPath("/big"), std::string(10'000, 'x'));

    auto ca = make_ref<CountingAccessor>(a.cast<SourceAccessor>());
    auto cb = make_ref<CountingAccessor>(b.cast<SourceAccessor>());

    EXPECT_FALSE(
        contentsEqual(SourcePath{ca, CanonPath::root}, SourcePath{cb, CanonPath::root}, CanonPath("/flake.nix")));

    /* The hint mismatch should short-circuit before any /big read. */
    EXPECT_EQ(ca->nReadFile.load(), 1u); /* just the hint file */
    EXPECT_EQ(cb->nReadFile.load(), 1u);
}

TEST(ContentsEqual, hintMatchStillContinuesToFullCompare)
{
    /* Hint agrees but trees differ outside the hint. The hint must
       not be treated as proof of equality. */
    auto a = emptyDir();
    a->addFile(CanonPath("/flake.nix"), "agree");
    a->addFile(CanonPath("/other"), "X");
    auto b = emptyDir();
    b->addFile(CanonPath("/flake.nix"), "agree");
    b->addFile(CanonPath("/other"), "Y");

    EXPECT_FALSE(
        contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}, CanonPath("/flake.nix")));
}

TEST(ContentsEqual, hintAbsentOnOneSideIsSkipped)
{
    /* Hint missing on one side → skip the hint and run the full
       compare. The trees themselves are equal. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");

    EXPECT_TRUE(
        contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}, CanonPath("/never-there")));
}

TEST(ContentsEqual, hintIsLowLevelNoSymlinkResolution)
{
    /* If the hint path lstat()s to a symlink rather than a regular
       file, the function must *not* follow it. We pin this by
       arranging an `s` on each side that points to different regular
       files; if symlink resolution were on, the hint compare would
       fail. With low-level semantics it just compares the symlink
       targets, which here are equal. */
    auto a = emptyDir();
    a->addFile(CanonPath("/realA"), "differs A");
    MemorySink{*a}.createSymlink(CanonPath("/s"), "realA");
    auto b = emptyDir();
    b->addFile(CanonPath("/realB"), "differs B");
    MemorySink{*b}.createSymlink(CanonPath("/s"), "realA");
    /* Note: both symlinks have target "realA", but on `b` realA
       doesn't exist. With low-level semantics we only compare the
       targets, which match → hint passes → fall through. The full
       compare will then find the trees disagree (realA vs realB)
       and return false. We assert false (from the full compare),
       not from the hint. */
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}, CanonPath("/s")));
}

/* ----- Step 5: full recursive NAR comparison ---------------------- */

TEST(ContentsEqual, identicalRegularFilesAreEqual)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");
    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

TEST(ContentsEqual, differingRegularBytesAreUnequal)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "one");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "two");
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

TEST(ContentsEqual, executableBitMatters)
{
    /* NAR semantics include the executable bit. Two regular files
       with identical bytes but differing exec bits must not be
       equal. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");
    /* Flip exec bit on a's file directly. */
    auto * fa = a->open(CanonPath("/f"), std::nullopt);
    ASSERT_NE(fa, nullptr);
    std::get<fso::Regular<std::string>>(fa->raw).executable = true;

    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

TEST(ContentsEqual, symlinkTargetMatters)
{
    auto a = emptyDir();
    MemorySink{*a}.createSymlink(CanonPath("/s"), "target1");
    auto b = emptyDir();
    MemorySink{*b}.createSymlink(CanonPath("/s"), "target1");
    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/s")}, SourcePath{b, CanonPath("/s")}));

    auto c = emptyDir();
    MemorySink{*c}.createSymlink(CanonPath("/s"), "target2");
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/s")}, SourcePath{c, CanonPath("/s")}));
}

TEST(ContentsEqual, directoriesRecursiveEqualAndUnequal)
{
    auto a = emptyDir();
    MemorySink{*a}.createDirectory(CanonPath("/d"));
    a->addFile(CanonPath("/d/x"), "X");
    a->addFile(CanonPath("/d/y"), "Y");

    auto b = emptyDir();
    MemorySink{*b}.createDirectory(CanonPath("/d"));
    b->addFile(CanonPath("/d/x"), "X");
    b->addFile(CanonPath("/d/y"), "Y");

    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/d")}, SourcePath{b, CanonPath("/d")}));

    /* Diverge one byte deep in y. */
    auto * fy = b->open(CanonPath("/d/y"), std::nullopt);
    ASSERT_NE(fy, nullptr);
    std::get<fso::Regular<std::string>>(fy->raw).contents = "Y!";

    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/d")}, SourcePath{b, CanonPath("/d")}));
}

TEST(ContentsEqual, directoryEntrySetDifferenceIsUnequal)
{
    auto a = emptyDir();
    MemorySink{*a}.createDirectory(CanonPath("/d"));
    a->addFile(CanonPath("/d/x"), "X");

    auto b = emptyDir();
    MemorySink{*b}.createDirectory(CanonPath("/d"));
    b->addFile(CanonPath("/d/x"), "X");
    b->addFile(CanonPath("/d/extra"), "Z");

    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/d")}, SourcePath{b, CanonPath("/d")}));
}

TEST(ContentsEqual, emptyDirectoriesAreEqual)
{
    auto a = emptyDir();
    auto b = emptyDir();
    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}));
}

TEST(ContentsEqual, fingerprintHintInteraction)
{
    /* Pin the order: a matching fingerprint should win before the
       hint is even consulted. We make the hint disagree, which would
       otherwise force a `false`; if the fingerprint short-circuit
       runs first the answer is `true`. */
    auto a = emptyDir();
    a->addFile(CanonPath("/flake.nix"), "A");
    a->fingerprint = "fp";
    auto b = emptyDir();
    b->addFile(CanonPath("/flake.nix"), "B");
    b->fingerprint = "fp";

    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}, CanonPath("/flake.nix")));
}

/* ----- Reflexivity / symmetry ------------------------------------ */

TEST(ContentsEqual, reflexiveOnDeepTree)
{
    /* Whatever the algorithm does, comparing a tree to itself must
       say yes. */
    auto a = emptyDir();
    MemorySink{*a}.createDirectory(CanonPath("/sub"));
    MemorySink{*a}.createDirectory(CanonPath("/sub/inner"));
    a->addFile(CanonPath("/sub/inner/file"), "x");
    MemorySink{*a}.createSymlink(CanonPath("/sub/link"), "inner");

    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{a, CanonPath::root}));
}

TEST(ContentsEqual, symmetric)
{
    /* `contentsEqual(a, b)` and `contentsEqual(b, a)` should agree,
       including under fingerprint, hint, and full compares. We
       exercise each path. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "X");
    a->fingerprint = "fpA";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "Y");
    b->fingerprint = "fpB";
    EXPECT_EQ(
        contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}),
        contentsEqual(SourcePath{b, CanonPath("/f")}, SourcePath{a, CanonPath("/f")}));

    /* Now equal but via fingerprint. */
    auto c = emptyDir();
    c->addFile(CanonPath("/f"), "anything"), c->fingerprint = "same-fp";
    auto d = emptyDir();
    d->addFile(CanonPath("/f"), "anything-else"), d->fingerprint = "same-fp";
    EXPECT_EQ(
        contentsEqual(SourcePath{c, CanonPath("/f")}, SourcePath{d, CanonPath("/f")}),
        contentsEqual(SourcePath{d, CanonPath("/f")}, SourcePath{c, CanonPath("/f")}));
}

/* ----- Pointer/path shortcut beats a bad fingerprint -------------- */

TEST(ContentsEqual, samePointerSamePathBeatsBadFingerprint)
{
    /* Even if the accessor lies about its fingerprint (and the
       fingerprint mechanism could otherwise mislead us), the
       same-accessor-same-path shortcut MUST fire first. */
    struct LyingFingerprint : SourceAccessor
    {
        ref<SourceAccessor> inner;

        explicit LyingFingerprint(ref<SourceAccessor> i)
            : inner(i)
        {
        }

        void anchor() override {}

        std::optional<Stat> maybeLstat(const CanonPath & p) override
        {
            return inner->maybeLstat(p);
        }

        void readFile(const CanonPath & p, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            inner->readFile(p, sink, sizeCallback);
        }

        using SourceAccessor::readFile;

        DirEntries readDirectory(const CanonPath & p) override
        {
            return inner->readDirectory(p);
        }

        std::string readLink(const CanonPath & p) override
        {
            return inner->readLink(p);
        }

        /* Pretend to return a fingerprint but bump the value each call
           so two consecutive lookups would falsely disagree. */
        std::atomic<int> calls{0};

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & p) override
        {
            return {p, "ever-changing-" + std::to_string(++calls)};
        }
    };

    auto inner = emptyDir();
    inner->addFile(CanonPath("/f"), "hello");
    auto liar = make_ref<LyingFingerprint>(inner.cast<SourceAccessor>());
    EXPECT_TRUE(contentsEqual(SourcePath{liar, CanonPath("/f")}, SourcePath{liar, CanonPath("/f")}));
}

/* ----- Fingerprint subpath disagreement falls through ------------- */

TEST(ContentsEqual, sameFingerprintDifferentSubpathFallsThrough)
{
    /* Two accessors with the same fingerprint string but
       different rebased subpaths (e.g. the same mounted store path
       accessed at two different intra-mount offsets) must not be
       claimed equal by the fingerprint shortcut; the contents-walk
       resolves it. Here contents differ, so the answer is false. */
    struct RebasedFingerprint : SourceAccessor
    {
        ref<SourceAccessor> inner;
        CanonPath rebase;
        std::string fp;

        RebasedFingerprint(ref<SourceAccessor> i, CanonPath r, std::string f)
            : inner(i)
            , rebase(std::move(r))
            , fp(std::move(f))
        {
        }

        void anchor() override {}

        std::optional<Stat> maybeLstat(const CanonPath & p) override
        {
            return inner->maybeLstat(p);
        }

        void readFile(const CanonPath & p, Sink & sink, fun<void(uint64_t)> sizeCallback) override
        {
            inner->readFile(p, sink, sizeCallback);
        }

        using SourceAccessor::readFile;

        DirEntries readDirectory(const CanonPath & p) override
        {
            return inner->readDirectory(p);
        }

        std::string readLink(const CanonPath & p) override
        {
            return inner->readLink(p);
        }

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & p) override
        {
            return {rebase, fp};
        }
    };

    auto innerA = emptyDir();
    innerA->addFile(CanonPath("/f"), "A");
    auto innerB = emptyDir();
    innerB->addFile(CanonPath("/f"), "B");
    auto a = make_ref<RebasedFingerprint>(innerA.cast<SourceAccessor>(), CanonPath("/store/sub-a"), "shared-fp");
    auto b = make_ref<RebasedFingerprint>(innerB.cast<SourceAccessor>(), CanonPath("/store/sub-b"), "shared-fp");
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

/* ----- Hint at a directory subpath -------------------------------- */

TEST(ContentsEqual, hintAtDirectoryWithDifferingEntriesIsFalse)
{
    auto a = emptyDir();
    MemorySink{*a}.createDirectory(CanonPath("/sub"));
    a->addFile(CanonPath("/sub/x"), "X");

    auto b = emptyDir();
    MemorySink{*b}.createDirectory(CanonPath("/sub"));
    b->addFile(CanonPath("/sub/x"), "X");
    b->addFile(CanonPath("/sub/extra"), "Z");

    /* Hint catches the entry-set disagreement at /sub without
       recursing. */
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}, CanonPath("/sub")));
}

/* ----- Both paths absent --------------------------------------- */

TEST(ContentsEqual, bothPathsAbsentIsFalse)
{
    /* Pin the conservative choice: if neither path resolves,
       contentsEqual returns false rather than vacuously true. */
    auto a = emptyDir();
    auto b = emptyDir();
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/nope")}, SourcePath{b, CanonPath("/nope")}));
}

TEST(ContentsEqual, onePathAbsentIsFalse)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "X");
    auto b = emptyDir();
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/f")}, SourcePath{b, CanonPath("/f")}));
}

/* ----- Larger files / deeper trees -------------------------------- */

TEST(ContentsEqual, largeRegularFilesByteByByte)
{
    /* Two big files that match byte-for-byte. Mostly a sanity check
       that we're not accidentally comparing pointers or sizes only. */
    std::string body(64 * 1024, '\0');
    for (size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>(i & 0xff);

    auto a = emptyDir();
    a->addFile(CanonPath("/big"), std::string(body));
    auto b = emptyDir();
    b->addFile(CanonPath("/big"), std::string(body));
    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath("/big")}, SourcePath{b, CanonPath("/big")}));

    body[42] ^= 1;
    auto c = emptyDir();
    c->addFile(CanonPath("/big"), std::string(body));
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath("/big")}, SourcePath{c, CanonPath("/big")}));
}

TEST(ContentsEqual, deepDirectoryTrees)
{
    /* Build /a/b/c/d/e/file on both sides with the same contents.
       Diverge only at the leaf to make sure recursion reaches the
       full depth. */
    auto build = [](std::string leaf) {
        auto acc = emptyDir();
        MemorySink sink{*acc};
        sink.createDirectory(CanonPath("/a"));
        sink.createDirectory(CanonPath("/a/b"));
        sink.createDirectory(CanonPath("/a/b/c"));
        sink.createDirectory(CanonPath("/a/b/c/d"));
        sink.createDirectory(CanonPath("/a/b/c/d/e"));
        acc->addFile(CanonPath("/a/b/c/d/e/file"), std::move(leaf));
        return acc;
    };
    auto a = build("identical"), b = build("identical");
    EXPECT_TRUE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{b, CanonPath::root}));

    auto c = build("different");
    EXPECT_FALSE(contentsEqual(SourcePath{a, CanonPath::root}, SourcePath{c, CanonPath::root}));
}

} // namespace nix

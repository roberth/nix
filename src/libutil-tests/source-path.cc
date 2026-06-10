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
 *   bool contentsEqual(ref<SourceAccessor>, ref<SourceAccessor>,
 *                      std::optional<CanonPath> hint = std::nullopt);
 *
 * Compares two accessors' trees from their roots under NAR
 * semantics: file type, regular-file contents + executable bit,
 * symlink target, directory entries recursively. The hint is
 * purely a cheap discriminator — disagreement at the hint
 * fast-rejects; agreement does not imply equality, the function
 * still does the full walk.
 * =================================================================
 */

/* ----- Same accessor pointer is the trivial yes ------------------ */

TEST(ContentsEqual, samePointerShortCircuits)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "hi");
    auto counting = make_ref<CountingAccessor>(a.cast<SourceAccessor>());

    EXPECT_TRUE(contentsEqual(counting, counting));

    /* The shortcut must not have read anything or asked for a
       fingerprint. */
    EXPECT_EQ(counting->nLstat.load(), 0u);
    EXPECT_EQ(counting->nReadFile.load(), 0u);
    EXPECT_EQ(counting->nReadDir.load(), 0u);
    EXPECT_EQ(counting->nFingerprint.load(), 0u);
}

/* ----- Fingerprint shortcut at the root -------------------------- */

TEST(ContentsEqual, matchingFingerprintShortCircuits)
{
    /* Two accessors whose trees disagree if walked. Both expose
       the same fingerprint at the root, so the shortcut should
       declare equality without any I/O. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "alpha");
    a->fingerprint = "shared-fp";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "beta");
    b->fingerprint = "shared-fp";

    auto ca = make_ref<CountingAccessor>(a.cast<SourceAccessor>());
    auto cb = make_ref<CountingAccessor>(b.cast<SourceAccessor>());

    EXPECT_TRUE(contentsEqual(ca, cb));

    EXPECT_EQ(ca->nReadFile.load(), 0u);
    EXPECT_EQ(cb->nReadFile.load(), 0u);
    EXPECT_EQ(ca->nReadDir.load(), 0u);
    EXPECT_EQ(cb->nReadDir.load(), 0u);
}

TEST(ContentsEqual, differingFingerprintsFallThrough)
{
    /* Different fingerprints but identical trees → fall through
       and discover equality by the contents walk. Pins
       "fingerprint disagreement is not a final no". */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    a->fingerprint = "fp-A";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");
    b->fingerprint = "fp-B";

    EXPECT_TRUE(contentsEqual(a, b));
}

TEST(ContentsEqual, oneFingerprintMissingFallsThrough)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    a->fingerprint = "fp";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same"); /* no fingerprint */

    EXPECT_TRUE(contentsEqual(a, b));
}

/* ----- Hint discriminator ---------------------------------------- */

TEST(ContentsEqual, hintMismatchReturnsFalse)
{
    /* Two trees that agree everywhere except at the hint subpath.
       The hint should fire and we return false without walking
       the rest. */
    auto a = emptyDir();
    a->addFile(CanonPath("/flake.nix"), "{ description = \"A\"; }");
    a->addFile(CanonPath("/big"), std::string(10'000, 'x'));
    auto b = emptyDir();
    b->addFile(CanonPath("/flake.nix"), "{ description = \"B\"; }");
    b->addFile(CanonPath("/big"), std::string(10'000, 'x'));

    auto ca = make_ref<CountingAccessor>(a.cast<SourceAccessor>());
    auto cb = make_ref<CountingAccessor>(b.cast<SourceAccessor>());

    EXPECT_FALSE(contentsEqual(ca, cb, CanonPath("/flake.nix")));

    /* The hint mismatch should short-circuit before any /big
       read. */
    EXPECT_EQ(ca->nReadFile.load(), 1u); /* just the hint file */
    EXPECT_EQ(cb->nReadFile.load(), 1u);
}

TEST(ContentsEqual, hintMatchStillContinuesToFullCompare)
{
    /* Hint agrees but trees differ outside the hint. The hint
       must not be treated as proof of equality — the full walk
       still runs and finds the difference. */
    auto a = emptyDir();
    a->addFile(CanonPath("/flake.nix"), "agree");
    a->addFile(CanonPath("/other"), "X");
    auto b = emptyDir();
    b->addFile(CanonPath("/flake.nix"), "agree");
    b->addFile(CanonPath("/other"), "Y");

    EXPECT_FALSE(contentsEqual(a, b, CanonPath("/flake.nix")));
}

TEST(ContentsEqual, hintAbsentOnOneSideIsSkipped)
{
    /* Hint missing on one side → skip the hint and run the full
       compare. The trees themselves are equal. */
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");

    EXPECT_TRUE(contentsEqual(a, b, CanonPath("/never-there")));
}

TEST(ContentsEqual, hintIsLowLevelNoSymlinkResolution)
{
    /* If the hint path lstat()s to a symlink rather than a regular
       file, the function must *not* follow it. We pin this by
       arranging an `s` on each side that points to different
       regular files; if symlink resolution were on, the hint
       compare would fail. With low-level semantics it compares
       the symlink targets, which here are equal, so the hint
       passes and we fall through. The full compare then finds
       the trees disagree (realA vs realB) and returns false. */
    auto a = emptyDir();
    a->addFile(CanonPath("/realA"), "differs A");
    MemorySink{*a}.createSymlink(CanonPath("/s"), "realA");
    auto b = emptyDir();
    b->addFile(CanonPath("/realB"), "differs B");
    MemorySink{*b}.createSymlink(CanonPath("/s"), "realA");
    EXPECT_FALSE(contentsEqual(a, b, CanonPath("/s")));
}

/* ----- Full recursive NAR comparison ----------------------------- */

TEST(ContentsEqual, identicalTreesAreEqual)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "same");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "same");
    EXPECT_TRUE(contentsEqual(a, b));
}

TEST(ContentsEqual, differingRegularBytesAreUnequal)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "one");
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "two");
    EXPECT_FALSE(contentsEqual(a, b));
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
    auto * fa = a->open(CanonPath("/f"), std::nullopt);
    ASSERT_NE(fa, nullptr);
    std::get<fso::Regular<std::string>>(fa->raw).executable = true;

    EXPECT_FALSE(contentsEqual(a, b));
}

TEST(ContentsEqual, symlinkTargetMatters)
{
    /* Root of each accessor is just a symlink (no surrounding
       directory). Test the top-level type recognition. */
    auto a = make_ref<MemorySourceAccessor>();
    MemorySink{*a}.createSymlink(CanonPath::root, "target1");
    auto b = make_ref<MemorySourceAccessor>();
    MemorySink{*b}.createSymlink(CanonPath::root, "target1");
    EXPECT_TRUE(contentsEqual(a, b));

    auto c = make_ref<MemorySourceAccessor>();
    MemorySink{*c}.createSymlink(CanonPath::root, "target2");
    EXPECT_FALSE(contentsEqual(a, c));
}

TEST(ContentsEqual, fileTypeMismatchInTreeIsFalse)
{
    /* /x is a regular file in a's tree and a directory in b's
       tree. The recursive walk catches the type mismatch. */
    auto a = emptyDir();
    a->addFile(CanonPath("/x"), "hi");

    auto b = emptyDir();
    MemorySink{*b}.createDirectory(CanonPath("/x"));

    EXPECT_FALSE(contentsEqual(a, b));
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

    EXPECT_FALSE(contentsEqual(a, b));
}

TEST(ContentsEqual, emptyDirectoriesAreEqual)
{
    auto a = emptyDir();
    auto b = emptyDir();
    EXPECT_TRUE(contentsEqual(a, b));
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
    EXPECT_TRUE(contentsEqual(a, b));

    auto c = build("different");
    EXPECT_FALSE(contentsEqual(a, c));
}

TEST(ContentsEqual, largeRegularFilesByteByByte)
{
    /* Two big files that match byte-for-byte. Mostly a sanity
       check that we're not accidentally comparing sizes only. */
    std::string body(64 * 1024, '\0');
    for (size_t i = 0; i < body.size(); ++i)
        body[i] = static_cast<char>(i & 0xff);

    auto a = emptyDir();
    a->addFile(CanonPath("/big"), std::string(body));
    auto b = emptyDir();
    b->addFile(CanonPath("/big"), std::string(body));
    EXPECT_TRUE(contentsEqual(a, b));

    body[42] ^= 1;
    auto c = emptyDir();
    c->addFile(CanonPath("/big"), std::string(body));
    EXPECT_FALSE(contentsEqual(a, c));
}

/* ----- Reflexivity / symmetry ------------------------------------ */

TEST(ContentsEqual, reflexiveOnDeepTree)
{
    auto a = emptyDir();
    MemorySink{*a}.createDirectory(CanonPath("/sub"));
    MemorySink{*a}.createDirectory(CanonPath("/sub/inner"));
    a->addFile(CanonPath("/sub/inner/file"), "x");
    MemorySink{*a}.createSymlink(CanonPath("/sub/link"), "inner");

    EXPECT_TRUE(contentsEqual(a, a));
}

TEST(ContentsEqual, symmetric)
{
    auto a = emptyDir();
    a->addFile(CanonPath("/f"), "X");
    a->fingerprint = "fpA";
    auto b = emptyDir();
    b->addFile(CanonPath("/f"), "Y");
    b->fingerprint = "fpB";
    EXPECT_EQ(contentsEqual(a, b), contentsEqual(b, a));

    /* And via fingerprint. */
    auto c = emptyDir();
    c->addFile(CanonPath("/f"), "anything"), c->fingerprint = "same-fp";
    auto d = emptyDir();
    d->addFile(CanonPath("/f"), "anything-else"), d->fingerprint = "same-fp";
    EXPECT_EQ(contentsEqual(c, d), contentsEqual(d, c));
}

/* ----- Fingerprint with rebased subpath disagreement falls
 * through --------------------------------------------------------- */

TEST(ContentsEqual, sameFingerprintDifferentRebasedRootFallsThrough)
{
    /* Two accessors expose the same fingerprint string but at
       different rebased internal paths — e.g. two different
       mounts inside the same fingerprinted tree. The shortcut
       must not fire; the full walk decides. */
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

        std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath &) override
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
    EXPECT_FALSE(contentsEqual(a, b));
}

} // namespace nix

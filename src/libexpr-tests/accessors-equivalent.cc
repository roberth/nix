#include <filesystem>

#include <gtest/gtest.h>

#include "nix/expr/source-root.hh"
#include "nix/expr/tests/libexpr.hh"
#include "nix/util/memory-source-accessor.hh"

namespace nix {

/* `EvalState::accessorsEquivalent(a, b, hint?)` answers the
   question "would these two Copyable accessors produce the same
   storePath, i.e. are their trees NAR-equivalent?" — correctly,
   without falling back to per-pair tree walks. This minimal
   commit covers three layers: pointer identity, fingerprint
   match, and (last resort) lookup-or-compute the storePath via
   `srcToStore`. Subsequent commits add the inequality cache and
   the cheap probes (root dir name set, hint subpath SHA256). */

class AccessorsEquivalentTest : public LibExprTest
{
protected:
    void SetUp() override
    {
        /* The layer-3 storePath compute (`accessorsEquivalent`'s
           fall-through to `srcToStore`) routes through
           `fetchToStore2`, which opens a sqlite cache under
           `$XDG_CACHE_HOME` (or `$HOME/.cache`). The nix sandbox's
           `HOME=/homeless-shelter` is unwritable, so point the cache
           somewhere writable. Sibling test files (`lock-input.cc`)
           do the same. */
        auto dir = std::filesystem::temp_directory_path() / "nix-test-cache";
        std::filesystem::create_directories(dir);
        ::setenv("XDG_CACHE_HOME", dir.c_str(), 1);
    }

    static ref<MemorySourceAccessor> mkAcc(std::initializer_list<std::pair<std::string, std::string>> files = {})
    {
        auto acc = make_ref<MemorySourceAccessor>();
        MemorySink{*acc}.createDirectory(CanonPath::root);
        for (auto & [path, contents] : files)
            acc->addFile(CanonPath(path), std::string(contents));
        return acc;
    }
};

/* --- Layer 1: pointer identity --------------------------------- */

TEST_F(AccessorsEquivalentTest, samePointerIsEquivalent)
{
    /* The cheapest decisive layer: same accessor ref, trivially
       equivalent. No tree work happens — pin that by giving the
       accessor a unique content that wouldn't accidentally match
       anything else. */
    auto acc = mkAcc({{"/f", "unique-bytes"}});
    EXPECT_TRUE(state.accessorsEquivalent(acc, acc));
}

/* --- Layer 2: fingerprint match (positive proof only) ---------- */

TEST_F(AccessorsEquivalentTest, sameFingerprintIsEquivalent)
{
    /* Fingerprint match is a one-way positive proof per the
       fingerprint contract: same fingerprint string ⇒ NAR-equal
       trees. Make the on-disk content differ so a tree walk
       would say "no" — the test passes iff the fingerprint
       shortcut runs. */
    auto accA = mkAcc({{"/f", "would-differ-on-walk-A"}});
    accA->fingerprint = "shared-fp";
    auto accB = mkAcc({{"/f", "would-differ-on-walk-B"}});
    accB->fingerprint = "shared-fp";
    EXPECT_TRUE(state.accessorsEquivalent(accA, accB));
}

TEST_F(AccessorsEquivalentTest, fingerprintMismatchIsNotConclusive)
{
    /* Different fingerprints do NOT prove inequality — the same
       NAR can come from different fingerprints (different URLs,
       different cache rebases of the same source). The function
       must fall through to the storePath compute and discover the
       trees are NAR-equal. */
    auto accA = mkAcc({{"/f", "shared"}});
    accA->fingerprint = "fp-A";
    auto accB = mkAcc({{"/f", "shared"}});
    accB->fingerprint = "fp-B";
    EXPECT_TRUE(state.accessorsEquivalent(accA, accB));
}

/* --- Layer 3: storePath lookup-or-compute --------------------- */

TEST_F(AccessorsEquivalentTest, differentAccessorsSameContentsAreEquivalent)
{
    /* No pointer match, no fingerprints. The storePath of each
       is computed and cached in srcToStore; the two storePaths
       come out equal because the NARs are equal. */
    auto accA = mkAcc({{"/f", "shared"}});
    auto accB = mkAcc({{"/f", "shared"}});
    EXPECT_TRUE(state.accessorsEquivalent(accA, accB));
}

TEST_F(AccessorsEquivalentTest, differentAccessorsDifferentContentsAreInequivalent)
{
    /* Different NARs ⇒ different storePaths ⇒ inequivalent.
       The decision is made by the storePath compare, not by
       any cheap layer. */
    auto accA = mkAcc({{"/f", "A"}});
    auto accB = mkAcc({{"/f", "B"}});
    EXPECT_FALSE(state.accessorsEquivalent(accA, accB));
}

} // namespace nix

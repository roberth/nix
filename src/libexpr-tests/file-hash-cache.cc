#include <gtest/gtest.h>

#include "nix/expr/file-hash-cache.hh"
#include "nix/util/file-system.hh"

#include <fstream>

namespace nix {

class FileHashCacheTest : public ::testing::Test
{
protected:
    std::filesystem::path tempDir;
    std::filesystem::path testFile;
    std::filesystem::path dbPath;

    void SetUp() override
    {
        tempDir = createTempDir();
        testFile = tempDir / "test.txt";
        dbPath = tempDir / "file-hash-cache.sqlite";
    }

    void TearDown() override
    {
        std::filesystem::remove_all(tempDir);
    }

    void writeFile(const std::string & content)
    {
        std::ofstream f(testFile);
        f << content;
    }
};

TEST_F(FileHashCacheTest, ComputesHashOnMiss)
{
    writeFile("hello world");

    FileHashCache cache{dbPath};
    auto hash = cache.getHash(testFile);

    // SHA-256 of "hello world"
    EXPECT_EQ(
        hash.to_string(HashFormat::Base16, false), "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");
}

TEST_F(FileHashCacheTest, ReturnsFromCacheOnHit)
{
    writeFile("cached content");

    FileHashCache cache{dbPath};
    auto hash1 = cache.getHash(testFile);
    auto hash2 = cache.getHash(testFile);
    EXPECT_EQ(hash1, hash2);
}

TEST_F(FileHashCacheTest, LookupReturnsNulloptOnMiss)
{
    writeFile("some content");

    FileHashCache cache{dbPath};
    auto result = cache.lookup(testFile);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FileHashCacheTest, LookupReturnsHashAfterGet)
{
    writeFile("lookup test");
    /* Sleep past the second boundary so the rounding guard in
       getHash permits caching. Without this, getHash refuses to
       record an entry whose mtime equals the current second (see
       RefusesToCacheSameSecondWrite for the safety rationale). */
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    FileHashCache cache{dbPath};
    auto hash = cache.getHash(testFile);

    auto result = cache.lookup(testFile);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, hash);
}

TEST_F(FileHashCacheTest, InvalidateClearsCache)
{
    writeFile("invalidate test");

    FileHashCache cache{dbPath};
    cache.getHash(testFile);
    cache.invalidate(testFile);

    auto result = cache.lookup(testFile);
    EXPECT_FALSE(result.has_value());
}

TEST_F(FileHashCacheTest, DetectsMtimeChange)
{
    writeFile("original");

    FileHashCache cache{dbPath};
    auto hash1 = cache.getHash(testFile);

    // Ensure mtime changes
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    writeFile("modified");

    auto hash2 = cache.getHash(testFile);
    EXPECT_NE(hash1, hash2);
}

TEST_F(FileHashCacheTest, RefusesToCacheSameSecondWrite)
{
    /* A write that lands in the current wall-clock second leaves
       getHash unable to prove the contents won't change before the
       second rolls over (POSIX mtime has 1-second granularity, so
       another write this same second can mutate the bytes without
       advancing mtime). getHash must still return the correct hash,
       but must NOT cache the entry — otherwise a subsequent
       same-second mutation would be served stale. */
    writeFile("fresh content");

    FileHashCache cache{dbPath};
    auto hash = cache.getHash(testFile);

    /* No cache entry yet — getHash refused because the write was
       too recent. */
    auto immediate = cache.lookup(testFile);
    EXPECT_FALSE(immediate.has_value()) << "must not cache an entry whose mtime equals the current second";

    /* Wait past the second boundary so the file's mtime is strictly
       in the past, then re-request. This call should cache. */
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    cache.getHash(testFile);
    auto ripened = cache.lookup(testFile);
    ASSERT_TRUE(ripened.has_value()) << "must cache once the file's mtime is strictly older than now";
    EXPECT_EQ(*ripened, hash);
}

TEST_F(FileHashCacheTest, ServesNoStaleHashAfterSameSecondMutation)
{
    /* The end-to-end safety property: write A, hash it, overwrite
       with B in the same second (mtime unchanged), then re-query.
       With the rounding guard, we must NOT receive H_A on the
       second call. Without the guard we did (which is why this
       test exists). */
    writeFile("A");

    FileHashCache cache{dbPath};
    auto h1 = cache.getHash(testFile);

    /* Same-second overwrite. The mtime stays at the current second
       (still ≤ now). */
    writeFile("BB");
    auto h2 = cache.getHash(testFile);

    EXPECT_NE(h1, h2) << "must compute B's hash, not serve the cached A";
}

TEST_F(FileHashCacheTest, ConstructorPerformsNoIO)
{
    // SystemEnvironment constructs a FileHashCache for every EvalState,
    // even when no caller will ever query it. Nix is sometimes invoked
    // with HOME set to a directory that must not be auto-created (e.g.
    // /homeless-shelter inside builds without sandboxing, or /fake-home
    // in lang.sh purity tests). Materialising the cache dir there breaks
    // downstream purity checks.
    //
    // Verify that the default-path constructor (the same path taken by
    // SystemEnvironment) performs no filesystem I/O: the cache dir must
    // not exist after construction, even though `HOME` points at a
    // non-existent location.
    auto fakeHome = tempDir / "non-existent-home";
    setenv("HOME", fakeHome.string().c_str(), 1);
    // Make sure XDG fallback doesn't shadow HOME.
    unsetenv("XDG_CACHE_HOME");
    unsetenv("NIX_CACHE_HOME");

    {
        FileHashCache cache;
        EXPECT_FALSE(std::filesystem::exists(fakeHome))
            << "FileHashCache constructor must not create HOME ('" << fakeHome.string() << "')";
    }
    // Sanity: still doesn't exist after destruction either.
    EXPECT_FALSE(std::filesystem::exists(fakeHome));
}

} // namespace nix

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

} // namespace nix

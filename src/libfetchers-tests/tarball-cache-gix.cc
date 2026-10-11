#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/tarball-cache-gix.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"

#include <gtest/gtest.h>

namespace nix::fetchers::gix {

/**
 * A bare repository, like the tarball cache, in a temporary directory.
 */
class TarballCacheGixTest : public ::testing::Test
{
    AutoDelete delTmpDir;

protected:
    std::filesystem::path repoDir;

public:
    void SetUp() override
    {
        repoDir = createTempDir() / "tarball-cache";
        GitRepo::openRepo(repoDir, {.create = true, .bare = true});
        delTmpDir = AutoDelete(repoDir.parent_path(), true);
    }

    /**
     * The tarball cache options of `Settings::getTarballWriterPool()`.
     */
    ref<GitRepoPool> openLibgit2Writer()
    {
        return GitRepoPool::create(repoDir, {.create = false, .bare = true, .packfilesOnly = true});
    }

    ref<GitRepo> openLibgit2Reader()
    {
        return GitRepo::openRepo(repoDir, {.create = false, .bare = true, .packfilesOnly = true});
    }

    /**
     * The names of the files in `objects/pack`, sorted.
     */
    std::vector<std::string> packFiles() const
    {
        std::vector<std::string> res;
        for (auto & entry : DirectoryIterator(repoDir / "objects" / "pack"))
            res.push_back(entry.path().filename().string());
        std::sort(res.begin(), res.end());
        return res;
    }

    /**
     * The number of objects in each pack, by the fan-out table of its
     * index, in the order of `packFiles()`.
     */
    std::vector<uint32_t> packObjectCounts() const
    {
        std::vector<uint32_t> res;
        for (auto & name : packFiles()) {
            if (!name.ends_with(".idx"))
                continue;
            auto index = readFile(repoDir / "objects" / "pack" / name);
            /* Magic, version, then 256 cumulative counts. */
            auto * last = reinterpret_cast<const uint8_t *>(index.data()) + 8 + 255 * 4;
            res.push_back(uint32_t(last[0]) << 24 | uint32_t(last[1]) << 16 | uint32_t(last[2]) << 8 | last[3]);
        }
        return res;
    }
};

namespace {

merkle::TreeEntry writeFile(merkle::FileSinkBuilder & store, std::string_view contents, bool executable = false)
{
    auto sink = store.makeRegularFileSink();
    (*sink)(contents);
    return {executable ? merkle::Mode::Executable : merkle::Mode::Regular, std::move(*sink).finalize()};
}

merkle::TreeEntry
writeDirectory(merkle::FileSinkBuilder & store, std::vector<std::pair<std::string, merkle::TreeEntry>> children)
{
    auto sink = store.makeDirectorySink();
    for (auto & [name, entry] : children)
        sink->insertChild(name, entry);
    return {merkle::Mode::Directory, std::move(*sink).finalize()};
}

/**
 * Write the tree that the tests use: a regular file, an executable, a
 * symlink, an empty directory and a nested file. The children are inserted
 * out of Git order on purpose.
 */
merkle::TreeEntry writeTestTree(merkle::FileSinkBuilder & store)
{
    auto nested = writeDirectory(store, {{"deep.txt", writeFile(store, "deep\n")}});
    auto empty = writeDirectory(store, {});
    return writeDirectory(
        store,
        {
            {"run.sh", writeFile(store, "#!/bin/sh\necho hi\n", true)},
            {"sub", nested},
            {"link", store.makeSymlink("hello.txt")},
            {"empty", empty},
            {"hello.txt", writeFile(store, "hello\n")},
            {"sub.txt", writeFile(store, "a file that sorts between 'sub' and 'sub/'\n")},
        });
}

/**
 * Every path below `path`, one line each, in the order of a depth-first
 * walk over sorted directory entries.
 */
void dump(SourceAccessor & accessor, const CanonPath & path, std::vector<std::string> & out)
{
    auto st = accessor.lstat(path);
    if (st.type == SourceAccessor::tDirectory) {
        out.push_back("dir " + path.abs());
        for (auto & [name, type] : accessor.readDirectory(path))
            dump(accessor, path / name, out);
    } else if (st.type == SourceAccessor::tRegular)
        out.push_back((st.isExecutable ? "exe " : "file ") + path.abs() + ": " + accessor.readFile(path));
    else if (st.type == SourceAccessor::tSymlink)
        out.push_back("link " + path.abs() + " -> " + accessor.readLink(path));
    else
        out.push_back("other " + path.abs());
}

std::vector<std::string> dump(SourceAccessor & accessor)
{
    std::vector<std::string> out;
    dump(accessor, CanonPath::root, out);
    return out;
}

const std::vector<std::string> expectedDump = {
    "dir /",
    "dir /empty",
    "file /hello.txt: hello\n",
    "link /link -> hello.txt",
    "exe /run.sh: #!/bin/sh\necho hi\n",
    "dir /sub",
    "file /sub/deep.txt: deep\n",
    "file /sub.txt: a file that sorts between 'sub' and 'sub/'\n",
};

/* The test `same_tree_as_libgit2` shows that libgit2 computes the same id. */
const std::string expectedTreeId = "6d41dae4afd9a3a465db433f6a805a00dedef512";

} // namespace

TEST_F(TarballCacheGixTest, round_trip)
{
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);
    writer->flush();

    EXPECT_EQ(tree.hash.gitRev(), expectedTreeId);
    EXPECT_EQ(dump(*openTarballCache(repoDir)->getAccessor(tree.hash, {}, "")), expectedDump);
}

TEST_F(TarballCacheGixTest, same_tree_as_libgit2)
{
    auto libgit2Tree = writeTestTree(*openLibgit2Writer());
    auto gixWriter = openTarballWriter(repoDir);
    auto gixTree = writeTestTree(*gixWriter);
    EXPECT_EQ(gixTree, libgit2Tree);
}

TEST_F(TarballCacheGixTest, libgit2_reads_gix_pack)
{
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);
    writer->flush();

    EXPECT_EQ(dump(*openLibgit2Reader()->getAccessor(tree.hash, {}, "")), expectedDump);
}

TEST_F(TarballCacheGixTest, gix_reads_libgit2_pack)
{
    auto writer = openLibgit2Writer();
    auto tree = writeTestTree(*writer);
    writer->flush();

    auto cache = openTarballCache(repoDir);
    EXPECT_TRUE(cache->hasObject(tree.hash));
    EXPECT_EQ(dump(*cache->getAccessor(tree.hash, {}, "")), expectedDump);
}

TEST_F(TarballCacheGixTest, one_pack_per_flush)
{
    auto writer = openTarballWriter(repoDir);
    writeTestTree(*writer);
    writer->flush();

    /* 4 blobs with contents, 1 symlink blob and 3 trees. */
    EXPECT_EQ(packObjectCounts(), std::vector<uint32_t>{8});

    /* Flushing again without new objects writes nothing. */
    writer->flush();
    EXPECT_EQ(packObjectCounts(), std::vector<uint32_t>{8});
}

TEST_F(TarballCacheGixTest, skips_objects_on_disk)
{
    {
        auto writer = openTarballWriter(repoDir);
        writeTestTree(*writer);
        writer->flush();
    }
    auto firstPack = packFiles();

    /* A new writer finds the objects of the first pack on disk, so the
       second pack contains only the new file and the new root tree. */
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);
    writeDirectory(*writer, {{"tree", tree}, {"new.txt", writeFile(*writer, "new\n")}});
    writer->flush();

    auto counts = packObjectCounts();
    std::sort(counts.begin(), counts.end());
    EXPECT_EQ(counts, (std::vector<uint32_t>{2, 8}));
    EXPECT_EQ(packFiles().size(), firstPack.size() + 2);
}

/**
 * The object store of a writer sizes its table of packs when the writer
 * opens. Packs that concurrent writers add later must not keep it from
 * finding the packs that were there: it used to read the pack directory on
 * its first lookup, failed when the table was too small, and then wrote
 * every object again.
 */
TEST_F(TarballCacheGixTest, skips_objects_on_disk_when_packs_appear_after_open)
{
    {
        auto writer = openTarballWriter(repoDir);
        writeFile(*writer, "old\n");
        writer->flush();
    }

    auto writer = openTarballWriter(repoDir);

    /* More packs than the 32 slots that a store with one pack has. */
    for (int i = 0; i < 40; ++i) {
        auto other = openTarballWriter(repoDir);
        writeFile(*other, fmt("other %d\n", i));
        other->flush();
    }

    writeFile(*writer, "old\n");
    writeFile(*writer, "new\n");
    writer->flush();

    /* Each pack has one object: the last one only "new". */
    EXPECT_EQ(packObjectCounts(), std::vector<uint32_t>(42, 1));
}

TEST_F(TarballCacheGixTest, unflushed_objects_are_not_on_disk)
{
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);

    EXPECT_FALSE(openTarballCache(repoDir)->hasObject(tree.hash));
    EXPECT_EQ(packFiles(), std::vector<std::string>{});

    writer->flush();
    EXPECT_TRUE(openTarballCache(repoDir)->hasObject(tree.hash));
}

TEST_F(TarballCacheGixTest, interrupted_flush_leaves_no_pack)
{
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);

    setInterrupted(true);
    Finally resetInterrupt([] { setInterrupted(false); });
    EXPECT_THROW(writer->flush(), Interrupted);
    setInterrupted(false);

    /* No pack, no index and no temporary file. */
    EXPECT_EQ(packFiles(), std::vector<std::string>{});
    EXPECT_FALSE(openTarballCache(repoDir)->hasObject(tree.hash));

    /* The writer keeps the objects, so a later flush writes them. */
    writer->flush();
    EXPECT_EQ(packObjectCounts(), std::vector<uint32_t>{8});
    EXPECT_EQ(dump(*openTarballCache(repoDir)->getAccessor(tree.hash, {}, "")), expectedDump);
}

TEST_F(TarballCacheGixTest, rejects_dot_git)
{
    auto writer = openTarballWriter(repoDir);
    auto sink = writer->makeDirectorySink();
    sink->insertChild(".GIT", writeFile(*writer, "x"));
    try {
        std::move(*sink).finalize();
        FAIL() << "a tree with a '.GIT' entry was written";
    } catch (Error & e) {
        EXPECT_EQ(e.info().msg.str(), "invalid name '.GIT' for a Git tree entry");
    }
}

TEST_F(TarballCacheGixTest, dereference_singleton_directory)
{
    auto writer = openTarballWriter(repoDir);
    auto inner = writeTestTree(*writer);
    auto outer = writeDirectory(*writer, {{"source", inner}});
    auto two = writeDirectory(*writer, {{"source", inner}, {"other", inner}});
    writer->flush();

    auto cache = openTarballCache(repoDir);
    EXPECT_EQ(cache->dereferenceSingletonDirectory(outer.hash), inner.hash);
    EXPECT_EQ(cache->dereferenceSingletonDirectory(two.hash), two.hash);
    EXPECT_EQ(cache->dereferenceSingletonDirectory(inner.hash), inner.hash);
}

TEST_F(TarballCacheGixTest, blob_root)
{
    auto writer = openTarballWriter(repoDir);
    auto file = writeFile(*writer, "just a file\n");
    writer->flush();

    auto accessor = openTarballCache(repoDir)->getAccessor(file.hash, {}, "");
    EXPECT_EQ(dump(*accessor), std::vector<std::string>{"file /: just a file\n"});
}

TEST_F(TarballCacheGixTest, missing_path)
{
    auto writer = openTarballWriter(repoDir);
    auto tree = writeTestTree(*writer);
    writer->flush();

    auto accessor = openTarballCache(repoDir)->getAccessor(tree.hash, {}, "");
    EXPECT_EQ(accessor->maybeLstat(CanonPath("/nope")), std::nullopt);
    EXPECT_EQ(accessor->maybeLstat(CanonPath("/hello.txt/below-a-file")), std::nullopt);
    EXPECT_FALSE(accessor->pathExists(CanonPath("/sub/nope")));
}

} // namespace nix::fetchers::gix

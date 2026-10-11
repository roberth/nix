#include "nix/fetchers/git-utils.hh"
#include "nix/fetchers/tarball-cache-gix.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/file-system.hh"
#include "nix/util/finally.hh"
#include "nix/util/reactor.hh"
#include "nix/util/signals.hh"

#include <archive.h>
#include <archive_entry.h>
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

/* A namespace of its own: in a unity build, this file shares a translation
   unit with the other tests of the fetchers. */
namespace nix::fetchers::gix::unpack_tests {

struct TarEntry
{
    std::string path;
    std::string contents;
    bool executable = false;
};

/**
 * A gzipped tar file with `entries`, all regular files.
 */
std::string makeTarGz(const std::vector<TarEntry> & entries)
{
    std::string out;
    auto * a = archive_write_new();
    Finally freeArchive([&]() { archive_write_free(a); });
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open(
        a,
        &out,
        nullptr,
        [](struct archive *, void * out, const void * data, size_t size) -> la_ssize_t {
            static_cast<std::string *>(out)->append(static_cast<const char *>(data), size);
            return size;
        },
        nullptr);
    for (auto & e : entries) {
        auto * entry = archive_entry_new();
        archive_entry_set_pathname(entry, e.path.c_str());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, e.executable ? 0755 : 0644);
        archive_entry_set_size(entry, e.contents.size());
        archive_entry_set_mtime(entry, 1700000000, 0);
        archive_write_header(a, entry);
        archive_write_data(a, e.contents.data(), e.contents.size());
        archive_entry_free(entry);
    }
    archive_write_close(a);
    return out;
}

const std::vector<TarEntry> testEntries = {
    {"top/hello.txt", "hello\n"},
    {"top/run.sh", "#!/bin/sh\necho hi\n", true},
    {"top/sub/deep.txt", "deep\n"},
};

/**
 * A bare repository, like the tarball cache, and a tarball to unpack into
 * it, in a temporary directory.
 */
class TarballUnpackTest : public ::testing::Test
{
    AutoDelete delTmpDir;

protected:
    std::filesystem::path tmpDir;
    std::filesystem::path repoDir;
    std::filesystem::path tarball;
    ref<FileTransfer> fileTransfer = makeFileTransfer();

public:
    void SetUp() override
    {
        tmpDir = createTempDir();
        delTmpDir = AutoDelete(tmpDir, true);
        repoDir = tmpDir / "tarball-cache";
        GitRepo::openRepo(repoDir, {.create = true, .bare = true});
        tarball = tmpDir / "test.tar.gz";
        nix::writeFile(tarball, makeTarGz(testEntries));
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
     * The download of the test tarball and its unpacking, as concurrent
     * tasks, the way `downloadIntoTarballCache()` runs them.
     */
    Task<UnpackedTree> downloadAndUnpack()
    {
        return downloadAndUnpackInto(repoDir);
    }

    Task<UnpackedTree> downloadAndUnpackInto(std::filesystem::path repoDir)
    {
        auto stream = make_ref<TransferStream>();
        std::optional<UnpackedTree> tree;
        std::vector<Task<void>> tasks;
        tasks.push_back([](Task<FileTransferResult> transfer) -> Task<void> {
            co_await std::move(transfer);
        }(fileTransfer->transferStreaming(FileTransferRequest(VerbatimURL("file://" + tarball.string())), stream)));
        tasks.push_back([](Task<UnpackedTree> unpack, std::optional<UnpackedTree> & out) -> Task<void> {
            out = co_await std::move(unpack);
        }(unpackIntoTarballCache(repoDir, stream, false), tree));
        co_await whenAll(std::move(tasks));
        co_return *tree;
    }

    /**
     * Start `task`, wait until `ready()`, then destroy the task. Return
     * how long the destruction took.
     */
    static std::chrono::milliseconds cancelWhen(Task<UnpackedTree> task, std::function<bool()> ready)
    {
        struct NoWake : WakeTarget
        {
            void wake() noexcept override {}
        };

        auto target = makeWakeTarget<NoWake>();
        EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);
        for (int i = 0; i < 1000 && !ready(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        EXPECT_TRUE(ready());

        auto start = std::chrono::steady_clock::now();
        task.reset();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    }
};

/**
 * Hold the writes to the tarball cache still in `phase` until they are
 * cancelled, for the duration of a test.
 */
struct PauseInPhase
{
    explicit PauseInPhase(std::string phase)
    {
        setTarballCachePhaseHook(
            [phase = std::move(phase)](std::string_view reached, const std::function<bool()> & isCancelled) {
                if (reached != phase)
                    return;
                while (!isCancelled())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
    }

    ~PauseInPhase()
    {
        setTarballCachePhaseHook({});
    }
};

TEST_F(TarballUnpackTest, unpacksTheTarballIntoOnePack)
{
    auto tree = runBlocking(downloadAndUnpack());

    EXPECT_EQ(tree.lastModified, 1700000000);

    auto files = packFiles();
    ASSERT_EQ(files.size(), 2u);
    EXPECT_TRUE(files[0].starts_with("pack-") && files[0].ends_with(".idx"));
    EXPECT_EQ(files[1], files[0].substr(0, files[0].size() - 4) + ".pack");

    auto cache = openTarballCache(repoDir);
    auto accessor = cache->getAccessor(cache->dereferenceSingletonDirectory(tree.treeHash), {}, "test");
    EXPECT_EQ(accessor->readFile(CanonPath("hello.txt")), "hello\n");
    EXPECT_EQ(accessor->readFile(CanonPath("sub/deep.txt")), "deep\n");
    EXPECT_TRUE(accessor->lstat(CanonPath("run.sh")).isExecutable);
}

TEST_F(TarballUnpackTest, cancellingWhileUnpackingLeavesTheCacheAsItWas)
{
    PauseInPhase pause("unpack");

    /* The unpacking holds still after the first file. By then, its thread
       has a writer with objects that are not flushed. */
    auto elapsed = cancelWhen(downloadAndUnpack(), [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return true;
    });

    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(packFiles(), std::vector<std::string>{});
}

TEST_F(TarballUnpackTest, cancellingWhileFlushingLeavesTheCacheAsItWas)
{
    PauseInPhase pause("flush");

    /* The flush holds still after it wrote the temporary pack, and before
       it writes the index. */
    auto elapsed = cancelWhen(downloadAndUnpack(), [&]() {
        auto files = packFiles();
        return files.size() == 1 && files[0].starts_with("tmp_pack_");
    });

    EXPECT_LT(elapsed, std::chrono::seconds(5));
    EXPECT_EQ(packFiles(), std::vector<std::string>{});
}

TEST_F(TarballUnpackTest, anErrorOfTheStreamKeepsItsType)
{
    auto stream = make_ref<TransferStream>();
    stream->close();

    /* The exception crosses from the C++ stream, through libarchive and
       the Rust future, back to C++. */
    EXPECT_THROW(runBlocking(unpackIntoTarballCache(repoDir, stream, false)), Cancelled);
    EXPECT_EQ(packFiles(), std::vector<std::string>{});
}

TEST_F(TarballUnpackTest, aNewTaskAfterACancelledOneUnpacksTheSameTree)
{
    auto freshRepoDir = tmpDir / "fresh";
    GitRepo::openRepo(freshRepoDir, {.create = true, .bare = true});
    auto expected = runBlocking(downloadAndUnpackInto(freshRepoDir)).treeHash;

    {
        PauseInPhase pause("flush");
        cancelWhen(downloadAndUnpack(), [&]() {
            auto files = packFiles();
            return files.size() == 1 && files[0].starts_with("tmp_pack_");
        });
    }

    EXPECT_EQ(runBlocking(downloadAndUnpack()).treeHash, expected);
    EXPECT_EQ(packFiles().size(), 2u);
}

TEST_F(TarballUnpackTest, unpacksOnTheReactor)
{
    auto tree = runOnReactor(downloadAndUnpack());

    EXPECT_EQ(tree.lastModified, 1700000000);
    EXPECT_EQ(packFiles().size(), 2u);
}

TEST_F(TarballUnpackTest, cancellingOnTheReactorWaitsForTheThreadInTheRun)
{
    PauseInPhase pause("flush");

    /* The race cancels the unpacking on the reactor thread, as soon as the
       flush holds still with a temporary pack. The drop must not join the
       unpacking thread there; it defers the join to the run. */
    auto flushing = [](const TarballUnpackTest & test) -> Task<void> {
        while (true) {
            auto files = test.packFiles();
            if (files.size() == 1 && files[0].starts_with("tmp_pack_"))
                co_return;
            co_await sleepFor(std::chrono::milliseconds(10));
        }
    };
    auto winner = runOnReactor(race(downloadAndUnpack(), flushing(*this)));

    EXPECT_EQ(winner.index(), 1u);
    /* `runOnReactor` returned after the deferred join, so the thread
       removed its temporary pack already. */
    EXPECT_EQ(packFiles(), std::vector<std::string>{});
}

} // namespace nix::fetchers::gix::unpack_tests

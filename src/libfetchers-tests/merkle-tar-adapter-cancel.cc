#include "nix/fetchers/merkle-tar-adapter.hh"
#include "nix/util/signals.hh"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

/* A namespace of its own: in a unity build, this file shares a translation
   unit with the other tests of the fetchers. */
namespace nix::merkle::cancel_tests {

using namespace std::chrono_literals;

/**
 * A store whose files take `fileDelay` each to write, like a slow machine
 * or a debug build. It records how many files were parsed but not written
 * yet, at most.
 */
struct SlowStore : FileSinkBuilder
{
    std::chrono::milliseconds fileDelay;
    std::atomic<int> outstanding = 0;
    std::atomic<int> maxOutstanding = 0;
    std::atomic<int> written = 0;

    explicit SlowStore(std::chrono::milliseconds fileDelay)
        : fileDelay(fileDelay)
    {
    }

    struct FileSink : RegularFileSinkWithFinalize
    {
        SlowStore & store;

        explicit FileSink(SlowStore & store)
            : store(store)
        {
            auto n = ++store.outstanding;
            for (auto max = store.maxOutstanding.load();
                 n > max && !store.maxOutstanding.compare_exchange_weak(max, n);)
                ;
        }

        void operator()(std::string_view data) override {}

        Hash finalize() && override
        {
            std::this_thread::sleep_for(store.fileDelay);
            --store.outstanding;
            ++store.written;
            return Hash(HashAlgorithm::SHA1);
        }
    };

    struct DirSink : DirectorySinkWithFinalize
    {
        void insertChild(std::string_view name, TreeEntry entry) override {}

        Hash finalize() && override
        {
            return Hash(HashAlgorithm::SHA1);
        }
    };

    std::unique_ptr<DirectorySinkWithFinalize> makeDirectorySink() override
    {
        return std::make_unique<DirSink>();
    }

    std::unique_ptr<RegularFileSinkWithFinalize> makeRegularFileSink() override
    {
        return std::make_unique<FileSink>(*this);
    }

    TreeEntry makeSymlink(const std::string & target) override
    {
        return {Mode::Symlink, Hash(HashAlgorithm::SHA1)};
    }

    void flush() override {}
};

/**
 * Parse `files` files of 1 KiB into `adapter` and flush it, on a thread.
 * The result is "done" or the type of the exception.
 */
std::future<std::string> parseOnThread(TarAdapter & adapter, int files)
{
    return std::async(std::launch::async, [&adapter, files]() -> std::string {
        try {
            for (int i = 0; i < files; ++i)
                adapter.createRegularFile(
                    CanonPath("file" + std::to_string(i)), false, [](Sink & sink) { sink(std::string(1024, 'x')); });
            adapter.flush();
            return "done";
        } catch (Cancelled &) {
            return "Cancelled";
        } catch (std::exception & e) {
            return std::string("other exception: ") + e.what();
        }
    });
}

TEST(TarAdapterCancel, theParseWaitsForTheWritersWhenTheBacklogIsFull)
{
    SlowStore store(5ms);
    auto adapter = makeTarSink(store, {}, TarAdapterBacklog{.maxFiles = 4, .maxBytes = 1 << 20});

    EXPECT_EQ(parseOnThread(*adapter, 100).get(), "done");

    EXPECT_EQ(store.written, 100);
    /* Four files in the backlog, and the one that the parse is reading. */
    EXPECT_LE(store.maxOutstanding, 5);
}

TEST(TarAdapterCancel, theBacklogIsBoundedInBytes)
{
    SlowStore store(5ms);
    /* Three files of 1 KiB fit, a fourth does not. */
    auto adapter = makeTarSink(store, {}, TarAdapterBacklog{.maxFiles = 100, .maxBytes = 3 * 1024});

    EXPECT_EQ(parseOnThread(*adapter, 50).get(), "done");

    EXPECT_EQ(store.written, 50);
    EXPECT_LE(store.maxOutstanding, 4);
}

/**
 * A pool of one thread is what a machine with one CPU gets. A full backlog
 * must still drain: when the parse thread had to be one of the writers, the
 * parse waited for room forever after the second file.
 */
TEST(TarAdapterCancel, onePoolThreadWritesAllFiles)
{
    SlowStore store(1ms);
    TarWriterPool writers(1);
    /* Ends a parse that hangs, so that the test fails instead. */
    std::atomic<bool> cancelled = false;
    auto adapter = makeTarSink(
        store, [&]() { return cancelled.load(); }, TarAdapterBacklog{.maxFiles = 2, .maxBytes = 1 << 20}, writers);

    auto result = parseOnThread(*adapter, 20);
    if (result.wait_for(10s) != std::future_status::ready)
        cancelled = true;

    EXPECT_EQ(result.get(), "done");
    EXPECT_EQ(store.written, 20);
}

/**
 * Concurrent archives share the threads of one pool, and each waits only
 * for its own files.
 */
TEST(TarAdapterCancel, adaptersShareAPool)
{
    TarWriterPool writers(2);
    SlowStore store1(1ms);
    SlowStore store2(1ms);
    auto adapter1 = makeTarSink(store1, {}, std::nullopt, writers);
    auto adapter2 = makeTarSink(store2, {}, std::nullopt, writers);

    auto result1 = parseOnThread(*adapter1, 50);
    auto result2 = parseOnThread(*adapter2, 30);

    EXPECT_EQ(result1.get(), "done");
    EXPECT_EQ(result2.get(), "done");
    EXPECT_EQ(store1.written, 50);
    EXPECT_EQ(store2.written, 30);
}

/**
 * A cancelled archive does not wait for the files of another archive that
 * are queued in the shared pool before its own.
 */
TEST(TarAdapterCancel, aCancellationDoesNotWaitForOtherArchives)
{
    TarWriterPool writers(1);
    /* 10 queued files of 100 ms: 1 s of work before any file of `adapter`. */
    SlowStore otherStore(100ms);
    auto other = makeTarSink(otherStore, {}, TarAdapterBacklog{.maxFiles = 10, .maxBytes = 1 << 20}, writers);
    auto otherResult = parseOnThread(*other, 10);
    std::this_thread::sleep_for(50ms);

    SlowStore store(1ms);
    std::atomic<bool> cancelled = false;
    auto adapter = makeTarSink(store, [&]() { return cancelled.load(); }, std::nullopt, writers);
    auto result = parseOnThread(*adapter, 5);
    std::this_thread::sleep_for(50ms);

    auto start = std::chrono::steady_clock::now();
    cancelled = true;
    auto outcome = result.get();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(outcome, "Cancelled");
    EXPECT_EQ(store.written, 0);
    EXPECT_LT(elapsed, 300ms);
    EXPECT_EQ(otherResult.get(), "done");
    EXPECT_EQ(otherStore.written, 10);
}

/**
 * Writing all files takes 200 × 100 ms / at most 20 files in the backlog
 * ≥ 1 s. A cancellation must not wait for that: neither for files that are
 * still to be parsed, nor for files that are queued.
 */
TEST(TarAdapterCancel, aCancellationDoesNotWaitForTheQueuedFiles)
{
    SlowStore store(100ms);
    std::atomic<bool> cancelled = false;
    auto adapter = makeTarSink(store, [&]() { return cancelled.load(); });

    auto result = parseOnThread(*adapter, 200);
    std::this_thread::sleep_for(300ms);

    auto start = std::chrono::steady_clock::now();
    cancelled = true;
    auto outcome = result.get();
    auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(outcome, "Cancelled");
    /* At most the files that the writers are writing: one round of
       100 ms, plus the 10 ms in which the parse notices. */
    EXPECT_LT(elapsed, 500ms);
    EXPECT_LT(store.written, 200);
}

} // namespace nix::merkle::cancel_tests

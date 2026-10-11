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
 * Writing all files takes 200 × 100 ms / at most 10 writers ≥ 2 s. A
 * cancellation must not wait for that: neither for files that are still to
 * be parsed, nor for files that are queued.
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

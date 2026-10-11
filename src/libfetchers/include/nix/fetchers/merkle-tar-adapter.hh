#pragma once
///@file

#include "nix/util/merkle-files.hh"
#include "nix/util/ref.hh"
#include "nix/util/tarfile.hh"

#include <boost/compat/move_only_function.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace nix::merkle {

/**
 * A merkle regular file sink that can be finalized to get the content hash.
 */
struct RegularFileSinkWithFinalize : virtual Sink
{
    /**
     * Finish the file, and return the hash of the blob.
     */
    virtual Hash finalize() && = 0;
};

/**
 * A merkle directory sink that can be finalized to get the tree hash.
 */
struct DirectorySinkWithFinalize : merkle::DirectorySink
{
    /**
     * Finish the directory, and return its tree hash.
     */
    virtual Hash finalize() && = 0;
};

/**
 * Interface for creating merkle file system object sinks.
 *
 * By returning sinks rather than taking callbacks, we allow starting
 * and finishing sink-based creation in an asynchronous manner. This is
 * crucial to being able to adapt messy providers of file system object
 * data (like tarballs) to this interface.
 *
 * A directory can only refer to objects that are already written, so
 * every sink for a directory's children must be finalized before it is
 * created. Implementations are responsible for whatever that ordering
 * requires of them --- see @ref nix::merkle::FileSinkBuilder::makeDirectorySink.
 */
struct FileSinkBuilder
{
    virtual ~FileSinkBuilder() = default;

    /**
     * Create a sink for one directory.
     *
     * Every sink handed out before this call must have been finalized:
     * an implementation is free to assume that, and to do whatever work
     * making earlier objects referenceable requires.
     */
    virtual std::unique_ptr<DirectorySinkWithFinalize> makeDirectorySink() = 0;

    virtual std::unique_ptr<RegularFileSinkWithFinalize> makeRegularFileSink() = 0;

    virtual merkle::TreeEntry makeSymlink(const std::string & target) = 0;

    /**
     * Finish any writes that are still in progress.
     *
     * Until this is called, objects written through this builder are not
     * necessarily available to anything else that reads the same store.
     */
    virtual void flush() = 0;
};

/**
 * Adapter that implements TarSink by building a merkle tree.
 */
struct TarAdapter : TarSink
{
    virtual merkle::TreeEntry flush() = 0;
};

/**
 * The bounds of the backlog of a `TarAdapter`: the files that it parsed
 * and that its writer threads have not finished. When the backlog is full,
 * the parse waits, so that a fast parse does not run ahead of the writers
 * with an unbounded amount of file contents in memory.
 *
 * A single file larger than `maxBytes` is admitted when the backlog is
 * empty.
 */
struct TarAdapterBacklog
{
    size_t maxFiles;
    uint64_t maxBytes;
};

/**
 * The threads that write the files of tar adapters, in the order in which
 * the adapters hand them over.
 *
 * All adapters of the process share one pool (`shared()`), so that the
 * number of writer threads does not grow with the number of archives that
 * are unpacked at once. With a pool per adapter, 200 concurrent fetches
 * ran more than 1000 threads, and the threads made each write more
 * expensive; see "Many fetches at once" in the developer documentation of
 * the tarball cache.
 */
class TarWriterPool
{
public:
    /**
     * A pool of at most `maxThreads` threads, at least 1. It starts a
     * thread when work is queued and no thread is idle.
     *
     * At most `maxPerOwner` work items of one owner run at the same time,
     * by default `maxThreads`. Each file that a thread writes occupies a
     * writer of the store: the libgit2 implementation writes a pack for
     * each, and more concurrent writes of one archive cost CPU time without
     * a faster unpack.
     */
    explicit TarWriterPool(size_t maxThreads, std::optional<size_t> maxPerOwner = std::nullopt);

    /**
     * Waits for the threads, which finish the work that they have started.
     * Every adapter that uses the pool must be destroyed before.
     */
    ~TarWriterPool();

    /**
     * The pool of the process: one thread per CPU, and at most 10 at the
     * same time for one archive, as with the pool of 10 threads that each
     * adapter had before. It is never destroyed, so its threads live as
     * long as the process.
     */
    static TarWriterPool & shared();

    /**
     * Move-only, so that a work item can own the sink that it finishes.
     */
    using Work = boost::compat::move_only_function<void()>;

    /**
     * Run `work` on a thread of the pool, for `owner`. `work` must not
     * throw.
     */
    void enqueue(const void * owner, Work work);

    /**
     * Remove the work of `owner` that no thread has started, and return
     * it. An owner that stops runs the work itself, so that it does not
     * wait for the work of other owners that is queued before its own.
     */
    std::vector<Work> takeQueued(const void * owner);

private:
    struct State;
    std::unique_ptr<State> state;
};

/**
 * Create a TarSink that builds a merkle tree from path-based tar entries.
 *
 * @param store Factory for creating merkle sinks
 *
 * @param isCancelled Whether the owner of the parse cancelled it. The
 * adapter checks it before it writes each file and directory, and while
 * it waits for room in the backlog, and then throws `Cancelled`; files
 * that are queued but not started are not written. Empty for a parse that
 * cannot be cancelled.
 *
 * @param backlog The bounds of the backlog. The default holds at most 20
 * files and at most 64 MiB.
 *
 * @param writers The threads that write the files; by default the pool of
 * the process. It must outlive the adapter.
 */
ref<TarAdapter> makeTarSink(
    FileSinkBuilder & store,
    std::function<bool()> isCancelled = {},
    std::optional<TarAdapterBacklog> backlog = std::nullopt,
    TarWriterPool & writers = TarWriterPool::shared());

} // namespace nix::merkle

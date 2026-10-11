#pragma once
///@file

#include "nix/util/merkle-files.hh"
#include "nix/util/ref.hh"
#include "nix/util/tarfile.hh"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

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
 * @param backlog The bounds of the backlog. The default holds at most two
 * files per writer thread, and at most 64 MiB.
 */
ref<TarAdapter> makeTarSink(
    FileSinkBuilder & store,
    std::function<bool()> isCancelled = {},
    std::optional<TarAdapterBacklog> backlog = std::nullopt);

} // namespace nix::merkle

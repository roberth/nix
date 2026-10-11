#include "nix/fetchers/merkle-tar-adapter.hh"
#include "nix/util/chunked-vector.hh"
#include "nix/util/signals.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <utility>
#include <thread>
#include <variant>

/**
 * Implementation of `nix::merkle::makeTarSink`.
 *
 * A namespace of its own because the names within are generic: internal
 * linkage alone would not keep them from colliding in a unity build.
 */
namespace nix::merkle::tar {

/**
 * Still want internal linkage too, though.
 */
namespace {

struct Directory;

/**
 * A regular file.
 *
 * The hash lives in a slot of its own rather than in the tree, because a
 * file is written while the rest of the archive is still being parsed,
 * and that parsing may overwrite this entry in the meantime. Overwriting
 * then just drops the pointer: the write finishes into a slot that nobody
 * reads. It also means a hard link can share the slot rather than copy
 * the hash out of it.
 */
struct File
{
    /**
     * Empty until whatever is writing the file has finished.
     */
    std::optional<Hash> * hash;

    bool executable;

    merkle::Mode mode() const
    {
        return executable ? merkle::Mode::Executable : merkle::Mode::Regular;
    }
};

/**
 * A symlink.
 *
 * Unlike a regular file, it is not sunk to the store asynchronously but
 * written in one go, because it is presumed to be short --- so it has its
 * hash from the start, and needs no slot.
 */
struct Symlink
{
    Hash hash;

    merkle::Mode mode() const
    {
        return merkle::Mode::Symlink;
    }
};

/**
 * An entry in a directory: another directory, a file, or a symlink. A
 * hard link is resolved when it is created, and so is just another one of
 * whatever it was made from.
 */
using Child = std::variant<Directory, File, Symlink>;

/**
 * A directory, to be written once all of its children are known.
 */
struct Directory
{
    std::map<std::string, Child, std::less<>> children;

    merkle::Mode mode() const
    {
        return merkle::Mode::Directory;
    }
};

/**
 * Adapter that builds a merkle tree from the flat, path-addressed
 * entries of a tar archive.
 *
 * 1. During tar parsing, build a tree of `Directory`, creating parents
 *    as needed and applying the overwrite semantics of `addNode`. Each
 *    file is handed to the store as soon as it has been parsed.
 * 2. At flush time:
 *    a. Wait for those writes to finish
 *    b. Resolve hard links
 *    c. Write the directories bottom-up (children before parents)
 */
struct TarAdapterImpl : merkle::TarAdapter
{
    merkle::FileSinkBuilder & store;

    /**
     * The root of the tree. Not necessarily a directory: a tar archive
     * may contain a single file (or symlink) and nothing else.
     */
    Child root = Directory{};

    /**
     * Groups of paths that name the same file, because hard links were
     * made between them. Every path in a group maps to the whole group,
     * so that replacing one of them can say what else it was linked to.
     */
    std::map<CanonPath, std::shared_ptr<std::set<CanonPath>>> hardLinked;

    /**
     * The hash of each file being written, filled in by the writer.
     *
     * Chunked so that the pointers we hand out stay put as more files
     * are added.
     */
    static constexpr size_t chunkSize = 1024;
    ChunkedVector<std::optional<Hash>, chunkSize, std::numeric_limits<uint32_t>::max() / chunkSize> results;

    /**
     * Whether the owner cancelled the parse. Empty if it cannot.
     */
    std::function<bool()> isCancelled;

    static constexpr size_t maxWorkers = 10;

    static size_t numWorkers()
    {
        return std::max<size_t>(std::min<size_t>(std::thread::hardware_concurrency(), maxWorkers), 1);
    }

    merkle::TarAdapterBacklog limits;

    /**
     * The files that were parsed and handed to `workers`, but are not
     * written yet, and their total size.
     *
     * Without a bound, the parse runs ahead of the writers whenever it is
     * faster than them: in a debug build, or on a machine with few cores.
     * The contents of all queued files are then in memory, and a
     * cancellation has to wait for the writers to drain the queue.
     */
    struct Backlog
    {
        size_t files = 0;
        uint64_t bytes = 0;

        /**
         * A writer failed. `workers` has stopped, and the files that are
         * still queued will not be written.
         */
        bool failed = false;
    };

    std::mutex backlogMutex;
    std::condition_variable backlogChanged;
    Backlog backlog;

    /**
     * Writes the files as they are parsed, so that hashing them overlaps
     * with parsing the rest of the archive.
     *
     * Bounded, because each worker occupies a separate writer in the
     * store, which is not necessarily free to have around.
     *
     * Declared last, so that the workers are joined before anything they
     * write into is destroyed.
     */
    ThreadPool workers{numWorkers()};

    TarAdapterImpl(
        merkle::FileSinkBuilder & store,
        std::function<bool()> isCancelled,
        std::optional<merkle::TarAdapterBacklog> backlog)
        : store(store)
        , isCancelled(std::move(isCancelled))
        , limits(backlog.value_or(
              merkle::TarAdapterBacklog{
                  .maxFiles = 2 * numWorkers(),
                  .maxBytes = 64 * 1024 * 1024,
              }))
    {
    }

    void checkCancelled() const
    {
        if (isCancelled && isCancelled())
            throw Cancelled("writing the files of an archive was cancelled");
    }

    /**
     * Wait until the backlog has room for a file of `size` bytes, and
     * count the file in it.
     *
     * If a writer failed, rethrow its exception instead: the workers have
     * stopped, so the backlog would never shrink.
     */
    void enterBacklog(uint64_t size)
    {
        std::unique_lock lock(backlogMutex);
        while (true) {
            if (backlog.failed) {
                lock.unlock();
                /* Rethrows the exception of the writer. */
                workers.process();
                unreachable();
            }
            if (backlog.files == 0 || (backlog.files < limits.maxFiles && backlog.bytes + size <= limits.maxBytes))
                break;
            /* Wake up regularly to see a cancellation, which does not
               notify `backlogChanged`. */
            backlogChanged.wait_for(lock, std::chrono::milliseconds(10));
            checkCancelled();
        }
        backlog.files++;
        backlog.bytes += size;
    }

    void leaveBacklog(uint64_t size, bool failed)
    {
        {
            std::lock_guard lock(backlogMutex);
            backlog.files--;
            backlog.bytes -= size;
            if (failed)
                backlog.failed = true;
        }
        backlogChanged.notify_all();
    }

    /**
     * The entry at `path`, or null if there is none --- including when
     * something along the way is not a directory.
     */
    Child * lookup(const CanonPath & path)
    {
        auto * cur = &root;
        for (auto & name : path) {
            auto * dir = std::get_if<Directory>(cur);
            if (!dir)
                return nullptr;
            auto i = dir->children.find(name);
            if (i == dir->children.end())
                return nullptr;
            cur = &i->second;
        }
        return cur;
    }

    /**
     * Insert an entry, creating parent directories as needed, and
     * applying tarball unpacking overwrite semantics.
     *
     * We behave like libarchive without `ARCHIVE_EXTRACT_NO_OVERWRITE`:
     * the last entry wins. The exception is a non-empty directory:
     * libarchive just tries to `unlink()` whatever is in the way, which
     * only succeeds for an empty directory, so replacing a non-empty one
     * is an error.
     */
    void addNode(const CanonPath & path, Child && child)
    {
        auto * cur = &root;

        if (auto parent = path.parent()) {
            for (auto & name : *parent) {
                auto * dir = std::get_if<Directory>(cur);
                if (!dir)
                    throw Error("parent of '%s' is not a directory", path);
                cur = &dir->children.emplace(std::string(name), Directory{}).first->second;
            }

            auto * dir = std::get_if<Directory>(cur);
            if (!dir)
                throw Error("parent of '%s' is not a directory", path);

            std::string name(*path.baseName());
            auto i = dir->children.find(name);
            if (i == dir->children.end()) {
                dir->children.emplace(std::move(name), std::move(child));
                return;
            }
            cur = &i->second;
        }

        /* Overwriting something that is already there. */
        if (auto * prev = std::get_if<Directory>(cur)) {
            /* "Replacing" a directory with a directory is always a-ok. */
            if (std::holds_alternative<Directory>(child))
                return;

            if (!prev->children.empty())
                throw Error("cannot create '%s', conflicting non-empty directory", path);
        }

        if (auto i = hardLinked.find(path); i != hardLinked.end()) {
            auto group = i->second;
            hardLinked.erase(i);
            group->erase(path);

            warn(
                "'%s' is replaced, though a hard link makes it the same file as %s. Older versions of Nix "
                "resolved hard links differently, so this archive unpacks to a different store path than it "
                "used to.",
                path,
                concatMapStringsSep(", ", *group, [](const CanonPath & other) { return quoteString(other.abs()); }));

            /* One path on its own is not linked to anything any more. */
            if (group->size() < 2)
                for (auto & other : *group)
                    hardLinked.erase(other);
        }

        *cur = std::move(child);
    }

    void createDirectory(const CanonPath & path) override
    {
        checkCancelled();
        addNode(path, Directory{});
    }

    void createRegularFile(const CanonPath & path, bool isExecutable, fun<void(Sink &)> callback) override
    {
        checkCancelled();

        auto sink = store.makeRegularFileSink();
        uint64_t size = 0;
        {
            LambdaSink counter([&](std::string_view data) {
                size += data.size();
                (*sink)(data);
            });
            callback(counter);
        }

        enterBacklog(size);

        /* The file is complete, so start writing it now: that way the
           store is busy with it while we parse the rest of the archive,
           and it lets go of whatever resources it holds for this file as
           soon as it is done. */
        auto & hash = results.add().first;
        try {
            workers.enqueue([this, sink = std::move(sink), &hash, size]() {
                /* A cancelled parse writes none of the files that are
                   still queued. Skip the file instead of throwing: the
                   pool prints every exception after the first, and
                   `flush()` throws `Cancelled` once. */
                if (isCancelled && isCancelled()) {
                    leaveBacklog(size, false);
                    return;
                }
                try {
                    hash = std::move(*sink).finalize();
                } catch (...) {
                    leaveBacklog(size, true);
                    throw;
                }
                leaveBacklog(size, false);
            });
        } catch (ThreadPoolShutDown &) {
            /* A writer failed after `enterBacklog()`. Rethrow its
               exception. */
            leaveBacklog(size, true);
            workers.process();
            unreachable();
        }

        addNode(path, File{.hash = &hash, .executable = isExecutable});
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        checkCancelled();
        addNode(path, Symlink{.hash = store.makeSymlink(target).hash});
    }

    void createHardlink(const CanonPath & path, const CanonPath & target) override
    try {
        checkCancelled();
        /* Resolved now rather than at flush, because a hard link refers
           to whatever is at `target` at this point in the archive, the
           way it would refer to an inode. A later entry replacing
           `target` gets a slot of its own, so this link keeps pointing
           at the old one --- which is what libarchive does, as it
           unlinks the file in the way rather than writing through it. */
        const auto * child = lookup(target);

        if (!child)
            throw Error("target does not exist");

        std::visit(
            overloaded{
                [&](const Directory &) { throw Error("target is a directory"); },
                /* Share the target's hash slot, rather than copying the
                   hash out of it --- which is also why a link made
                   before the target has finished being written works. */
                [&](const File & file) { addNode(path, File{file}); },
                [&](const Symlink & link) { addNode(path, Symlink{link}); },
            },
            std::as_const(*child));

        /* `path` and `target` are now names for one file. */
        auto group = [&] {
            if (auto i = hardLinked.find(target); i != hardLinked.end())
                return i->second;
            auto fresh = std::make_shared<std::set<CanonPath>>(std::set<CanonPath>{target});
            hardLinked.emplace(target, fresh);
            return fresh;
        }();
        group->insert(path);
        hardLinked.insert_or_assign(path, group);
    } catch (Error & e) {
        e.addTrace(nullptr, "while creating a hard link from '%s' to '%s'", path, target);
        throw;
    }

    merkle::TreeEntry flush() override
    {
        /* Wait for the files to be written. An exception in a worker is
           propagated here, so every hash below is filled in. A
           cancellation stops the workers before their next file, and the
           queue has at most `limits.maxFiles` files, so this does not
           wait for a backlog of files to drain. */
        workers.process();

        /* Skipped files have no hash. */
        checkCancelled();

        /* Write the directories, children before parents, which the
           recursion gives us for free. */
        /* `self`, not `this`: GCC does not allow implicit member access
           in a lambda with an explicit object parameter. */
        auto entry = [self = this](this const auto & write, const Child & child) -> merkle::TreeEntry {
            return {
                .mode = std::visit([](const auto & sub) { return sub.mode(); }, child),
                .hash = std::visit(
                    overloaded{
                        [&](const Directory & dir) {
                            self->checkCancelled();
                            auto dirSink = self->store.makeDirectorySink();
                            for (auto & [name, sub] : dir.children)
                                dirSink->insertChild(name, write(sub));
                            return std::move(*dirSink).finalize();
                        },
                        [&](const File & file) {
                            /* Filled in by the writers we waited for just above. */
                            assert(file.hash->has_value());
                            return **file.hash;
                        },
                        [&](const Symlink & link) { return link.hash; },
                    },
                    child),
            };
        }(root);

        store.flush();

        return entry;
    }
};

} // namespace

} // namespace nix::merkle::tar

namespace nix::merkle {

ref<TarAdapter>
makeTarSink(FileSinkBuilder & store, std::function<bool()> isCancelled, std::optional<TarAdapterBacklog> backlog)
{
    return make_ref<tar::TarAdapterImpl>(store, std::move(isCancelled), backlog);
}

} // namespace nix::merkle

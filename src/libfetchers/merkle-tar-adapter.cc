#include "nix/fetchers/merkle-tar-adapter.hh"
#include "nix/util/chunked-vector.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/util.hh"

#ifdef __linux__
#  include "nix/util/linux-namespaces.hh"
#endif

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <utility>
#include <thread>
#include <unordered_map>
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

    /**
     * The most regular files in one archive: 64 Mi. `ChunkedVector`
     * allocates and zeroes its array of chunk pointers up front, so the
     * bound is memory that each unpacking holds: 512 KiB. With the bound
     * of `uint32_t`, it was 32 MiB per unpacking, all of it written: 200
     * concurrent fetches of small tarballs peaked at 4.7 GB of RSS.
     */
    static constexpr size_t maxFiles = 64 * 1024 * 1024;

    ChunkedVector<std::optional<Hash>, chunkSize, maxFiles / chunkSize> results;

    /**
     * Whether the owner cancelled the parse. Empty if it cannot.
     */
    std::function<bool()> isCancelled;

    /**
     * The default bound of the backlog in files: 2 files for each of 10
     * writers, or fewer on a machine with fewer CPUs.
     *
     * The bound is also the number of files of one archive that are
     * written at the same time. It stays what it was with a pool of 10
     * writers per archive, because each file that is being written
     * occupies a separate writer in the store, which is not necessarily
     * free to have around: the libgit2 implementation writes a pack for
     * each.
     */
    static size_t defaultMaxFiles()
    {
        return 2 * std::max<size_t>(std::min<size_t>(std::thread::hardware_concurrency(), 10), 1);
    }

    merkle::TarAdapterBacklog limits;

    /**
     * The files that were parsed and handed to `writers`, but are not
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
         * The exception of the first writer that failed. The files that
         * are still queued are not written.
         */
        std::exception_ptr error;
    };

    std::mutex backlogMutex;
    std::condition_variable backlogChanged;
    Backlog backlog;

    /**
     * Whether a writer failed, or the adapter is being destroyed: the
     * files that are still queued are not written. Atomic, so that the
     * writers can check it without `backlogMutex`.
     */
    std::atomic<bool> stopWriting = false;

    /**
     * Writes the files as they are parsed, so that hashing them overlaps
     * with parsing the rest of the archive.
     */
    merkle::TarWriterPool & writers;

    TarAdapterImpl(
        merkle::FileSinkBuilder & store,
        std::function<bool()> isCancelled,
        std::optional<merkle::TarAdapterBacklog> backlog,
        merkle::TarWriterPool & writers)
        : store(store)
        , isCancelled(std::move(isCancelled))
        , limits(backlog.value_or(
              merkle::TarAdapterBacklog{
                  .maxFiles = defaultMaxFiles(),
                  .maxBytes = 64 * 1024 * 1024,
              }))
        , writers(writers)
    {
    }

    /**
     * The writers refer to this adapter and to the sinks of `store`, so
     * wait for them; the files that are still queued are skipped.
     */
    ~TarAdapterImpl()
    {
        stopWriting = true;
        waitForWriters();
    }

    /**
     * Wait until no file of this adapter is queued or being written.
     *
     * When the files are not to be written any more, take them out of the
     * queue of the pool, which other adapters share, and skip them here:
     * a cancellation then waits only for the files that a thread already
     * writes, not for the files of other archives before ours.
     */
    void waitForWriters()
    {
        bool taken = false;
        std::unique_lock lock(backlogMutex);
        while (backlog.files != 0) {
            if (!taken && (stopWriting || (isCancelled && isCancelled()))) {
                taken = true;
                lock.unlock();
                for (auto & work : writers.takeQueued(this))
                    work();
                lock.lock();
                continue;
            }
            /* Wake up regularly to see a cancellation, which does not
               notify `backlogChanged`. */
            backlogChanged.wait_for(lock, std::chrono::milliseconds(10));
        }
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
     * If a writer failed, rethrow its exception instead: the files after
     * it are not written.
     */
    void enterBacklog(uint64_t size)
    {
        std::unique_lock lock(backlogMutex);
        while (true) {
            if (backlog.error)
                std::rethrow_exception(backlog.error);
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

    /**
     * Take a file out of the backlog. This is the last access of a writer
     * to the adapter: once the backlog is empty, the destructor or
     * `flush()` returns, and the adapter can be gone. So notify under the
     * lock, before the waiter can see the empty backlog.
     */
    void leaveBacklog(uint64_t size, std::exception_ptr error)
    {
        std::lock_guard lock(backlogMutex);
        backlog.files--;
        backlog.bytes -= size;
        if (error && !backlog.error) {
            backlog.error = std::move(error);
            stopWriting = true;
        }
        backlogChanged.notify_all();
    }

    /**
     * Write one file, on a thread of `writers`.
     */
    void writeFile(
        std::unique_ptr<merkle::RegularFileSinkWithFinalize> sink, std::optional<Hash> & hash, uint64_t size) noexcept
    {
        std::exception_ptr error;
        /* A cancelled parse, a failed writer or the destruction of the
           adapter skips the files that are still queued. */
        if (!stopWriting && !(isCancelled && isCancelled())) {
#ifndef _WIN32
            /* Like a `ThreadPool`: a `checkInterrupt()` in the store stops
               once another file failed. */
            unix::interruptCheck = [this]() { return stopWriting.load(); };
#endif
            try {
                hash = std::move(*sink).finalize();
            } catch (...) {
                error = std::current_exception();
            }
#ifndef _WIN32
            unix::interruptCheck = nullptr;
#endif
        }
        /* The sink belongs to `store`, which may be gone once the backlog
           is empty. */
        sink.reset();
        leaveBacklog(size, std::move(error));
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

        if (results.size() >= maxFiles)
            throw Error("the archive has more than %d regular files", maxFiles);

        enterBacklog(size);

        /* The file is complete, so start writing it now: that way the
           store is busy with it while we parse the rest of the archive,
           and it lets go of whatever resources it holds for this file as
           soon as it is done. */
        auto & hash = results.add().first;
        try {
            writers.enqueue(this, [this, sink = std::move(sink), &hash, size]() mutable noexcept {
                writeFile(std::move(sink), hash, size);
            });
        } catch (...) {
            /* The pool could not start a thread. */
            leaveBacklog(size, std::current_exception());
            throw;
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
        /* Wait for the files to be written. An exception in a writer is
           propagated here, so every hash below is filled in. A
           cancellation stops the writers before their next file, and the
           queue has at most `limits.maxFiles` files, so this does not
           wait for a backlog of files to drain. */
        waitForWriters();
        {
            std::lock_guard lock(backlogMutex);
            if (backlog.error)
                std::rethrow_exception(backlog.error);
        }

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

struct TarWriterPool::State
{
    size_t maxThreads;
    size_t maxPerOwner;

    std::mutex mutex;
    std::condition_variable workAvailable;

    struct Queued
    {
        const void * owner;
        Work work;
    };

    std::deque<Queued> queue;
    std::vector<std::thread> threads;

    /**
     * The number of work items that run, for each owner that has any.
     */
    std::unordered_map<const void *, size_t> running;

    /**
     * The first queued item whose owner runs fewer than `maxPerOwner`
     * items.
     */
    std::deque<Queued>::iterator findRunnable()
    {
        return std::ranges::find_if(queue, [&](const Queued & queued) {
            auto i = running.find(queued.owner);
            return i == running.end() || i->second < maxPerOwner;
        });
    }

    /**
     * The number of queued items that may run now: for each owner, its
     * queued items, but at most as many as its bound leaves room for.
     */
    size_t countRunnable() const
    {
        std::unordered_map<const void *, size_t> room;
        size_t n = 0;
        for (auto & queued : queue) {
            auto [i, inserted] = room.try_emplace(queued.owner, 0);
            if (inserted) {
                auto r = running.find(queued.owner);
                i->second = maxPerOwner - (r == running.end() ? 0 : r->second);
            }
            if (i->second > 0) {
                --i->second;
                ++n;
            }
        }
        return n;
    }

    /**
     * The number of items that run, the sum of `running`.
     */
    size_t busy = 0;

    bool quit = false;

    void run() noexcept
    {
#ifdef __linux__
        /* The threads of the shared pool live as long as the process.
           Like the other long-lived threads, they must not keep the main
           thread from unsharing its mount namespace. */
        try {
            tryUnshareFilesystem();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
#endif
        /* Like a `ThreadPool`: a user interrupt reaches a writer that
           waits in a system call. */
        ReceiveInterrupts receiveInterrupts;

        std::unique_lock lock(mutex);
        while (true) {
            auto next = findRunnable();
            if (next == queue.end()) {
                /* The queue is empty, or its owners run as many items as
                   they may. A thread that finishes an item of such an
                   owner takes the next one itself. */
                if (quit && queue.empty())
                    return;
                workAvailable.wait(lock, [&] { return (quit && queue.empty()) || findRunnable() != queue.end(); });
                continue;
            }
            auto owner = next->owner;
            {
                auto work = std::move(next->work);
                queue.erase(next);
                ++running[owner];
                ++busy;
                lock.unlock();
                work();
                /* `work` and what it owns go before the lock is taken
                   again. */
            }
            lock.lock();
            --busy;
            if (--running[owner] == 0)
                running.erase(owner);
        }
    }
};

TarWriterPool::TarWriterPool(size_t maxThreads, std::optional<size_t> maxPerOwner)
    : state(std::make_unique<State>())
{
    state->maxThreads = std::max<size_t>(maxThreads, 1);
    state->maxPerOwner = std::max<size_t>(maxPerOwner.value_or(state->maxThreads), 1);
}

TarWriterPool::~TarWriterPool()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(state->mutex);
        state->quit = true;
        threads = std::move(state->threads);
    }
    state->workAvailable.notify_all();
    for (auto & thread : threads)
        thread.join();
}

TarWriterPool & TarWriterPool::shared()
{
    /* Never destroyed: an adapter can be in use while static destructors
       run, and the threads would have to be joined at exit. */
    static auto * pool = new TarWriterPool(
        std::thread::hardware_concurrency(), std::min<size_t>(std::thread::hardware_concurrency(), 10));
    return *pool;
}

std::vector<TarWriterPool::Work> TarWriterPool::takeQueued(const void * owner)
{
    std::vector<Work> taken;
    std::lock_guard lock(state->mutex);
    std::erase_if(state->queue, [&](State::Queued & queued) {
        if (queued.owner != owner)
            return false;
        taken.push_back(std::move(queued.work));
        return true;
    });
    return taken;
}

void TarWriterPool::enqueue(const void * owner, Work work)
{
    {
        std::lock_guard lock(state->mutex);
        state->queue.push_back({owner, std::move(work)});
        /* Start a thread when the threads that do not run an item cannot
           take all the work that may run now. These include threads that
           have just started or been notified, so a burst of work does not
           start more threads than it can use. Work of an owner at its bound
           does not count: a thread that finishes an item of that owner
           takes it. Each thread that uses the store of the tarball cache
           makes its writes more expensive (see `TarWriterPool`). */
        if (state->countRunnable() > state->threads.size() - state->busy && state->threads.size() < state->maxThreads) {
            try {
                state->threads.emplace_back([s = state.get()]() { s->run(); });
            } catch (...) {
                /* Without any thread, the work would never run. */
                if (state->threads.empty()) {
                    state->queue.pop_back();
                    throw;
                }
            }
        }
    }
    state->workAvailable.notify_one();
}

ref<TarAdapter> makeTarSink(
    FileSinkBuilder & store,
    std::function<bool()> isCancelled,
    std::optional<TarAdapterBacklog> backlog,
    TarWriterPool & writers)
{
    return make_ref<tar::TarAdapterImpl>(store, std::move(isCancelled), backlog, writers);
}

} // namespace nix::merkle

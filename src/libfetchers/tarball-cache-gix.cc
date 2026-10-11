#include "nix/fetchers/tarball-cache-gix.hh"

#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/merkle-tar-adapter.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/sync.hh"
#include "nix/util/tarfile.hh"

#include "nix/rust/tarball-cache.hh"
#include "nix/rust/tarball_cache.rs.h"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

/* All names are in this namespace, including those with internal linkage:
   in a unity build this file shares a translation unit with `git-utils.cc`,
   which has similar names. */
namespace nix::fetchers::gix {

namespace {

using Oid = std::array<uint8_t, 20>;

Oid toOid(const Hash & hash)
{
    if (hash.algo != HashAlgorithm::SHA1)
        throw Error("the tarball cache supports only SHA-1 Git objects, not '%s'", hash.to_string(HashFormat::SRI, true));
    Oid oid;
    std::memcpy(oid.data(), hash.hash, oid.size());
    return oid;
}

Hash toHash(const Oid & oid)
{
    Hash hash(HashAlgorithm::SHA1);
    std::memcpy(hash.hash, oid.data(), oid.size());
    return hash;
}

std::string toString(const ::rust::Vec<uint8_t> & bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/**
 * Call into Rust, and turn an `Err` into a Nix exception.
 *
 * An `Err` that comes from an exception in a C++ hook, for example
 * `Interrupted`, keeps only its message on the way through Rust. Calling
 * `checkInterrupt()` again throws the original exception type.
 */
template<typename F>
auto callRust(F && f) -> decltype(f())
{
    try {
        return f();
    } catch (::rust::Error & e) {
        checkInterrupt();
        throw Error("%s", Uncolored(e.what()));
    }
}

struct Hooks final : rs::TarballCacheHooks
{
    void logDebug(::rust::Str message) const override
    {
        debug("%s", std::string_view(message.data(), message.size()));
    }
};

/**
 * A cancellation token that a user interrupt sets, for the synchronous
 * writer, which has no owner that could cancel it otherwise.
 */
struct InterruptToken
{
    ::rust::Box<rs::CancelToken> token = rs::cancel_token_new();
    std::unique_ptr<InterruptCallback> callback =
        createInterruptCallback([token = &*token]() { token->cancel(); });

    InterruptToken()
    {
        /* The callback only sees interrupts from now on. */
        if (getInterrupted())
            token->cancel();
    }
};

/* Writing. */

struct RegularFileSink final : merkle::RegularFileSinkWithFinalize
{
    const rs::TarballCacheWriter & writer;

    /**
     * The contents. Git hashes the size of an object before its contents,
     * so the size must be known before hashing can start.
     */
    std::string contents;

    RegularFileSink(const rs::TarballCacheWriter & writer)
        : writer(writer)
    {
    }

    void operator()(std::string_view data) override
    {
        contents += data;
    }

    Hash finalize() && override
    {
        auto data = ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(contents.data()), contents.size());
        return toHash(callRust([&] { return writer.write_blob(data); }));
    }
};

struct DirectorySink final : merkle::DirectorySinkWithFinalize
{
    const rs::TarballCacheWriter & writer;

    std::vector<rs::GitTreeEntry> entries;

    DirectorySink(const rs::TarballCacheWriter & writer)
        : writer(writer)
    {
    }

    void insertChild(std::string_view name, merkle::TreeEntry entry) override
    {
        ::rust::Vec<uint8_t> nameBytes;
        nameBytes.reserve(name.size());
        for (char c : name)
            nameBytes.push_back(static_cast<uint8_t>(c));
        entries.push_back(
            rs::GitTreeEntry{
                .name = std::move(nameBytes),
                .mode = static_cast<uint32_t>(entry.mode),
                .id = toOid(entry.hash),
            });
    }

    Hash finalize() && override
    {
        auto slice = ::rust::Slice<const rs::GitTreeEntry>(entries.data(), entries.size());
        return toHash(callRust([&] { return writer.write_tree(slice); }));
    }
};

/**
 * The writer of the tarball cache. Unlike the libgit2 implementation, it
 * has no pool of repository handles and no separate phase for directories:
 * the Rust writer is thread-safe, and it writes a tree without looking up
 * its children.
 */
struct TarballWriter final : GitRepoPool
{
    ::rust::Box<rs::TarballCacheWriter> writer;

    TarballWriter(const std::filesystem::path & repoDir)
        : writer(callRust([&] { return rs::tarball_cache_writer_open((repoDir / "objects").string()); }))
    {
    }

    std::unique_ptr<merkle::DirectorySinkWithFinalize> makeDirectorySink() override
    {
        return std::make_unique<DirectorySink>(*writer);
    }

    std::unique_ptr<merkle::RegularFileSinkWithFinalize> makeRegularFileSink() override
    {
        return std::make_unique<RegularFileSink>(*writer);
    }

    merkle::TreeEntry makeSymlink(const std::string & target) override
    {
        auto data = ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(target.data()), target.size());
        return merkle::TreeEntry{
            .mode = merkle::Mode::Symlink,
            .hash = toHash(callRust([&] { return writer->write_blob(data); })),
        };
    }

    void flush() override
    {
        Hooks hooks;
        InterruptToken interrupt;
        callRust([&] { writer->flush(*interrupt.token, hooks); });
    }

    uint64_t getRevCount(const Hash & rev) override
    {
        throw UnimplementedError("the tarball cache has no commits, so it cannot count them");
    }
};

/* Unpacking as a task. */

/**
 * The sinks of a writer that the Rust unpack future owns. `flush()` does
 * nothing: the future flushes the writer itself, after the parse.
 */
struct UnownedWriterSinks final : merkle::FileSinkBuilder
{
    const rs::TarballCacheWriter & writer;

    UnownedWriterSinks(const rs::TarballCacheWriter & writer)
        : writer(writer)
    {
    }

    std::unique_ptr<merkle::DirectorySinkWithFinalize> makeDirectorySink() override
    {
        return std::make_unique<DirectorySink>(writer);
    }

    std::unique_ptr<merkle::RegularFileSinkWithFinalize> makeRegularFileSink() override
    {
        return std::make_unique<RegularFileSink>(writer);
    }

    merkle::TreeEntry makeSymlink(const std::string & target) override
    {
        auto data = ::rust::Slice<const uint8_t>(reinterpret_cast<const uint8_t *>(target.data()), target.size());
        return merkle::TreeEntry{
            .mode = merkle::Mode::Symlink,
            .hash = toHash(callRust([&] { return writer.write_blob(data); })),
        };
    }

    void flush() override {}
};

void throwIfCancelled(const rs::CancelToken & cancel)
{
    if (cancel.isCancelled())
        throw Cancelled("unpacking into the tarball cache was cancelled");
}

/**
 * A `Source` that remembers the exception that it threw. libarchive turns
 * an exception of its source into an error message, so the unpacker
 * rethrows the remembered exception instead, with its dynamic type: the
 * error of the download, or `Cancelled`.
 */
struct RememberingSource final : Source
{
    Source & source;
    const rs::CancelToken & cancel;
    std::exception_ptr error;

    RememberingSource(Source & source, const rs::CancelToken & cancel)
        : source(source)
        , cancel(cancel)
    {
    }

    size_t read(char * data, size_t len) override
    {
        try {
            throwIfCancelled(cancel);
            return source.read(data, len);
        } catch (EndOfFile &) {
            throw;
        } catch (...) {
            error = std::current_exception();
            throw;
        }
    }
};

/**
 * A `TarSink` that checks for cancellation between the entries of the
 * archive, and that holds still in the "unpack" phase for a test
 * (`_NIX_TEST_TARBALL_CACHE_PAUSE=unpack`).
 */
struct CancellableTarSink final : TarSink
{
    TarSink & sink;
    const rs::CancelToken & cancel;
    bool pauseForTest = getEnv("_NIX_TEST_TARBALL_CACHE_PAUSE") == "unpack";

    CancellableTarSink(TarSink & sink, const rs::CancelToken & cancel)
        : sink(sink)
        , cancel(cancel)
    {
    }

    void check()
    {
        throwIfCancelled(cancel);
    }

    void createDirectory(const CanonPath & path) override
    {
        check();
        sink.createDirectory(path);
    }

    void createRegularFile(const CanonPath & path, bool isExecutable, fun<void(Sink &)> callback) override
    {
        check();
        sink.createRegularFile(path, isExecutable, callback);
        if (pauseForTest) {
            pauseForTest = false;
            /* Not a log message: a test waits for this exact line. */
            std::fprintf(stderr, "nix: tarball cache test hook: paused in phase 'unpack'\n");
            while (!cancel.isCancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check();
        }
    }

    void createSymlink(const CanonPath & path, const std::string & target) override
    {
        check();
        sink.createSymlink(path, target);
    }

    void createHardlink(const CanonPath & path, const CanonPath & target) override
    {
        check();
        sink.createHardlink(path, target);
    }
};

/**
 * Parses an archive from a `TransferStream` and writes it with the writer
 * of the Rust unpack future, on the thread of that future.
 */
struct Unpacker final : rs::TarballUnpacker
{
    ref<TransferStream> stream;
    bool isZip;
    Hooks hooks_;

    Unpacker(ref<TransferStream> stream, bool isZip)
        : stream(std::move(stream))
        , isZip(isZip)
    {
    }

    nix_exception * run(
        const rs::TarballCacheWriter & writer,
        const rs::CancelToken & cancel,
        rs::TarballUnpackResult & out) const noexcept override
    {
        RememberingSource source(*stream, cancel);
        try {
            AutoDelete cleanupTemp;

            auto archive = isZip ? ({
                /* In streaming mode, libarchive doesn't handle symlinks
                   in zip files correctly (#10649). So write the entire
                   file to disk so libarchive can access it in
                   random-access mode. */
                auto [fdTemp, path] = createTempFile("nix-zipfile");
                cleanupTemp.cancel();
                cleanupTemp = {path};
                {
                    FdSink sink(fdTemp.get());
                    source.drainInto(sink);
                }
                TarArchive{path};
            })
                                 : TarArchive{source};

            UnownedWriterSinks sinks(writer);
            auto adapter = merkle::makeTarSink(sinks);
            CancellableTarSink sink(*adapter, cancel);
            auto lastModified = unpackTarfileToSink(archive, sink);
            throwIfCancelled(cancel);
            auto root = adapter->flush();

            out.tree = toOid(root.hash);
            out.last_modified = lastModified;
            return nullptr;
        } catch (...) {
            if (source.error) {
                try {
                    std::rethrow_exception(source.error);
                } catch (...) {
                    return rs::currentException();
                }
            }
            return rs::currentException();
        }
    }

    void cancel() const noexcept override
    {
        stream->close();
    }

    const rs::TarballCacheHooks & hooks() const noexcept override
    {
        return hooks_;
    }
};

} // namespace

} // namespace nix::fetchers::gix

namespace nix::rs {

template<>
struct AbiType<TarballUnpackResult>
{
    static constexpr const char * name = "TarballUnpackResult";
};

} // namespace nix::rs

namespace nix::fetchers::gix {

Task<UnpackedTree> unpackIntoTarballCache(std::filesystem::path repoDir, ref<TransferStream> stream, bool isZip)
{
    auto result = co_await awaitCFuture<rs::TarballUnpackResult>(rs::tarball_cache_unpack(
        (repoDir / "objects").string(), std::make_unique<Unpacker>(std::move(stream), isZip)));
    co_return UnpackedTree{
        .treeHash = toHash(result.tree),
        .lastModified = (time_t) result.last_modified,
    };
}

namespace {

/* Reading. */

struct Entry
{
    uint32_t mode;
    Oid id;
};

constexpr uint32_t modeTree = 0040000;
constexpr uint32_t modeBlob = 0100644;
constexpr uint32_t modeExecutable = 0100755;
constexpr uint32_t modeSymlink = 0120000;
constexpr uint32_t modeCommit = 0160000;

/**
 * A `SourceAccessor` for a tree (or blob) in the tarball cache. It
 * behaves like the libgit2 accessor without its options (export-ignore and
 * Git LFS), which the tarball cache does not use.
 */
struct TarballCacheAccessor final : SourceAccessor
{
private:
    void anchor() override {};

public:
    struct State
    {
        ::rust::Box<rs::TarballCacheReader> reader;
        rs::GitPeeledObject root;

        /**
         * The entries of all trees that were read, by path.
         */
        boost::unordered_flat_map<CanonPath, Entry> entries;

        /**
         * The directories whose entries are in `entries`.
         */
        boost::unordered_flat_set<CanonPath> readDirs;
    };

    Sync<State> state_;

    TarballCacheAccessor(::rust::Box<rs::TarballCacheReader> reader, const Hash & rev)
        : state_{State{.reader = std::move(reader), .root = {}}}
    {
        auto state(state_.lock());
        state->root = callRust([&] { return state->reader->peel_to_tree_or_blob(toOid(rev)); });
    }

    /**
     * The entries of the tree `id` at `path`, which are added to the cache.
     */
    std::vector<std::pair<std::string, Entry>> readTree(State & state, const CanonPath & path, const Oid & id)
    {
        auto treeEntries = callRust([&] { return state.reader->read_tree(id); });
        std::vector<std::pair<std::string, Entry>> res;
        res.reserve(treeEntries.size());
        for (auto & treeEntry : treeEntries) {
            auto name = toString(treeEntry.name);
            Entry entry{.mode = treeEntry.mode, .id = treeEntry.id};
            if (!state.readDirs.contains(path)) {
                auto childPath = path;
                childPath.push(name);
                state.entries.emplace(std::move(childPath), entry);
            }
            res.emplace_back(std::move(name), entry);
        }
        state.readDirs.insert(path);
        return res;
    }

    /**
     * The entry at `path`, which is not the root, or nothing if it does not
     * exist.
     */
    std::optional<Entry> lookup(State & state, const CanonPath & path)
    {
        if (auto i = state.entries.find(path); i != state.entries.end())
            return i->second;

        auto parent = path.parent();
        if (!parent || state.readDirs.contains(*parent))
            return std::nullopt;

        auto parentTree = lookupTree(state, *parent);
        if (!parentTree)
            return std::nullopt;

        readTree(state, *parent, *parentTree);

        if (auto i = state.entries.find(path); i != state.entries.end())
            return i->second;
        return std::nullopt;
    }

    /**
     * The id of the tree at `path`, or nothing if `path` does not exist or
     * is not a tree.
     */
    std::optional<Oid> lookupTree(State & state, const CanonPath & path)
    {
        if (path.isRoot())
            return state.root.is_tree ? std::optional(state.root.id) : std::nullopt;
        auto entry = lookup(state, path);
        if (!entry || entry->mode != modeTree)
            return std::nullopt;
        return entry->id;
    }

    Entry need(State & state, const CanonPath & path)
    {
        auto entry = lookup(state, path);
        if (!entry)
            throw Error("'%s' does not exist", showPath(path));
        return *entry;
    }

    /**
     * The id of the blob at `path`, which must be a regular file, or a
     * symlink if `expectSymlink`.
     */
    Oid getBlob(State & state, const CanonPath & path, bool expectSymlink)
    {
        /* Like the libgit2 accessor: a blob root is the regular file at
           every path. */
        if (!expectSymlink && !state.root.is_tree)
            return state.root.id;

        auto notExpected = [&]() {
            throw Error(expectSymlink ? "'%s' is not a symlink" : "'%s' is not a regular file", showPath(path));
        };

        if (path.isRoot())
            notExpected();

        auto entry = need(state, path);
        if (expectSymlink ? entry.mode != modeSymlink : entry.mode != modeBlob && entry.mode != modeExecutable)
            notExpected();

        return entry.id;
    }

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override
    {
        auto state(state_.lock());
        auto id = getBlob(*state, path, false);
        auto contents = callRust([&] { return state->reader->read_blob(id); });
        sizeCallback(contents.size());
        sink(std::string_view(reinterpret_cast<const char *>(contents.data()), contents.size()));
    }

    bool pathExists(const CanonPath & path) override
    {
        auto state(state_.lock());
        return path.isRoot() ? true : lookup(*state, path).has_value();
    }

    std::optional<Stat> maybeLstat(const CanonPath & path) override
    {
        auto state(state_.lock());

        if (path.isRoot())
            return Stat{.type = state->root.is_tree ? tDirectory : tRegular};

        auto entry = lookup(*state, path);
        if (!entry)
            return std::nullopt;

        switch (entry->mode) {
        case modeTree:
            return Stat{.type = tDirectory};
        case modeBlob:
            return Stat{.type = tRegular};
        case modeExecutable:
            return Stat{.type = tRegular, .isExecutable = true};
        case modeSymlink:
            return Stat{.type = tSymlink};
        case modeCommit:
            /* Treat submodules as an empty directory, like the libgit2
               accessor. The tarball cache has none. */
            return Stat{.type = tDirectory};
        default:
            throw Error("file '%s' has an unsupported Git file type", showPath(path));
        }
    }

    DirEntries readDirectory(const CanonPath & path) override
    {
        auto state(state_.lock());

        if (path.isRoot() && !state->root.is_tree)
            throw Error("Git root object '%s' is not a directory", toHash(state->root.id).gitRev());

        if (!path.isRoot()) {
            auto entry = need(*state, path);
            if (entry.mode == modeCommit)
                return {};
            if (entry.mode != modeTree)
                throw Error("'%s' is not a directory", showPath(path));
        }

        DirEntries res;
        for (auto & [name, entry] : readTree(*state, path, *lookupTree(*state, path))) {
            DirEntry type;
            switch (entry.mode) {
            case modeTree:
            case modeCommit:
                type = tDirectory;
                break;
            case modeBlob:
            case modeExecutable:
                type = tRegular;
                break;
            case modeSymlink:
                type = tSymlink;
                break;
            default:
                break;
            }
            res.emplace(std::move(name), type);
        }
        return res;
    }

    std::string readLink(const CanonPath & path) override
    {
        auto state(state_.lock());
        auto id = getBlob(*state, path, true);
        return toString(callRust([&] { return state->reader->read_blob(id); }));
    }
};

/**
 * The tarball cache, for reading. Each accessor gets its own reader: a
 * gitoxide object handle has caches that are not thread-safe, and the
 * accessor already serialises its calls.
 */
struct TarballCache final : GitRepo, std::enable_shared_from_this<TarballCache>
{
    std::filesystem::path objectsDir;

    Sync<::rust::Box<rs::TarballCacheReader>> reader_;

    TarballCache(const std::filesystem::path & repoDir)
        : objectsDir(repoDir / "objects")
        , reader_(openReader())
    {
    }

    ::rust::Box<rs::TarballCacheReader> openReader() const
    {
        return callRust([&] { return rs::tarball_cache_reader_open(objectsDir.string()); });
    }

    [[noreturn]] static void unsupported(std::string_view what)
    {
        throw UnimplementedError("the tarball cache does not support %s", what);
    }

    uint64_t getLastModified(const Hash & rev) override
    {
        unsupported("commits");
    }

    bool isShallow() override
    {
        return false;
    }

    Hash resolveRef(std::string ref) override
    {
        unsupported("refs");
    }

    void setRemote(const std::string & name, const std::string & url) override
    {
        unsupported("remotes");
    }

    WorkdirInfo getWorkdirInfo() override
    {
        unsupported("a working directory");
    }

    std::optional<std::string> getWorkdirRef() override
    {
        unsupported("a working directory");
    }

    std::vector<std::tuple<Submodule, Hash>> getSubmodules(const Hash & rev, bool exportIgnore) override
    {
        unsupported("submodules");
    }

    std::string resolveSubmoduleUrl(const std::string & url) override
    {
        unsupported("submodules");
    }

    bool hasObject(const Hash & oid) override
    {
        return (*reader_.lock())->has_object(toOid(oid));
    }

    ref<SourceAccessor>
    getAccessor(const Hash & rev, const GitAccessorOptions & options, std::string displayPrefix) override
    {
        if (options.exportIgnore || options.smudgeLfs)
            unsupported("export-ignore and Git LFS");
        auto accessor = make_ref<TarballCacheAccessor>(openReader(), rev);
        accessor->setPathDisplay(std::move(displayPrefix));
        return accessor;
    }

    ref<SourceAccessor> getAccessor(
        const WorkdirInfo & wd, const GitAccessorOptions & options, MakeNotAllowedError makeNotAllowedError) override
    {
        unsupported("a working directory");
    }

    void flush() override {}

    std::unique_ptr<merkle::DirectorySinkWithFinalize> makeDirectorySink() override
    {
        unsupported("writing through the reader; use `Settings::getTarballWriterPool()`");
    }

    std::unique_ptr<merkle::RegularFileSinkWithFinalize> makeRegularFileSink() override
    {
        unsupported("writing through the reader; use `Settings::getTarballWriterPool()`");
    }

    void fetch(const std::string & url, const std::string & refspec, bool shallow) override
    {
        unsupported("fetching");
    }

    void verifyCommit(const Hash & rev, const std::vector<fetchers::PublicKey> & publicKeys) override
    {
        unsupported("commits");
    }

    Hash treeHashToNarHash(const fetchers::Settings & settings, const Hash & treeHash) override
    {
        auto accessor = getAccessor(treeHash, {}, "");

        fetchers::Cache::Key cacheKey{"treeHashToNarHash", {{"treeHash", treeHash.gitRev()}}};

        if (auto res = settings.getCache()->lookup(cacheKey))
            return Hash::parseAny(fetchers::getStrAttr(*res, "narHash"), HashAlgorithm::SHA256);

        auto narHash = accessor->hashPath(CanonPath::root);

        settings.getCache()->upsert(cacheKey, fetchers::Attrs({{"narHash", narHash.to_string(HashFormat::SRI, true)}}));

        return narHash;
    }

    Hash dereferenceSingletonDirectory(const Hash & oid) override
    {
        auto entries = callRust([&] { return (*reader_.lock())->read_tree(toOid(oid)); });
        if (entries.size() == 1 && entries[0].mode == modeTree)
            return toHash(entries[0].id);
        return oid;
    }
};

} // namespace

bool useGixTarballCache()
{
    static const bool useGix = [] {
        auto backend = getEnv("_NIX_TARBALL_CACHE_BACKEND").value_or("gix");
        if (backend == "gix")
            return true;
        if (backend == "libgit2")
            return false;
        throw Error("_NIX_TARBALL_CACHE_BACKEND must be 'gix' or 'libgit2', not '%s'", backend);
    }();
    return useGix;
}

ref<GitRepo> openTarballCache(const std::filesystem::path & repoDir)
{
    return make_ref<TarballCache>(repoDir);
}

ref<GitRepoPool> openTarballWriter(const std::filesystem::path & repoDir)
{
    return make_ref<TarballWriter>(repoDir);
}

} // namespace nix::fetchers::gix

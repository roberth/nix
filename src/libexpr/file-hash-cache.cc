#include "nix/expr/file-hash-cache.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/users.hh"

#include <sys/stat.h>
#include <ctime>

namespace nix {

static const char * fileHashCacheSchema = R"sql(
create table if not exists FileHashes (
    path text primary key,
    mtime integer not null,
    hash text not null
);
)sql";

struct FileHashCache::State
{
    bool opened = false;
    std::filesystem::path dbPath;
    SQLite db;
    SQLiteStmt selectorHash;
    SQLiteStmt insertHash;
    SQLiteStmt deleteHash;
};

FileHashCache::FileHashCache(std::filesystem::path dbPath)
    : _state(std::make_unique<Sync<State>>())
{
    // Defer SQLite open and cache-dir creation until first use. Construction is
    // unconditional (SystemEnvironment owns one), but only the tracing replay
    // path actually calls getHash/lookup. Eagerly creating ~/.cache/nix here
    // would materialise phantom HOME dirs like /homeless-shelter or /fake-home
    // when Nix is invoked with such HOME values (test isolation, build purity).
    auto state(_state->lock());
    state->dbPath = std::move(dbPath);
}

FileHashCache::~FileHashCache() = default;

void FileHashCache::ensureOpen(FileHashCache::State & state)
{
    if (state.opened)
        return;
    state.opened = true;

    auto dbPath = state.dbPath;
    try {
        if (dbPath.empty()) {
            auto cacheDir = std::filesystem::path(getCacheDir());
            createDirs(cacheDir);
            dbPath = cacheDir / "file-hash-cache.sqlite";
        }

        state.db = SQLite(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
        state.db.isCache();
        state.db.exec(fileHashCacheSchema);
    } catch (std::exception & e) {
        // Fall back to in-memory database if the on-disk cache can't be opened
        // (e.g. read-only filesystem, sandboxed builds).
        debug("file hash cache: falling back to in-memory database: %s", e.what());
        state.db = SQLite(":memory:", {.mode = SQLiteOpenMode::Normal});
        state.db.exec(fileHashCacheSchema);
    }

    state.selectorHash.create(state.db, "select mtime, hash from FileHashes where path = ?");
    state.insertHash.create(state.db, "insert or replace into FileHashes(path, mtime, hash) values (?, ?, ?)");
    state.deleteHash.create(state.db, "delete from FileHashes where path = ?");
}

static std::optional<time_t> getMtime(const std::filesystem::path & path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return std::nullopt;
    return st.st_mtime;
}

std::optional<Hash> FileHashCache::lookup(const std::filesystem::path & path)
{
    auto currentMtime = getMtime(path);
    if (!currentMtime)
        return std::nullopt;

    auto state(_state->lock());
    ensureOpen(*state);
    auto query = state->selectorHash.use()(path.string());
    if (!query.next())
        return std::nullopt;

    auto cachedMtime = static_cast<time_t>(query.getInt(0));
    if (cachedMtime != *currentMtime) {
        debug("file hash cache: mtime changed for %s", path.string());
        return std::nullopt;
    }

    auto hashStr = query.getStr(1);
    return Hash::parseAny(hashStr, HashAlgorithm::SHA256);
}

Hash FileHashCache::getHash(const std::filesystem::path & path)
{
    if (auto cached = lookup(path)) {
        debug("file hash cache hit: %s", path.string());
        return *cached;
    }

    debug("file hash cache miss: %s", path.string());

    /* Stat-hash-stat sandwich with the freshness check sampled
       *before* the hash:

       (a) `nowAtStart > mtimeBefore` rules out the same-second
           TOCTOU. POSIX mtime has 1-second granularity, so a
           write within the same wall-clock second as the file's
           current mtime mutates the file invisibly. The check has
           to hold at hash *start* — not after — because a slow
           hash could span into the next second and pass an
           after-the-fact check while a write that happened in
           second `mtimeBefore` *during* the hash leaves
           `mtimeAfter == mtimeBefore` and goes undetected. By
           establishing the freshness before reading the file, we
           know any subsequent write must land in a strictly later
           second and therefore advance mtime, which step (b)
           catches.

       (b) `mtimeBefore == mtimeAfter` rules out a mid-hash write
           that bumped mtime into a new second. Combined with (a)
           this is sufficient: writes in second `mtimeBefore` are
           impossible (the second was already over), and writes in
           any later second would have bumped mtime. */
    auto mtimeBefore = getMtime(path);
    if (!mtimeBefore)
        throw Error("cannot stat file '%s'", path.string());

    auto nowAtStart = ::time(nullptr);

    auto hash = hashFile(HashAlgorithm::SHA256, path);

    auto mtimeAfter = getMtime(path);
    if (!mtimeAfter)
        throw Error("cannot stat file '%s'", path.string());

    if (*mtimeBefore == *mtimeAfter && nowAtStart > *mtimeBefore) {
        auto state(_state->lock());
        ensureOpen(*state);
        state->insertHash.use()(path.string())(static_cast<int64_t>(*mtimeBefore))(
            hash.to_string(HashFormat::SRI, true))
            .exec();
    } else {
        debug(
            "file hash cache: not caching %s (mtimeBefore=%d, mtimeAfter=%d, nowAtStart=%d)",
            path.string(),
            static_cast<int64_t>(*mtimeBefore),
            static_cast<int64_t>(*mtimeAfter),
            static_cast<int64_t>(nowAtStart));
    }

    return hash;
}

void FileHashCache::invalidate(const std::filesystem::path & path)
{
    auto state(_state->lock());
    ensureOpen(*state);
    state->deleteHash.use()(path.string()).exec();
}

} // namespace nix

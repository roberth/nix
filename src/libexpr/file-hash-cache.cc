#include "nix/expr/file-hash-cache.hh"
#include "nix/store/sqlite.hh"
#include "nix/util/file-system.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/util/users.hh"

#include <sys/stat.h>

namespace nix {

static const char * schema = R"sql(
create table if not exists FileHashes (
    path text primary key,
    mtime integer not null,
    hash text not null
);
)sql";

struct FileHashCache::State
{
    SQLite db;
    SQLiteStmt queryHash;
    SQLiteStmt insertHash;
    SQLiteStmt deleteHash;
};

FileHashCache::FileHashCache(std::filesystem::path dbPath)
    : _state(std::make_unique<Sync<State>>())
{
    if (dbPath.empty()) {
        auto cacheDir = std::filesystem::path(getCacheDir());
        createDirs(cacheDir);
        dbPath = cacheDir / "file-hash-cache.sqlite";
    }

    auto state(_state->lock());
    state->db = SQLite(dbPath, {.mode = SQLiteOpenMode::Normal, .useWAL = true});
    state->db.isCache();
    state->db.exec(schema);

    state->queryHash.create(state->db, "select mtime, hash from FileHashes where path = ?");
    state->insertHash.create(state->db, "insert or replace into FileHashes(path, mtime, hash) values (?, ?, ?)");
    state->deleteHash.create(state->db, "delete from FileHashes where path = ?");
}

FileHashCache::~FileHashCache() = default;

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
    auto query = state->queryHash.use()(path.string());
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

    auto hash = hashFile(HashAlgorithm::SHA256, path);
    auto mtime = getMtime(path);
    if (!mtime)
        throw Error("cannot stat file '%s'", path.string());

    auto state(_state->lock());
    state->insertHash.use()(path.string())(static_cast<int64_t>(*mtime))(hash.to_string(HashFormat::SRI, true)).exec();

    return hash;
}

void FileHashCache::invalidate(const std::filesystem::path & path)
{
    auto state(_state->lock());
    state->deleteHash.use()(path.string()).exec();
}

} // namespace nix

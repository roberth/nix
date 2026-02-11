#pragma once

#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <filesystem>
#include <optional>

struct sqlite3;

namespace nix {

struct SQLite;
struct SQLiteStmt;

/**
 * A cache mapping file paths to their SHA-256 content hashes.
 *
 * Uses mtime to detect when a cached entry needs revalidation.
 * Cache is stored in ~/.cache/nix/file-hash-cache.sqlite.
 */
class FileHashCache
{
public:
    FileHashCache();
    ~FileHashCache();

    FileHashCache(const FileHashCache &) = delete;
    FileHashCache & operator=(const FileHashCache &) = delete;

    /**
     * Get the SHA-256 hash of a file's contents.
     *
     * If the file's mtime matches the cached entry, returns the cached hash.
     * Otherwise, computes the hash, updates the cache, and returns the new hash.
     */
    Hash getHash(const std::filesystem::path & path);

    /**
     * Look up a hash without computing it if not cached or stale.
     * Returns nullopt if the cache doesn't have a valid entry.
     */
    std::optional<Hash> lookup(const std::filesystem::path & path);

    /**
     * Remove a specific path from the cache.
     */
    void invalidate(const std::filesystem::path & path);

private:
    struct State;
    std::unique_ptr<Sync<State>> _state;
};

} // namespace nix

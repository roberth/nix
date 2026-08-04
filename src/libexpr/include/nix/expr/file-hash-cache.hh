#pragma once

#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace nix {

/**
 * A cache mapping file paths to their SHA-256 content hashes.
 *
 * Uses mtime to detect when a cached entry needs revalidation.
 * Cache is stored in ~/.cache/nix/file-hash-cache.sqlite.
 *
 * Path arguments take `std::string_view` — the hot warm-replay path
 * feeds paths in as `std::string` from Env-layer `FileReadRequest`
 * payloads, and every conversion to `std::filesystem::path` triggers
 * `_M_split_cmpts` (measured at ~1.4% of warm runtime on network.nix).
 * The cache treats paths as opaque byte strings for lookup equality;
 * path-component semantics aren't needed here.
 */
class FileHashCache
{
public:
    /**
     * @param dbPath Optional path for the SQLite database.
     *               Defaults to ~/.cache/nix/file-hash-cache.sqlite.
     */
    FileHashCache(std::filesystem::path dbPath = {});
    ~FileHashCache();

    FileHashCache(const FileHashCache &) = delete;
    FileHashCache & operator=(const FileHashCache &) = delete;

    /**
     * Get the SHA-256 hash of a file's contents.
     *
     * If the file's mtime matches the cached entry, returns the cached hash.
     * Otherwise, computes the hash, updates the cache, and returns the new hash.
     */
    Hash getHash(std::string_view path);

    /**
     * Look up a hash without computing it if not cached or stale.
     * Returns nullopt if the cache doesn't have a valid entry.
     */
    std::optional<Hash> lookup(std::string_view path);

    /**
     * Remove a specific path from the cache.
     */
    void invalidate(std::string_view path);

private:
    struct State;
    std::unique_ptr<Sync<State>> _state;

    /**
     * Open the SQLite database on first use, deferring any I/O until the
     * cache is actually queried. The constructor must not eagerly create
     * the cache dir, since SystemEnvironment instantiates this for every
     * EvalState and Nix is sometimes invoked with HOME pointing at a
     * directory that must not be auto-created (e.g. /homeless-shelter).
     * Falls back to an in-memory DB on failure.
     */
    static void ensureOpen(State & state);
};

} // namespace nix

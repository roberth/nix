#pragma once
///@file
///
/// The tarball cache on gitoxide. See `src/librust/src/tarball_cache.rs` and
/// `doc/manual/source/development/rust.md`.

#include "nix/fetchers/git-utils.hh"
#include "nix/store/filetransfer.hh"
#include "nix/util/task.hh"

namespace nix::fetchers::gix {

/**
 * Whether the tarball cache uses gitoxide (the default) or libgit2.
 *
 * `_NIX_TARBALL_CACHE_BACKEND=libgit2` selects libgit2. The variable is
 * internal: it exists so that one Nix binary can compare the two
 * implementations, for example in a benchmark.
 */
bool useGixTarballCache();

/**
 * Create the tarball cache if it does not exist, and return the path of
 * its repository.
 */
std::filesystem::path initTarballCache();

/**
 * Open the tarball cache at `repoDir`, an existing bare repository, for
 * reading. Only the operations that the tarball cache needs are
 * implemented; the others throw `UnimplementedError`.
 */
ref<GitRepo> openTarballCache(const std::filesystem::path & repoDir);

/**
 * Open a writer for the tarball cache at `repoDir`, an existing bare
 * repository. All threads share the writer, and `flush()` writes one pack.
 */
ref<GitRepoPool> openTarballWriter(const std::filesystem::path & repoDir);

struct UnpackedTree
{
    /**
     * The root of the tree, as the archive has it: a single top-level
     * directory is not removed.
     */
    Hash treeHash;

    time_t lastModified;
};

/**
 * Unpack an archive, which a reader takes from `stream`, into the tarball
 * cache at `repoDir`, an existing bare repository.
 *
 * The work runs as a Rust future on a thread of its own: parsing with
 * libarchive, writing with gitoxide, and a flush that writes one pack.
 * Destroying the task before it completes cancels that work and waits
 * for its thread, which leaves the cache as it was. It also closes
 * `stream`, so that a blocked read returns.
 *
 * If reading `stream` fails, the task fails with the exception of the
 * stream, for example the error of the download.
 */
Task<UnpackedTree> unpackIntoTarballCache(std::filesystem::path repoDir, ref<TransferStream> stream, bool isZip);

/**
 * A function that a write to the tarball cache calls when it reaches a
 * phase, on the thread of the write. `isCancelled` tells whether the write
 * is cancelled; the write checks it again right after the call. Phases:
 *
 * - `unpack`: after the first file of an archive.
 * - `flush`: the pack is on disk under a temporary name, and its index is
 *   not.
 */
using TarballCachePhaseHook = std::function<void(std::string_view phase, const std::function<bool()> & isCancelled)>;

/**
 * Set the phase hook, or remove it with an empty function. For tests: a
 * hook that blocks until the write is cancelled holds the write still in a
 * phase, so that a test can interrupt it there. Not thread-safe: set it
 * while no write runs.
 */
void setTarballCachePhaseHook(TarballCachePhaseHook hook);

} // namespace nix::fetchers::gix

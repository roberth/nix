#pragma once
///@file
///
/// The tarball cache on gitoxide. See `src/librust/src/tarball_cache.rs` and
/// `doc/manual/source/development/rust.md`.

#include "nix/fetchers/git-utils.hh"

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

} // namespace nix::fetchers::gix

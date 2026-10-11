#pragma once
///@file
///
/// Upward interface of the gitoxide tarball cache (`src/tarball_cache.rs`).
/// See `boundary.hh` for why Rust calls up only through abstract classes.

#include "nix/rust/boundary.hh"
#include "nix/rust/future-abi.hh"
#include "rust/cxx.h"

namespace nix::rs {

/* Defined by the bridge in `tarball_cache.rs.h`. */
struct TarballCacheWriter;
struct CancelToken;
struct TarballUnpackResult;

/**
 * The services of the C++ libraries that the tarball cache uses while it
 * writes a pack.
 *
 * Cancellation is not one of them: the owner of a write cancels it with a
 * `CancelToken`, which the writer checks. A global interrupt check here
 * would stop all writes in the process at once, and could not stop one
 * write whose owner gave up on it.
 */
class TarballCacheHooks
{
public:
    virtual ~TarballCacheHooks();

    /**
     * Log a message at the debug verbosity. (Not `debug`: that is a macro
     * in `nix/util/logging.hh`.)
     */
    virtual void logDebug(::rust::Str message) const = 0;

    /**
     * A write reached `phase`, on the thread of the write. Phases:
     *
     * - `flush`: the pack is on disk under a temporary name, and its index
     *   is not.
     *
     * The write checks `cancel` right after the call. A test can block
     * here until `cancel` is set, to interrupt the write in this phase.
     *
     * Both functions may throw. The exception stops the write, and the
     * caller of the write gets it unchanged.
     */
    virtual void reachedPhase(::rust::Str phase, const CancelToken & cancel) const = 0;
};

/**
 * Reads an archive and writes its contents into the tarball cache, for the
 * future of `tarball_cache_unpack()`.
 *
 * The future calls `run()` on a thread of its own, and may call
 * `cancel()` on another thread at the same time. An implementation must
 * therefore be thread-safe for these two calls.
 */
class TarballUnpacker
{
public:
    virtual ~TarballUnpacker();

    /**
     * Parse the archive and write its files and directories with `writer`,
     * without a flush. Store the root of the tree in `out`. Return null,
     * or the exception that stopped the parse. Check `cancel` while
     * waiting for data and between entries, and stop with an exception
     * when it is set.
     */
    virtual nix_exception *
    run(const TarballCacheWriter & writer, const CancelToken & cancel, TarballUnpackResult & out) const noexcept = 0;

    /**
     * Wake `run()` if it waits for data, so that it sees the cancellation.
     */
    virtual void cancel() const noexcept = 0;

    virtual const TarballCacheHooks & hooks() const noexcept = 0;
};

} // namespace nix::rs

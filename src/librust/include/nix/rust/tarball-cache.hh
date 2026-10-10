#pragma once
///@file
///
/// Upward interface of the gitoxide tarball cache (`src/tarball_cache.rs`).
/// See `boundary.hh` for why Rust calls up only through abstract classes.

#include "rust/cxx.h"

namespace nix::rs {

/**
 * The services of the C++ libraries that the tarball cache uses while it
 * writes a pack.
 */
class TarballCacheHooks
{
public:
    virtual ~TarballCacheHooks();

    /**
     * Throw if the user interrupted Nix. The exception arrives in Rust as an
     * `Err`, and the Rust code stops and cleans up. The C++ caller then
     * calls `checkInterrupt()` again to throw the original exception type.
     */
    virtual void checkInterrupt() const = 0;

    /**
     * Log a message at the debug verbosity. (Not `debug`: that is a macro
     * in `nix/util/logging.hh`.)
     */
    virtual void logDebug(::rust::Str message) const = 0;
};

} // namespace nix::rs

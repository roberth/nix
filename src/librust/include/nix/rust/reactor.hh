#pragma once
///@file
///
/// Upward interface of the reactor (`src/reactor.rs`): the callbacks that
/// the reactor thread calls. See `boundary.hh` for why Rust calls up only
/// through abstract classes. The libutil header `nix/util/reactor.hh` wraps
/// them for C++ code.

#include "nix/rust/future-abi.hh"
#include "rust/cxx.h"

namespace nix::rs {

/**
 * Work that the reactor runs on its thread, once (`reactor_post`) or each
 * time a timer expires (`reactor_timer`).
 */
class ReactorCallback
{
public:
    virtual ~ReactorCallback();

    virtual void run() const noexcept = 0;
};

/**
 * Receives the readiness of a file descriptor (`reactor_watch_fd`), on the
 * reactor thread.
 */
class FdCallback
{
public:
    virtual ~FdCallback();

    /**
     * The descriptor is ready for the directions that are true. An error
     * or a hangup is reported as readiness in each direction of the
     * interest, so that the next read or write sees it.
     */
    virtual void ready(bool readable, bool writable) const noexcept = 0;
};

} // namespace nix::rs

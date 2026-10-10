#pragma once
///@file
///
/// The C++ side of the test probes in `src/future_probe.rs`.

#include "nix/rust/future-abi.hh"
#include "rust/cxx.h"

namespace nix::rs {

/**
 * An event log that a test owns and that probe futures write to.
 */
class ProbeLog
{
public:
    virtual ~ProbeLog();

    virtual void push(::rust::Str event) const = 0;
};

} // namespace nix::rs

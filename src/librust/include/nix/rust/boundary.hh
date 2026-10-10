#pragma once
///@file
///
/// Interfaces through which Rust code in `libnixrust` calls up into the C++
/// libraries that are linked above it.
///
/// `libnixrust` is at the bottom of the library graph, so it must not refer
/// to symbols of `libnixutil` or of any library above it. Rust code therefore
/// calls upward only through virtual functions of the abstract classes in
/// this file. The libraries above implement them, and the dispatch happens
/// through the vtable at run time, not through a link-time reference.
///
/// The namespace is `nix::rs`, not `nix::rust`: a `nix::rust` namespace
/// would hide cxx's `::rust` namespace (`rust::String`, `rust::Box`) from
/// all code inside `namespace nix`.

#include "rust/cxx.h"

namespace nix::rs {

/**
 * A function implemented in C++ that Rust code calls.
 *
 * An exception from `call()` arrives in Rust as an `Err` that holds the
 * `what()` string of the exception.
 */
class Callback
{
public:
    virtual ~Callback();

    virtual ::rust::String call() const = 0;
};

} // namespace nix::rs

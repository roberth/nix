#pragma once
///@file
///
/// The rules for the interfaces through which Rust code in `libnixrust`
/// calls up into the C++ libraries that are linked above it.
///
/// `libnixrust` is at the bottom of the library graph, so it must not refer
/// to symbols of `libnixutil` or of any library above it. Rust code therefore
/// calls upward only through virtual functions of abstract classes that
/// `libnixrust` declares, for example `TarballCacheHooks` in
/// `nix/rust/tarball-cache.hh`. The libraries above implement them, and the
/// dispatch happens through the vtable at run time, not through a link-time
/// reference.
///
/// Each such class has an out-of-line destructor in `src/boundary.cc`, its
/// key function, so that its vtable and type information are strong
/// symbols of `libnixrust`.
///
/// The namespace is `nix::rs`, not `nix::rust`: a `nix::rust` namespace
/// would hide cxx's `::rust` namespace (`rust::String`, `rust::Box`) from
/// all code inside `namespace nix`.

#include "rust/cxx.h"

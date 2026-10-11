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
/// An upward call that may throw is declared with `Result` in the bridge.
/// cxx then catches the exception, through `rust::behavior::trycatch`
/// below, and gives Rust only its message. The `trycatch` here also keeps
/// the exception itself on the thread, and `From<cxx::Exception>` for
/// `nix_rust_future::Error` takes it back, so that the exception keeps its
/// type and identity through Rust.
///
/// The namespace is `nix::rs`, not `nix::rust`: a `nix::rust` namespace
/// would hide cxx's `::rust` namespace (`rust::String`, `rust::Box`) from
/// all code inside `namespace nix`.

#include "rust/cxx.h"

#include <string>

namespace nix::rs {

/**
 * Keep the exception that is being handled on this thread, for
 * `nix_exception_take_kept`, and return its `what()`. Call it in a `catch`
 * block.
 */
std::string keepCurrentException() noexcept;

} // namespace nix::rs

/* cxx calls this function, instead of its own, around each `extern "C++"`
   function with `Result`, if a header of the bridge declares it. Unlike the
   default, it also catches exceptions that are not a `std::exception`. */
namespace rust::behavior {

template<typename Try, typename Fail>
static void trycatch(Try && func, Fail && fail) noexcept
{
    try {
        func();
    } catch (...) {
        fail(::nix::rs::keepCurrentException());
    }
}

} // namespace rust::behavior

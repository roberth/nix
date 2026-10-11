#include "nix/rust/boundary.hh"
#include "nix/rust/reactor.hh"
#include "nix/rust/tarball-cache.hh"

namespace nix::rs {

/* The out-of-line destructors are the key functions of the classes. They make
   the vtables and the type information strong symbols of libnixrust, so that
   the libraries above it share one copy. See `-Werror=weak-vtables` in
   `nix-meson-build-support/common/meson.build`. */
TarballCacheHooks::~TarballCacheHooks() = default;

TarballUnpacker::~TarballUnpacker() = default;

ReactorCallback::~ReactorCallback() = default;

FdCallback::~FdCallback() = default;

SlowPollHook::~SlowPollHook() = default;

} // namespace nix::rs

#include "nix/rust/boundary.hh"
#include "nix/rust/tarball-cache.hh"

namespace nix::rs {

/* The out-of-line destructors are the key functions of the classes. They make
   the vtables and the type information strong symbols of libnixrust, so that
   the libraries above it share one copy. See `-Werror=weak-vtables` in
   `nix-meson-build-support/common/meson.build`. */
Callback::~Callback() = default;

TarballCacheHooks::~TarballCacheHooks() = default;

} // namespace nix::rs

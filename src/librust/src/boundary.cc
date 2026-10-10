#include "nix/rust/boundary.hh"

namespace nix::rs {

/* The out-of-line destructor is the key function of the class. It makes the
   vtable and the type information strong symbols of libnixrust, so that the
   libraries above it share one copy. See `-Werror=weak-vtables` in
   `nix-meson-build-support/common/meson.build`. */
Callback::~Callback() = default;

} // namespace nix::rs

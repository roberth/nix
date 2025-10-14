#include "nix/expr/environment.hh"

namespace nix {

/* Out-of-line virtual destructor so the abstract base gets a key
   function — without it, clang's `-Wweak-vtables` reports the vtable
   as emitted in every TU. */
Environment::~Environment() = default;

} // namespace nix

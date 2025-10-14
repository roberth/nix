#include "nix/expr/environment.hh"
#include "nix/expr/environment/system.hh"

namespace nix {

/* Out-of-line virtual destructor so the abstract base gets a key
   function — without it, clang's `-Wweak-vtables` reports the vtable
   as emitted in every TU. */
Environment::~Environment() = default;

ref<Environment>
makeSystemEnvironment(const EvalSettings & settings, ref<Store> store, std::shared_ptr<Store> buildStore)
{
    return make_ref<SystemEnvironment>(settings, store, buildStore);
}

} // namespace nix

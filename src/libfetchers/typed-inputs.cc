#include "nix/fetchers/typed-inputs.hh"
#include "nix/fetchers/fetch-settings.hh"

namespace nix::fetchers {

std::string InputBase::getName() const
{
    // Default implementation: use the type name
    // Specific input types can override this for better names
    return type;
}

} // namespace nix::fetchers

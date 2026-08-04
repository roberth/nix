#include "nix/expr/arg-cell.hh"

namespace nix {

/* Out-of-line key functions so each class's vtable lands in one TU
   (-Werror=weak-vtables). */

ArgCell::~ArgCell() = default;

const CallbackState * RegularArgCell::getCallbackState() const
{
    return nullptr;
}

const CallbackState * CallbackArgCell::getCallbackState() const
{
    return &callbackState;
}

} // namespace nix

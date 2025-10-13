#include "nix/expr/interpreter.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/eval-settings.hh"

namespace nix {

/* Out-of-line virtual destructors so the abstract bases get a key
   function — without these, clang's `-Wweak-vtables` reports the
   vtable as emitted in every TU that includes the header. */
Object::~Object() = default;
Evaluator::~Evaluator() = default;

Interpreter::Interpreter(ref<EvalState> evalState)
    : evalState(evalState)
{
}

bool Interpreter::isReadOnly() const
{
    return evalState->settings.readOnlyMode;
}

Store & Interpreter::getStore()
{
    return *evalState->store;
}

const fetchers::Settings & Interpreter::getFetchSettings()
{
    return evalState->fetchSettings;
}

} // namespace nix
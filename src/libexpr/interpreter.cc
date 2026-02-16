#include "nix/expr/interpreter.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/environment/system.hh"

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
    return *evalState->systemEnvironment->store;
}

const fetchers::Settings & Interpreter::getFetchSettings()
{
    return evalState->fetchSettings;
}

ref<Object> Interpreter::evalFile(const RootedPath & path, const std::string & displayPath)
{
    auto v = evalState->allocValue();
    evalState->evalFile(path, *v);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    auto v = evalState->allocValue();
    auto e = evalState->parseExprFromString(expr, basePath);
    evalState->eval(e, *v);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

} // namespace nix
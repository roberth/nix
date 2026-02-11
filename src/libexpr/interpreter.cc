#include "nix/expr/interpreter.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/environment/system.hh"

namespace nix {

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

ref<Object> Interpreter::evalFile(const SourcePath & path, const std::string & displayPath)
{
    auto v = evalState->allocValue();
    evalState->evalFile(path, *v);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    auto v = evalState->allocValue();
    auto e = evalState->parseExprFromString(expr, basePath);
    evalState->eval(e, *v);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

EvalState * Interpreter::getEvalState()
{
    return &*evalState;
}

} // namespace nix
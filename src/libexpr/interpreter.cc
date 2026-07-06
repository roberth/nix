#include "nix/expr/interpreter.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/expr-from-object.hh"
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

ref<Object> Interpreter::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    auto v = evalState->allocValue();
    auto e = evalState->parseExprFromString(expr, basePath);
    evalState->mkThunk_(*v, e);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::mkString(const std::string & s)
{
    auto v = evalState->allocValue();
    v->mkString(s, evalState->mem);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::mkInt(NixInt i)
{
    auto v = evalState->allocValue();
    v->mkInt(i);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::mkBool(bool b)
{
    auto v = evalState->allocValue();
    v->mkBool(b);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::mkPath(const RootedPath & path)
{
    auto v = evalState->allocValue();
    v->mkPath(path, evalState->mem);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::getInternalPrimOp(const std::string & name)
{
    auto it = evalState->internalPrimOps.find(name);
    if (it == evalState->internalPrimOps.end())
        throw Error("no internal primop named '%s'", name);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(it->second));
}

ref<Object> Interpreter::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    auto v = evalState->allocValue();
    auto bindings = evalState->buildBindings(attrs.size());
    for (const auto & [name, obj] : attrs) {
        // TODO: make lazy
        auto attrValue = obj->defeatCache();
        bindings.insert(evalState->symbols.create(name), *attrValue);
    }
    v->mkAttrs(bindings.finish());
    return make_ref<InterpreterObject>(*evalState, allocRootValue(v));
}

ref<Object> Interpreter::apply(ref<Object> fn, ref<Object> arg)
{
    /* `toValueOrProxy` is the right method here: for concrete Objects
       it returns the underlying forced Value (= same as `defeatCache`);
       for `OuterObject` it returns a thunk wrapping an `ExprFromObject`
       proxy that defers materialisation. This replaces the old try/catch
       defeatCache pattern — `defeatCache` was the wrong name for the
       virtual-value case, since OuterObjects can't be "defeated"
       (they ARE the cache). */
    auto fnValue = fn->toValueOrProxy(*evalState, outerResolver);
    auto argValue = arg->toValueOrProxy(*evalState, outerResolver);

    auto result = evalState->allocValue();
    // Create a lazy application thunk - evaluation happens when forced
    result->mkApp(*fnValue, *argValue);
    return make_ref<InterpreterObject>(*evalState, allocRootValue(result));
}

EvalState & Interpreter::getEvalState()
{
    return *evalState;
}

} // namespace nix
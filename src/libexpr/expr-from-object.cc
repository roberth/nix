#include "nix/expr/expr-from-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"

namespace nix {

// TODO: share with tracing-object.cc, tracing-replay-object.cc, ambient-object.cc
static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs: return "set";
    case nList: return "list";
    case nString: return "string";
    case nPath: return "path";
    case nInt: return "int";
    case nFloat: return "float";
    case nBool: return "bool";
    case nNull: return "null";
    case nFunction: return "lambda";
    case nExternal: return "external";
    case nThunk: return "thunk";
    case nFailed: return "failed";
    }
    return "unknown";
}

/**
 * Stateful resolver that maps virtual value ids to outer Objects.
 * Resolves contra-queries by dispatching to the appropriate Object method,
 * registering child Objects under structurally derived ids.
 */
struct AmbientResolver
{
    std::map<std::string, std::shared_ptr<Object>> objects;
    std::map<std::string, std::shared_ptr<Object>> localObjects; // inner values passed to callbacks
    std::map<std::string, Value *> bridgedLocals; // local id → outer Value (cached for reuse)
    EvalState * outerState = nullptr; // for bridging local values to outer heap
    std::shared_ptr<Evaluator> innerEvaluator; // for bridging local values with function support
    uint64_t nextLocalId = 0;

    explicit AmbientResolver(std::string rootId, std::shared_ptr<Object> rootObj)
    {
        objects[rootId] = std::move(rootObj);
    }

    std::shared_ptr<Object> resolve(const std::string & id)
    {
        auto it = objects.find(id);
        if (it != objects.end())
            return it->second;
        auto lit = localObjects.find(id);
        if (lit != localObjects.end())
            return lit->second;
        throw Error("ambient query: unknown value id '%s'", id);
    }

    trace::ResultVariant query(const trace::QueryVariant & q)
    {
        return std::visit(
            [&](const auto & query) -> trace::ResultVariant {
                using Q = std::decay_t<decltype(query)>;
                if constexpr (std::is_same_v<Q, trace::QueryApply>) {
                    // Covariant callback: call ambient function with local value.
                    if (!outerState)
                        throw Error("ambient apply requires outerState");
                    auto fnObj = resolve(query.fn);
                    auto argIt = localObjects.find(query.arg);
                    if (argIt == localObjects.end())
                        throw Error("ambient apply: unknown local value '%s'", query.arg);

                    // Bridge local arg as an AmbientObject backed by the resolver.
                    // Attribute accesses route back through the resolver to the
                    // inner Object, avoiding eager forcing of self-referential values.
                    // Reuse bridged values for fixed-point cycle detection.
                    auto fnVal = fnObj->defeatCache();
                    auto & argThunk = bridgedLocals[query.arg];
                    if (!argThunk) {
                        // Bridge the local value directly via ExprFromObject
                        // with the inner evaluator. This ensures functions within
                        // the local value route through the inner evaluator,
                        // not through the ambient bridge.
                        argThunk = outerState->allocValue();
                        auto * argExpr = new ExprFromObject(argIt->second, innerEvaluator);
                        outerState->mkThunk_(*argThunk, argExpr);
                    }

                    // Call the function eagerly. The argument stays lazy (bridged
                    // via AmbientObject/ExprFromObject thunk) so the fixed-point's
                    // self-reference uses the same outer Value pointer, enabling
                    // the Nix evaluator's cycle detection.
                    auto * resultVal = outerState->allocValue();
                    outerState->callFunction(**fnVal, *argThunk, *resultVal, noPos);
                    auto resultObj = std::make_shared<InterpreterObject>(*outerState, allocRootValue(resultVal));
                    auto resultId = query.fn + ".apply(" + query.arg + ")";
                    objects[resultId] = resultObj;
                    return trace::ResultType{objectTypeToString(resultObj->getType())};
                } else if constexpr (!requires { query.from; }) {
                    throw Error("ambient query: query type has no 'from' field");
                } else {
                    auto obj = resolve(query.from);

                    if constexpr (std::is_same_v<Q, trace::QueryGetType>) {
                        return trace::ResultType{objectTypeToString(obj->getType())};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return trace::ResultMaybeType{std::nullopt};
                        // Detect self-references by comparing underlying Value
                        // pointers. Without this, self.buildPackages = self
                        // creates infinite ExprFromObject expansions.
                        std::string childId = query.from + "." + query.name;
                        try {
                            auto childVal = child->defeatCache();
                            auto findByValue = [&](auto & map) {
                                for (auto & [id, existing] : map) {
                                    try {
                                        auto existingVal = existing->defeatCache();
                                        if (*childVal == *existingVal) {
                                            childId = id;
                                            child = existing;
                                            return true;
                                        }
                                    } catch (...) {}
                                }
                                return false;
                            };
                            if (!findByValue(objects))
                                findByValue(localObjects);
                        } catch (...) {}
                        objects[childId] = child;
                        return trace::ResultMaybeType{
                            std::optional<std::string>{objectTypeToString(child->getType())}};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetString>) {
                        return trace::ResultString{obj->getStringIgnoreContext()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetStringWithContext>) {
                        auto [str, ctx] = obj->getStringWithContext();
                        std::vector<std::string> ctxStrings;
                        for (auto & c : ctx)
                            ctxStrings.push_back(c.to_string());
                        return trace::ResultStringWithContext{str, std::move(ctxStrings)};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttrNames>) {
                        return trace::ResultListOfStrings{obj->getAttrNames()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetBool>) {
                        return trace::ResultBool{obj->getBool()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetInt>) {
                        return trace::ResultInt{obj->getInt().value};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetFloat>) {
                        return trace::ResultFloat{obj->getFloat()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListSize>) {
                        return trace::ResultListSize{obj->getListSize()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListElem>) {
                        auto child = obj->getListElem(query.index);
                        auto childId = query.from + "[" + std::to_string(query.index) + "]";
                        objects[childId] = child;
                        return trace::ResultType{objectTypeToString(child->getType())};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetPath>) {
                        return trace::ResultPath{obj->getPath().path.abs()};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetFunctionInfo>) {
                        auto info = obj->getFunctionInfo();
                        if (!info)
                            return trace::ResultFunctionInfo{false, {}, false};
                        return trace::ResultFunctionInfo{true, info->formals, info->ellipsis};
                    } else {
                        throw Error("unsupported ambient query type");
                    }
                }
            },
            q);
    }
};

void ExprFromObject::eval(EvalState & state, Env & env, Value & v)
{
    auto type = obj->getType();

    switch (type) {
    case nAttrs: {
        auto names = obj->getAttrNames();
        auto attrs = state.buildBindings(names.size());
        for (const auto & name : names) {
            auto * thunk = state.allocValue();
            auto * expr = new ExprFromObjectAttr(obj, name, innerEvaluator);
            state.mkThunk_(*thunk, expr);
            attrs.insert(state.symbols.create(name), thunk);
        }
        v.mkAttrs(attrs);
        break;
    }

    case nList: {
        auto size = obj->getListSize();
        auto builder = state.buildList(size);
        for (size_t i = 0; i < size; i++) {
            auto childObj = obj->getListElem(i);
            auto childExpr = new ExprFromObject(std::move(childObj), innerEvaluator);
            builder.elems[i] = childExpr->maybeThunk(state, env);
        }
        v.mkList(builder);
        break;
    }

    case nString: {
        auto [str, ctx] = obj->getStringWithContext();
        v.mkString(str, ctx, state.mem);
        break;
    }

    case nPath: {
        auto path = obj->getPath();
        v.mkPath(path, state.mem);
        break;
    }

    case nInt: {
        auto i = obj->getInt();
        v.mkInt(i);
        break;
    }

    case nFloat: {
        auto f = obj->getFloat();
        v.mkFloat(f);
        break;
    }

    case nBool: {
        auto b = obj->getBool();
        v.mkBool(b);
        break;
    }

    case nNull: {
        v.mkNull();
        break;
    }

    case nFunction: {
        auto objPtr = obj;
        auto innerEval = innerEvaluator;
        auto * primOp = new
#if NIX_USE_BOEHMGC
            (GC)
#endif
                PrimOp{
                    .name = "<cached-fn>",
                    .args = {"args"},
                    .arity = 1,
                    .impl =
                        [objPtr, innerEval](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                            if (!innerEval) {
                                // No inner evaluator — this is an ambient function.
                                // Issue an ambient QueryApply through the AmbientObject's queryFn.
                                auto * ambient = dynamic_cast<AmbientObject *>(&*objPtr);
                                if (!ambient)
                                    state.error<TypeError>("cached function call without inner evaluator or ambient object").atPos(pos).debugThrow();

                                // Register the outer argument as a local value.
                                // Do NOT force it — it may be self-referential.
                                auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                                auto argId = "local:" + std::to_string(reinterpret_cast<uintptr_t>(args[0]));

                                // Issue the apply through the ambient query mechanism
                                auto result = ambient->queryApply(argId, argObj);

                                // Bridge result back
                                ExprFromObject(result).eval(state, state.baseEnv, v);
                                return;
                            }

                            // Wrap the outer argument as an AmbientObject.
                            // Route queries through the inner Environment so
                            // the TracingEnvironment records them in the trie.
                            state.forceValue(*args[0], pos);
                            std::shared_ptr<Object> outerArgObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                            auto resolver = std::make_shared<AmbientResolver>("0", outerArgObj);
                            resolver->outerState = &state;
                            resolver->innerEvaluator = innerEval;
                            auto & innerEnv = *innerEval->getEvalState().environment;
                            AmbientQueryFn queryFn = [resolver, &innerEnv](const trace::QueryVariant & q) {
                                return innerEnv.ambientQuery(
                                    q, [&resolver](const trace::QueryVariant & q2) { return resolver->query(q2); });
                            };
                            AmbientRegisterLocalFn registerLocal = [resolver](std::shared_ptr<Object> obj) {
                                // Deduplicate: same Object pointer → same local id.
                                // Essential for fixed-point combinators.
                                for (auto & [id, existing] : resolver->localObjects)
                                    if (existing == obj)
                                        return id;
                                auto localId = "L" + std::to_string(resolver->nextLocalId++);
                                resolver->localObjects[localId] = std::move(obj);
                                return localId;
                            };
                            auto contraArg = make_ref<AmbientObject>("0", std::move(queryFn), std::move(registerLocal));

                            // Apply the cached function to the contra argument
                            auto result = innerEval->apply(ref<Object>(objPtr), contraArg);

                            // Bridge result back to outer evaluator
                            ExprFromObject(result.get_ptr(), innerEval).eval(state, state.baseEnv, v);
                        },
                    .getFunctionInfo = [objPtr]() -> std::optional<FunctionInfo> { return objPtr->getFunctionInfo(); },
                };
        v.mkPrimOp(primOp);
        break;
    }

    case nExternal:
    case nThunk:
    case nFailed:
        state.error<TypeError>("ExprFromObject: cannot represent type %s", showType(type)).debugThrow();
        break;
    }
}

void ExprFromObjectAttr::eval(EvalState & state, Env & env, Value & v)
{
    auto childObj = parentObj->maybeGetAttr(name);
    if (!childObj)
        state.error<TypeError>("ExprFromObjectAttr: attribute '%s' missing", name).debugThrow();
    ExprFromObject(std::move(childObj), innerEvaluator).eval(state, env, v);
}

} // namespace nix

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
struct AmbientResolver : std::enable_shared_from_this<AmbientResolver>
{
    std::map<int, std::shared_ptr<Object>> outerValues; // outer values (from ambient evaluator)
    std::map<int, std::shared_ptr<Object>> localValues; // inner values (passed to callbacks)
    std::map<int, Value *> bridgedLocals;               // local id → outer Value (cached for reuse)
    EvalState * outerState = nullptr;
    std::shared_ptr<Evaluator> innerEvaluator;
    int nextId = 0;

    int registerOuter(std::shared_ptr<Object> obj)
    {
        auto id = nextId++;
        outerValues[id] = std::move(obj);
        return id;
    }

    int registerLocal(std::shared_ptr<Object> obj)
    {
        auto id = nextId++;
        localValues[id] = std::move(obj);
        return id;
    }

    std::shared_ptr<Object> resolve(int id)
    {
        auto it = outerValues.find(id);
        if (it != outerValues.end())
            return it->second;
        auto lit = localValues.find(id);
        if (lit != localValues.end())
            return lit->second;
        throw Error("ambient query: unknown value id %d", id);
    }

    AmbientQueryResult query(int objectId, const trace::QueryVariant & q)
    {
        return std::visit(
            [&](const auto & query) -> AmbientQueryResult {
                using Q = std::decay_t<decltype(query)>;
                if constexpr (std::is_same_v<Q, trace::QueryApply>) {
                    throw Error("ambient query: QueryApply should go through applyFn, not queryFn");
                } else if constexpr (!requires { query.from; }) {
                    throw Error("ambient query: query type has no 'from' field");
                } else {
                    auto obj = resolve(objectId);

                    if constexpr (std::is_same_v<Q, trace::QueryGetType>) {
                        return {trace::ResultType{objectTypeToString(obj->getType())}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return {trace::ResultMaybeType{std::nullopt}, std::nullopt};
                        auto childId = registerOuter(child);
                        return {trace::ResultMaybeType{std::optional<std::string>{objectTypeToString(child->getType())}}, childId};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetString>) {
                        return {trace::ResultString{obj->getStringIgnoreContext()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetStringWithContext>) {
                        auto [str, ctx] = obj->getStringWithContext();
                        std::vector<std::string> ctxStrings;
                        for (auto & c : ctx)
                            ctxStrings.push_back(c.to_string());
                        return {trace::ResultStringWithContext{str, std::move(ctxStrings)}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttrNames>) {
                        return {trace::ResultListOfStrings{obj->getAttrNames()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetBool>) {
                        return {trace::ResultBool{obj->getBool()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetInt>) {
                        return {trace::ResultInt{obj->getInt().value}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetFloat>) {
                        return {trace::ResultFloat{obj->getFloat()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListSize>) {
                        return {trace::ResultListSize{obj->getListSize()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListElem>) {
                        auto child = obj->getListElem(query.index);
                        auto childId = registerOuter(child);
                        return {trace::ResultType{objectTypeToString(child->getType())}, childId};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetPath>) {
                        return {trace::ResultPath{obj->getPath().path.abs()}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetFunctionInfo>) {
                        auto info = obj->getFunctionInfo();
                        if (!info)
                            return {trace::ResultFunctionInfo{false, {}, false}, std::nullopt};
                        return {trace::ResultFunctionInfo{true, info->formals, info->ellipsis}, std::nullopt};
                    } else {
                        throw Error("unsupported ambient query type");
                    }
                }
            },
            q);
    }

    int apply(int fnId, std::shared_ptr<Object> argObj)
    {
        if (!outerState)
            throw Error("ambient apply requires outerState");
        auto fnObj = resolve(fnId);

        // Register the arg as a local value
        auto argId = registerLocal(argObj);

        // Bridge local arg via ExprFromObject with the inner evaluator
        auto & argThunk = bridgedLocals[argId];
        if (!argThunk) {
            argThunk = outerState->allocValue();
            auto * argExpr = new ExprFromObject(argObj, innerEvaluator, shared_from_this());
            outerState->mkThunk_(*argThunk, argExpr);
        }

        // Create lazy application
        auto fnVal = fnObj->defeatCache();
        auto * resultVal = outerState->allocValue();
        resultVal->mkApp(*fnVal, argThunk);
        auto resultObj = std::make_shared<InterpreterObject>(*outerState, allocRootValue(resultVal));
        return registerOuter(resultObj);
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
            auto * expr = new ExprFromObjectAttr(obj, name, innerEvaluator, ambientResolver);
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
            auto childExpr = new ExprFromObject(std::move(childObj), innerEvaluator, ambientResolver);
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
                        [objPtr, innerEval, resolver = this->ambientResolver](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                            if (!innerEval) {
                                // No inner evaluator — this is an ambient function.
                                // Issue an ambient QueryApply through the AmbientObject's queryFn.
                                auto * ambient = dynamic_cast<AmbientObject *>(&*objPtr);
                                if (!ambient)
                                    state.error<TypeError>("cached function call without inner evaluator or ambient object").atPos(pos).debugThrow();

                                // Register the outer argument as a local value.
                                // Do NOT force it — it may be self-referential.
                                auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));

                                // Issue the apply through the ambient query mechanism
                                auto result = ambient->queryApply(std::move(argObj));

                                // Bridge result back, propagating the resolver
                                ExprFromObject(result, nullptr, resolver).eval(state, state.baseEnv, v);
                                return;
                            }

                            // Use the shared resolver from the builtins.cache call.
                            // Do NOT force args[0] — it may be self-referential.
                            auto & res = resolver;
                            if (!res) {
                                state.error<TypeError>("cached function call: no ambient resolver").atPos(pos).debugThrow();
                            }
                            std::shared_ptr<Object> outerArgObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                            auto rootId = res->registerOuter(outerArgObj);
                            auto & innerEnv = *innerEval->getEvalState().environment;
                            AmbientQueryFn queryFn = [res, &innerEnv](int objectId, const trace::QueryVariant & q) {
                                auto qr = res->query(objectId, q);
                                // Record the ambient interaction in the inner trace
                                innerEnv.ambientQuery(q, [&](const trace::QueryVariant &) { return qr.result; });
                                return qr;
                            };
                            AmbientApplyFn applyFn = [res, &innerEnv](int fnId, std::shared_ptr<Object> argObj) {
                                auto resultId = res->apply(fnId, std::move(argObj));
                                // Record the apply as an ambient interaction
                                trace::QueryApply applyQuery{std::to_string(fnId), std::to_string(resultId)};
                                innerEnv.ambientQuery(applyQuery, [&](const trace::QueryVariant &) -> trace::ResultVariant {
                                    return trace::ResultType{"apply"};
                                });
                                return resultId;
                            };
                            auto contraArg = make_ref<AmbientObject>(rootId, std::move(queryFn), std::move(applyFn));

                            // Apply the cached function to the contra argument
                            auto result = innerEval->apply(ref<Object>(objPtr), contraArg);

                            // Bridge result back to outer evaluator
                            ExprFromObject(result.get_ptr(), innerEval, resolver).eval(state, state.baseEnv, v);
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
    ExprFromObject(std::move(childObj), innerEvaluator, ambientResolver).eval(state, env, v);
}

std::shared_ptr<AmbientResolver> makeAmbientResolver(EvalState * outerState, std::shared_ptr<Evaluator> innerEvaluator)
{
    auto resolver = std::make_shared<AmbientResolver>();
    resolver->outerState = outerState;
    resolver->innerEvaluator = std::move(innerEvaluator);
    return resolver;
}

} // namespace nix

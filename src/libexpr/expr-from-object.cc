#include "nix/expr/expr-from-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"

namespace nix {

// TODO: share with tracing-object.cc, tracing-replay-object.cc, ambient-object.cc
static std::string objectTypeToStringExpr(ObjectType type)
{
    switch (type) {
    case nAttrs:
        return "set";
    case nList:
        return "list";
    case nString:
        return "string";
    case nPath:
        return "path";
    case nInt:
        return "int";
    case nFloat:
        return "float";
    case nBool:
        return "bool";
    case nNull:
        return "null";
    case nFunction:
        return "lambda";
    case nExternal:
        return "external";
    case nThunk:
        return "thunk";
    case nFailed:
        return "failed";
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
    std::map<AmbientId, std::shared_ptr<Object>> outerValues; // outer values (from ambient evaluator)
    std::map<AmbientId, std::shared_ptr<Object>> localValues; // inner values (passed to callbacks)
    std::map<AmbientId, Value *> bridgedLocals;               // local id → outer Value (cached for reuse)
    EvalState * outerState = nullptr;
    std::shared_ptr<Evaluator> innerEvaluator;
    int nextId = 0;

    AmbientId registerOuter(std::shared_ptr<Object> obj)
    {
        auto id = AmbientId(nextId++);
        outerValues[id] = std::move(obj);
        return id;
    }

    AmbientId registerLocal(std::shared_ptr<Object> obj)
    {
        auto id = AmbientId(nextId++);
        localValues[id] = std::move(obj);
        return id;
    }

    std::shared_ptr<Object> resolve(AmbientId id)
    {
        auto it = outerValues.find(id);
        if (it != outerValues.end())
            return it->second;
        auto lit = localValues.find(id);
        if (lit != localValues.end())
            return lit->second;
        throw Error("ambient query: unknown value id %d", id.value());
    }

    AmbientQueryResult query(AmbientId objectId, const trace::QueryVariant & q)
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
                        return {trace::ResultType{objectTypeToStringExpr(obj->getType())}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return {trace::ResultMaybeType{std::nullopt}, std::nullopt};
                        auto childId = registerOuter(child);
                        return {
                            trace::ResultMaybeType{
                                std::optional<std::string>{objectTypeToStringExpr(child->getType())}},
                            childId};
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
                        return {trace::ResultType{objectTypeToStringExpr(child->getType())}, childId};
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

    AmbientId apply(AmbientId fnId, std::shared_ptr<Object> argObj)
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

/* Out-of-line virtual definitions so the abstract base gets a key
   function — without these, clang's `-Wweak-vtables` reports the
   vtable as emitted in every TU. */
void ExprProxy::show(const SymbolTable & symbols, std::ostream & str) const
{
    str << "<proxy>";
}

void ExprProxy::bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env)
{
    // No variables to bind - we pull from external sources, not the environment
}

/**
 * Create a PrimOp for a function defined inside the cache boundary.
 * Calls route through the inner evaluator, with the ambient resolver
 * bridging arguments between outer and inner.
 */
static PrimOp * makeCachedFnPrimOp(
    std::shared_ptr<Object> fnObj, std::shared_ptr<Evaluator> innerEval, std::shared_ptr<AmbientResolver> resolver)
{
    return new
#if NIX_USE_BOEHMGC
        (GC)
#endif
            PrimOp{
                .name = "<cached-fn>",
                .args = {"args"},
                .arity = 1,
                .impl =
                    [fnObj, innerEval, resolver](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                        // Do NOT force args[0] — it may be self-referential.
                        auto outerArgObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                        auto rootId = resolver->registerOuter(outerArgObj);
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        AmbientQueryFn queryFn = [resolver,
                                                  &innerEnv](AmbientId objectId, const trace::QueryVariant & q) {
                            auto qr = resolver->query(objectId, q);
                            innerEnv.ambientQuery(q, [&](const trace::QueryVariant &) { return qr.result; });
                            return qr;
                        };
                        AmbientApplyFn applyFn = [resolver, &innerEnv](AmbientId fnId, std::shared_ptr<Object> argObj) {
                            auto resultId = resolver->apply(fnId, std::move(argObj));
                            trace::QueryApply applyQuery{
                                std::to_string(fnId.value()), std::to_string(resultId.value())};
                            innerEnv.ambientQuery(applyQuery, [&](const trace::QueryVariant &) -> trace::ResultVariant {
                                return trace::ResultType{"apply"};
                            });
                            return resultId;
                        };
                        /* lazy-paths: pin AmbientObject's path SourceRoot
                           on the outer EvalState's `rootFSRoot` so the
                           SourceRoot outlives the Values the outer
                           evaluator builds from any returned RootedPaths. */
                        auto contraArg =
                            make_ref<AmbientObject>(rootId, std::move(queryFn), state.rootFSRoot, std::move(applyFn));
                        auto result = innerEval->apply(ref<Object>(fnObj), contraArg);
                        ExprFromObject(result.get_ptr(), innerEval, resolver).eval(state, state.baseEnv, v);
                    },
                .getFunctionInfo = [fnObj]() -> std::optional<FunctionInfo> { return fnObj->getFunctionInfo(); },
            };
}

/**
 * Create a PrimOp for an ambient function (from the outer evaluator).
 * Calls dispatch through AmbientObject::queryApply without an inner evaluator.
 */
static PrimOp * makeAmbientFnPrimOp(std::shared_ptr<Object> fnObj, std::shared_ptr<AmbientResolver> resolver)
{
    return new
#if NIX_USE_BOEHMGC
        (GC)
#endif
            PrimOp{
                .name = "<ambient-fn>",
                .args = {"args"},
                .arity = 1,
                .impl =
                    [fnObj, resolver](EvalState & state, const PosIdx pos, Value ** args, Value & v) {
                        auto * ambient = dynamic_cast<AmbientObject *>(&*fnObj);
                        if (!ambient)
                            state.error<TypeError>("expected an ambient function object").atPos(pos).debugThrow();
                        auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                        auto result = ambient->queryApply(std::move(argObj));
                        ExprFromObject(result, nullptr, resolver).eval(state, state.baseEnv, v);
                    },
                .getFunctionInfo = [fnObj]() -> std::optional<FunctionInfo> { return fnObj->getFunctionInfo(); },
            };
}

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
        PrimOp * primOp;
        if (innerEvaluator) {
            assert(ambientResolver && "inner evaluator requires ambient resolver");
            primOp = makeCachedFnPrimOp(obj, innerEvaluator, ambientResolver);
        } else {
            primOp = makeAmbientFnPrimOp(obj, ambientResolver);
        }
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

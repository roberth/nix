#include "nix/expr/expr-from-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/tracing-writer.hh"

namespace nix {

/**
 * Stateful resolver mapping ambient ids to outer/local Objects.
 *
 * Ambient ids are SHA-256 hashes:
 * - Seed roots: hashString("seed:N") for outer values entering the
 *   inner; hashString("local:N") for local values reaching back to
 *   the outer through a covariant callback.
 * - Derived ids: the producer query's queryHash. A child Object
 *   reached via getAttr("f") on parent P is identified by
 *   queryHash(QueryGetAttr{name="f", from=hex(P)}). On replay the
 *   walker recovers P from the Requests pool and re-dispatches the
 *   producer query, yielding the same child by Merkle identity.
 * - Apply results: queryHash(QueryApply{fn=hex(fnId), arg=hex(argId)})
 *   under which the resolver registers the outer's mkApp Object.
 *   The apply Request is also inserted into the pool so downstream
 *   `from=<apply_qH>` Facts can chase identity back.
 */
struct AmbientResolver : std::enable_shared_from_this<AmbientResolver>
{
    std::map<AmbientId, std::shared_ptr<Object>> outerValues;
    std::map<AmbientId, std::shared_ptr<Object>> localValues;
    std::map<AmbientId, Value *> bridgedLocals;
    EvalState * outerState = nullptr;
    std::shared_ptr<Evaluator> innerEvaluator;
    /* Writer for the inner trace. When set, the resolver wraps
       covariant-callback args in TracingLocalObject so the outer's
       accesses on them land in the inner's factSet as Facts whose
       response payloads can be replayed back from the Responses
       pool. Null when no inner writer is plumbed in — the wrap is
       skipped and replay can't hit on the apply. */
    TracingWriter * innerWriter = nullptr;
    /* SourceRoot for TracingLocalObject's getPath. Reused from the
       outer EvalState's rootFSRoot. Held as shared_ptr (rather than
       ref) so AmbientResolver stays default-constructible. */
    std::shared_ptr<SourceRoot> outerRootFSRoot;

    /* Separate counters for seed vs local roots — the strings
       `hashString("seed:N")` and `hashString("local:N")` already
       namespace them in the wire format, but using one counter
       per namespace keeps assignments stable when one side
       advances without the other. */
    unsigned int nextSeedCounter = 0;
    unsigned int nextLocalCounter = 0;

    /** Allocate a fresh outer seed-id hash and register the Object under it. */
    AmbientId registerOuterSeed(std::shared_ptr<Object> obj)
    {
        auto id = hashString(HashAlgorithm::SHA256, "seed:" + std::to_string(nextSeedCounter++));
        outerValues[id] = std::move(obj);
        return id;
    }

    /** Allocate a fresh local seed-id hash and register the Object under it. */
    AmbientId registerLocalSeed(std::shared_ptr<Object> obj)
    {
        auto id = hashString(HashAlgorithm::SHA256, "local:" + std::to_string(nextLocalCounter++));
        localValues[id] = std::move(obj);
        return id;
    }

    /** Register an outer value under an explicit id (used for
        derived values, where the id is the producer query's
        queryHash). Idempotent: if id is already mapped, overwrites. */
    void registerOuterAt(AmbientId id, std::shared_ptr<Object> obj)
    {
        outerValues[id] = std::move(obj);
    }

    std::shared_ptr<Object> resolve(AmbientId id)
    {
        auto it = outerValues.find(id);
        if (it != outerValues.end())
            return it->second;
        auto lit = localValues.find(id);
        if (lit != localValues.end())
            return lit->second;
        throw Error("ambient query: unknown value id %s", id.to_string(HashFormat::Base16, false));
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
                        return {trace::ResultType{objectTypeToString(obj->getType())}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return {trace::ResultMaybeType{std::nullopt}, std::nullopt};
                        /* Derived child id is the producer query's queryHash. */
                        auto childId = TracingDecisionGraph::computeQueryHash(query);
                        registerOuterAt(childId, child);
                        return {
                            trace::ResultMaybeType{std::optional<std::string>{objectTypeToString(child->getType())}},
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
                        auto childId = TracingDecisionGraph::computeQueryHash(query);
                        registerOuterAt(childId, child);
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

    /** Apply an outer fn (resolved from fnId) to a local argObj.
     *  Returns a pair: (argId, resultId). argId is the local seed
     *  Hash assigned to argObj; resultId is the producer queryHash
     *  of QueryApply{fn=fnId, arg=argId}, under which the
     *  resulting Object is registered as an outer value. The
     *  caller (applyFn closure) records the QueryApply Fact with
     *  the same arg id. */
    std::pair<AmbientId, AmbientId> apply(AmbientId fnId, std::shared_ptr<Object> argObj)
    {
        if (!outerState)
            throw Error("ambient apply requires outerState");
        auto fnObj = resolve(fnId);

        /* Register the arg as a local seed (id = hashString("local:N")). */
        auto argId = registerLocalSeed(argObj);

        /* Wrap the argObj in TracingLocalObject so the outer's
           accesses on it during the apply land in the inner trace
           with `from=hex(argId)`. The recorder always stores those
           response payloads; the replay dispatcher reads them back
           since there's no live inner to recompute against. */
        auto wrappedArg = (innerWriter && outerRootFSRoot)
            ? std::shared_ptr<Object>(std::make_shared<TracingLocalObject>(
                  argObj, argId, *innerWriter, ref<SourceRoot>(outerRootFSRoot)))
            : argObj;

        /* Bridge local arg via ExprFromObject with the inner evaluator */
        auto & argThunk = bridgedLocals[argId];
        if (!argThunk) {
            argThunk = outerState->allocValue();
            auto * argExpr = new ExprFromObject(wrappedArg, innerEvaluator, shared_from_this());
            outerState->mkThunk_(*argThunk, argExpr);
        }

        /* Build the outer mkApp thunk. */
        auto fnVal = fnObj->defeatCache();
        auto * resultVal = outerState->allocValue();
        resultVal->mkApp(*fnVal, argThunk);
        auto resultObj = std::make_shared<InterpreterObject>(*outerState, allocRootValue(resultVal));

        /* Result id is queryHash(QueryApply{fn=fnId, arg=argId}). */
        auto fnIdStr  = fnId.to_string(HashFormat::Base16, false);
        auto argIdStr = argId.to_string(HashFormat::Base16, false);
        trace::QueryApply applyQuery{fnIdStr, argIdStr};
        auto resultId = TracingDecisionGraph::computeQueryHash(applyQuery);
        registerOuterAt(resultId, std::move(resultObj));

        /* Insert the QueryApply Request into the decision graph's
           Requests pool, so resolveAmbientId on replay can find the
           apply by its result id and re-invoke it. We don't add it
           to the FactSet -- QueryApply has no result type (it's a
           fresh app thunk), so there's no Fact body to record.

           Also insert a sidecar Request at the local arg id that
           points back to this apply. Without it, replay can't
           resolve a local id whose first dispatched Fact is a
           local-incoming observation (the walk picks Facts in
           hash-set order; the corresponding apply-result Fact may
           be dispatched later). The sidecar lets resolveAmbientId
           chase localId -> applyResultId -> invoke apply, which
           registers the live argObj under localId in idToObject. */
        if (innerWriter)
            if (auto * dg = innerWriter->getDecisionGraph()) {
                nlohmann::json applyJson = applyQuery;
                dg->insertRequest(resultId, jsonToCborString(applyJson));
                nlohmann::json localSidecar = {
                    {"kind", "localArg"},
                    {"applyResultId", resultId.to_string(HashFormat::Base16, false)},
                };
                dg->insertRequest(argId, jsonToCborString(localSidecar));
            }

        return {argId, resultId};
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
                        auto rootId = resolver->registerOuterSeed(outerArgObj);
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        AmbientQueryFn queryFn = [resolver,
                                                  &innerEnv](AmbientId objectId, const trace::QueryVariant & q) {
                            auto qr = resolver->query(objectId, q);
                            innerEnv.ambientQuery(q, [&](const trace::QueryVariant &) { return qr.result; });
                            return qr;
                        };
                        /* applyFn does NOT record a QueryApply Fact:
                           a fresh app thunk has no result type
                           ("apply" is not a value type). The
                           apply-result Object is still registered in
                           the resolver under
                           queryHash(QueryApply{fn=fnId, arg=argId}),
                           and the QueryApply Request itself is
                           inserted into the pool (see
                           AmbientResolver::apply) so downstream
                           Facts with `from=<apply_qH>` can have
                           their response payloads located via the
                           Responses pool on replay. */
                        AmbientApplyFn applyFn = [resolver](AmbientId fnId, std::shared_ptr<Object> argObj) {
                            auto [argId, resultId] = resolver->apply(fnId, std::move(argObj));
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
        /* Dispatch on obj's dynamic type. An AmbientObject wraps an
           outer value reached via ambient query; its apply must
           route through queryApply (makeAmbientFnPrimOp). A concrete
           fn with an inner evaluator goes through innerEval->apply
           (makeCachedFnPrimOp). A concrete fn without an inner
           evaluator falls back to makeAmbientFnPrimOp — the impl
           will throw at apply time (matching the prior behaviour
           for that combination, which the unit tests rely on for
           construction-only checks). */
        PrimOp * primOp;
        if (dynamic_cast<AmbientObject *>(obj.get())) {
            primOp = makeAmbientFnPrimOp(obj, ambientResolver);
        } else if (innerEvaluator) {
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

std::shared_ptr<AmbientResolver> makeAmbientResolver(
    EvalState * outerState, std::shared_ptr<Evaluator> innerEvaluator, TracingWriter * innerWriter)
{
    auto resolver = std::make_shared<AmbientResolver>();
    resolver->outerState = outerState;
    resolver->innerEvaluator = std::move(innerEvaluator);
    resolver->innerWriter = innerWriter;
    if (outerState)
        resolver->outerRootFSRoot = outerState->rootFSRoot.get_ptr();
    return resolver;
}

} // namespace nix

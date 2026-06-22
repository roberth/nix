#include "nix/expr/expr-from-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/tracing-writer.hh"

#include <nlohmann/json.hpp>

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
/* Pure-storage registry mapping content-defined ids to live outer
   Objects (values the inner reads through AmbientObject). The Local
   direction (inner values the outer reads via callback) doesn't go
   through this registry at all on replay — those are served by
   ReplayLocalObject standins from the Responses pool. Local
   registration on the recording side was previously here as a write-
   only map; dropped because nothing read it back.
   Last-write-wins on `registerOuterAt`: same-shape sibling navigation
   produces the same `childId` and overwrites the prior entry. Benign
   when shapes match (children produce same observations), but the
   root of #63 when sibling APPLY ids collide downstream. */
struct AmbientRegistry
{
    std::map<AmbientId, std::shared_ptr<Object>> outerValues;

    /** Register an outer value under an explicit id (used for
        derived values, where the id is the producer query's
        queryHash, and for apply results). Idempotent: if id is
        already mapped, overwrites. */
    void registerOuterAt(AmbientId id, std::shared_ptr<Object> obj)
    {
        outerValues[id] = std::move(obj);
    }

    std::shared_ptr<Object> resolveOuter(AmbientId id)
    {
        auto it = outerValues.find(id);
        if (it != outerValues.end())
            return it->second;
        throw Error("ambient query: unknown value id %s", id.to_string(HashFormat::Base16, false));
    }
};

/* Pure-dispatch wrapper around an Object's query interface. Knows how
   to invoke the right Object method for each QueryVariant alternative,
   how to derive a child's content-defined id from a producer query's
   payload, and how to register the derived child / delay its settled
   identity into the writer. Stateless apart from the references it
   holds. Constructed on demand from AmbientResolver members. */
struct AmbientQuery
{
    AmbientRegistry & registry;
    TracingWriter * innerWriter;

    /** Dispatch a query against the given outer Object directly,
        bypassing the resolver's id → Object lookup. Boundary-trace-
        only discipline (per the design doc): each cb apply's queryFn
        captures its own outer arg and calls this directly for seed
        observations, so sibling cb invocations don't collide on the
        shared `outerValues` map. */
    AmbientQueryResult on(std::shared_ptr<Object> obj, const trace::QueryVariant & q) const
    {
        return std::visit(
            [&](const auto & query) -> AmbientQueryResult {
                using Q = std::decay_t<decltype(query)>;
                if constexpr (std::is_same_v<Q, trace::QueryApply>) {
                    throw Error("ambient query: QueryApply should go through applyFn, not queryFn");
                } else if constexpr (!requires { query.from; }) {
                    throw Error("ambient query: query type has no 'from' field");
                } else {
                    (void) 0;  // obj already provided

                    if constexpr (std::is_same_v<Q, trace::QueryGetType>) {
                        return {trace::ResultType{objectTypeToString(obj->getType())}, std::nullopt};
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetAttr>) {
                        auto child = obj->maybeGetAttr(query.name);
                        if (!child)
                            return {trace::ResultMaybeType{std::nullopt}, std::nullopt};
                        /* Derived child id is the producer query's queryHash. */
                        auto childId = TracingDecisionGraph::computeQueryHash(query);
                        registry.registerOuterAt(childId, child);
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
                        registry.registerOuterAt(childId, child);
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
};

/* Memoised Object* → Value* cache for bridged argThunks. Lives long
   enough to span multiple apply calls within one cb body — when the
   inner passes the same argObj to the outer multiple times, the
   outer's cycle detection must see ONE Value, not many. Keyed by
   Object* identity (NOT argId): two distinct argObjs can share the
   same argId hash (e.g. a frozen ReplayLocalObject built by the
   walker's apply branch and a live InterpreterObject from a fall-back
   inner rerun both seed at depth-marker), and they correctly resolve
   to distinct thunks here. */
struct BridgedThunkCache
{
    std::map<Object *, Value *> thunks;

    template<typename Factory>
    Value * getOrCreate(Object * key, Factory && factory)
    {
        auto & v = thunks[key];
        if (!v)
            v = factory();
        return v;
    }
};

/* Orchestrates a covariant-callback apply: resolves the outer fn from
   the registry, opens a cell for the inner-supplied arg, wraps the arg
   in TracingLocalObject so outer accesses on it land in the inner
   trace, bridges the wrapped arg via ExprFromObject into an outer
   `mkApp` thunk, registers the apply result, and defers the Pass-1
   apply Request + Pass-2 localArg sidecar to the writer's flush.
   Constructed transiently per call; holds refs/copies from the owning
   resolver. The `resolverHandle` shared_ptr is required for
   ExprFromObject's `ambientResolver` field; everything else is by
   reference. */
struct AmbientApply
{
    AmbientRegistry & registry;
    BridgedThunkCache & bridgedLocals;
    EvalState * outerState;
    std::shared_ptr<Evaluator> innerEvaluator;
    TracingWriter * innerWriter;
    std::shared_ptr<SourceRoot> outerRootFSRoot;
    std::shared_ptr<AmbientResolver> resolverHandle;

    std::pair<AmbientId, AmbientId> run(
        AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgScopeCell> callerScope);
};

struct AmbientResolver : std::enable_shared_from_this<AmbientResolver>
{
    AmbientRegistry registry;
    BridgedThunkCache bridgedLocals;
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

    AmbientQueryResult query(AmbientId objectId, const trace::QueryVariant & q)
    {
        return queryOn(registry.resolveOuter(objectId), q);
    }

    AmbientQueryResult queryOn(std::shared_ptr<Object> obj, const trace::QueryVariant & q)
    {
        return AmbientQuery{registry, innerWriter}.on(std::move(obj), q);
    }

    /** Apply an outer fn (resolved from fnId) to a local argObj.
     *  Returns a pair: (argId, resultId). argId is the local seed
     *  Hash assigned to argObj; resultId is the producer queryHash
     *  of QueryApply{fn=fnId, arg=argId}, under which the
     *  resulting Object is registered as an outer value. The
     *  caller (applyFn closure) records the QueryApply Fact with
     *  the same arg id. */
    std::pair<AmbientId, AmbientId> apply(
        AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgScopeCell> callerScope)
    {
        return AmbientApply{
            registry, bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.run(fnId, std::move(argObj), std::move(callerScope));
    }
};

std::pair<AmbientId, AmbientId> AmbientApply::run(
    AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgScopeCell> callerScope)
{
    if (!outerState)
        throw Error("ambient apply requires outerState");
    auto fnObj = registry.resolveOuter(fnId);

    /* Scope-graph cell for the cb arg, rooted at the caller's
       effective scope (which AmbientObject::queryApply passes in
       because a resolved fn may be an InterpreterObject without a
       proxy parent chain). The cell carries only topology. */
    auto localCell = ArgScopeCell::make(callerScope, argObj);
    /* CDI fix: the local arg's Subject is the static positional handle
       at this apply-stack depth. Its argId = contentIdAfter(subject,
       {}) = positional initial. Cb body observations evolve the
       per-Asks-edge content id at flush. */
    cidasks::Subject argSubject{cidasks::PositionalSeed{localCell->depth}};
    auto argId = cidasks::contentIdAfter(argSubject, {});

    /* Wrap the argObj in TracingLocalObject so the outer's
       accesses on it during the apply land in the inner trace
       with `from=hex(argId)`. */
    auto wrappedArg = (innerWriter && outerRootFSRoot)
        ? std::shared_ptr<Object>(std::make_shared<TracingLocalObject>(
              argObj, argSubject, *innerWriter, ref<SourceRoot>(outerRootFSRoot), localCell))
        : argObj;

    /* Bridge local arg via ExprFromObject. The cache memoises by
       argObj identity so cycle detection sees one Value per logical
       arg (see BridgedThunkCache for why pointer-identity, not argId). */
    auto * argThunk = bridgedLocals.getOrCreate(argObj.get(), [&]() {
        auto * v = outerState->allocValue();
        auto * expr = new ExprFromObject(wrappedArg, innerEvaluator, resolverHandle);
        outerState->mkThunk_(*v, expr);
        return v;
    });

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
    registry.registerOuterAt(resultId, std::move(resultObj));

    /* Defer the QueryApply Request and the localArg sidecar to the
       writer's flush at logResult. Pool entries land at the natural
       reqHashes (no substitution under the via-Asks design's
       single-edge default). */
    if (innerWriter) {
        nlohmann::json applyJson = applyQuery;
        innerWriter->deferRequest(applyJson);
        nlohmann::json localSidecar = {
            {"kind", "localArg"},
            {"applyResultId", resultId.to_string(HashFormat::Base16, false)},
        };
        innerWriter->deferRequest(localSidecar, argIdStr);
    }

    return {argId, resultId};
}

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
                        /* Scope-graph cell for this seed. Parent = the
                           fn proxy's cell (so curried applies chain
                           through depth 0, 1, ... naturally).
                           cell.liveObject is set to the AmbientObject
                           we're about to construct (below) so chain
                           navigation returns the proxy. */
                        auto parentCell = effectiveArgScope(*fnObj);
                        auto seedCell = ArgScopeCell::make(parentCell, /*liveObject set below*/ nullptr);
                        /* CDI fix: this seed's Subject is the positional
                           handle at this static apply-stack depth.
                           Sibling cb apply invocations share the same
                           Subject and discriminate via their observation
                           factsets, not via state-creep. */
                        cidasks::Subject seedSubject{cidasks::PositionalSeed{seedCell->depth}};
                        auto rootId = cidasks::contentIdAfter(seedSubject, {});
                        /* Boundary-trace-only discipline: do NOT
                           register outerArgObj under rootId in the
                           shared resolver. Sibling cb apply invocations
                           share the same rootId (= cell.contentId() at
                           apply time = depth marker for empty cell), so
                           a shared registration would last-write-wins
                           and queryFn closures would all resolve to the
                           latest outer arg. Instead each invocation's
                           queryFn captures its own outerArgObj and uses
                           it directly for seed (rootId) queries. */
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        AmbientQueryFn queryFn = [resolver, outerArgObj, rootId,
                                                  &innerEnv](
                            AmbientId objectId,
                            const trace::QueryVariant & q,
                            cidasks::Subject subject) {
                            /* For cb-arg queries (objectId == this cb's
                               rootId), dispatch on the captured
                               outerArgObj directly — bypass the shared
                               resolver lookup. For derived ids (child
                               objects from earlier getAttr/getListElem),
                               delegate to the resolver which has them
                               registered. */
                            AmbientQueryResult qr = (objectId == rootId)
                                ? resolver->queryOn(outerArgObj, q)
                                : resolver->query(objectId, q);
                            innerEnv.ambientQuery(
                                q,
                                [&](const trace::QueryVariant &) { return qr.result; },
                                std::move(subject));
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
                        AmbientApplyFn applyFn = [resolver](
                            AmbientId fnId,
                            std::shared_ptr<Object> argObj,
                            std::shared_ptr<const ArgScopeCell> callerScope) {
                            auto [argId, resultId] = resolver->apply(fnId, std::move(argObj), std::move(callerScope));
                            return resultId;
                        };
                        /* lazy-paths: pin AmbientObject's path SourceRoot
                           on the outer EvalState's `rootFSRoot` so the
                           SourceRoot outlives the Values the outer
                           evaluator builds from any returned RootedPaths. */
                        auto contraArg =
                            make_ref<AmbientObject>(std::move(seedSubject), std::move(queryFn), state.rootFSRoot, std::move(applyFn));
                        /* Wire seedCell.liveObject to contraArg now
                           that it exists. This is the deliberate
                           shared_ptr cycle documented on
                           ArgScopeCell::liveObject. */
                        seedCell->liveObject = contraArg.get_ptr();
                        contraArg->withScope(seedCell);
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
                        auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                        auto result = fnObj->queryApply(std::move(argObj));
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

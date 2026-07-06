#include "nix/expr/expr-from-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/replay-callback-arg.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-callback-arg.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-writer.hh"

#include <cassert>
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
   ReplayCallbackArg standins from LocalResponseMap. Local
   registration on the recording side was previously here as a write-
   only map; dropped because nothing read it back. */
struct AmbientRegistry
{
    std::map<AmbientId, std::shared_ptr<Object>> outerValues;

    /** Register an outer value under an explicit id (used for
        derived values, where the id is the producer query's
        queryHash, and for apply results). Single-entry contract:
        eval is reproducible, so two distinct entries arriving at
        the same id is a reproducibility bug to surface rather than
        suppress. */
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

                    if constexpr (std::is_same_v<Q, trace::QueryGetWHNF>) {
                        return {computeWHNFFromObject(*obj), std::nullopt};
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
                    } else if constexpr (std::is_same_v<Q, trace::QueryGetListElem>) {
                        auto child = obj->getListElem(query.index);
                        auto childId = TracingDecisionGraph::computeQueryHash(query);
                        registry.registerOuterAt(childId, child);
                        return {trace::ResultType{objectTypeToString(child->getType())}, childId};
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
   same argId hash (e.g. a frozen ReplayCallbackArg built by the
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
   in TracingCallbackArg so outer accesses on it land in the inner
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
        AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope);

    /** Same as run, but the fnObj is provided directly instead of
        being resolved via the registry. Used by makeCachedFnPrimOp's
        applyFn closure when the seed AmbientObject itself is being
        applied (= fnId is the seed's argStateId, which boundary discipline
        keeps unregistered to avoid sibling collisions; the closure
        captures outerArgObj instead). */
    std::pair<AmbientId, AmbientId> runOn(
        std::shared_ptr<Object> fnObj, AmbientId fnId,
        std::shared_ptr<Object> argObj,
        std::shared_ptr<const ArgCell> callerScope);
};

struct AmbientResolver : std::enable_shared_from_this<AmbientResolver>
{
    AmbientRegistry registry;
    BridgedThunkCache bridgedLocals;
    EvalState * outerState = nullptr;
    std::shared_ptr<Evaluator> innerEvaluator;
    /* Writer for the inner trace. When set, the resolver wraps
       covariant-callback args in TracingCallbackArg so the outer's
       accesses on them land in the inner's factSet as Facts whose
       response payloads can be replayed back from the Responses
       pool. Null when no inner writer is plumbed in — the wrap is
       skipped and replay can't hit on the apply. */
    TracingWriter * innerWriter = nullptr;
    /* SourceRoot for TracingCallbackArg's getPath. Reused from the
       outer EvalState's rootFSRoot. Held as shared_ptr (rather than
       ref) so AmbientResolver stays default-constructible. */
    std::shared_ptr<SourceRoot> outerRootFSRoot;
    /* Inherited scope (argStateId of this cached call's Q) — used by the
       cb-apply boundary to make sibling cached calls' scope state ids
       distinct via cidasks inheritance. Zero hash means no
       inheritance (= no scope discrimination). */
    Hash callScope = Hash(HashAlgorithm::SHA256);

    /* Outer-direction proxies registered live by the standin's
       `<replay-local-lambda>` primop (= `registerAmbientResolverProxy`).
       Keyed by `(subject, scope)` so the walker's `resolveCdiId`
       can match the registered seed's cidasks-evolved argStateId at any
       walk-edge index, not just the initial one. List rather than
       map because subject equality isn't trivially hashable;
       n_registrations is small (= one per cb-apply boundary the
       primop fires at). */
    struct LiveProxyEntry
    {
        Subject subject;
        Hash scope;
        std::shared_ptr<Object> obj;
    };
    std::vector<LiveProxyEntry> liveProxies;

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
        AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return AmbientApply{
            registry, bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.run(fnId, std::move(argObj), std::move(callerScope));
    }

    /** Apply variant where fnObj is provided directly (= callers
        with a captured reference, like makeCachedFnPrimOp's
        applyFn closure for seed-self applies). */
    std::pair<AmbientId, AmbientId> applyOn(
        std::shared_ptr<Object> fnObj, AmbientId fnId,
        std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return AmbientApply{
            registry, bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.runOn(std::move(fnObj), fnId, std::move(argObj), std::move(callerScope));
    }
};

std::pair<AmbientId, AmbientId> AmbientApply::run(
    AmbientId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
{
    auto fnObj = registry.resolveOuter(fnId);
    return runOn(std::move(fnObj), fnId, std::move(argObj), std::move(callerScope));
}

std::pair<AmbientId, AmbientId> AmbientApply::runOn(
    std::shared_ptr<Object> fnObj, AmbientId fnId,
    std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
{
    if (!outerState)
        throw Error("ambient apply requires outerState");

    /* Scope-graph cell for the cb arg, rooted at the caller's
       effective scope (which AmbientObject::queryApply passes in
       because a resolved fn may be an InterpreterObject without a
       proxy parent chain). The cell carries only topology. */
    auto localCell = ArgCell::make(callerScope, argObj);
    /* Each new value that crosses INTO a cb-apply boundary is
       treated uniformly as a value — no inherited Subject is
       propagated. Identity at this boundary starts fresh as
       PositionalSeed at the apply's static (reverse-De-Bruijn)
       depth; the body's own observations on the arg evolve the scopeStateId
       within the Asks structure. This keeps observations at the
       boundary maximally predictable — two cb calls observing the
       same way through their args reach the same trie position
       regardless of where the arg's source came from. */
    Subject argSubject{PositionalSeed{localCell->depth}};
    /* Sample resolver->callScope at fire time. TracingEvaluator::apply
       leaves callScope at the current sibling's siblingScope (no
       restore), so this sample reflects the CURRENT sibling context
       walker is operating under. Do not freeze at closure-creation
       time — the scope evolves, and freezing would emit stale hashes. */
    Hash argScope = resolverHandle->callScope;
    auto argId = scopeStateIdAfter(argSubject, argScope, {});
    tracingCacheLog("AmbientApply::run: argScope=%s argId=%s",
                    argScope.to_string(HashFormat::Base16, false).substr(0, 12),
                    argId.to_string(HashFormat::Base16, false).substr(0, 12));

    /* Compute the resultId early so we can pass it to the
       TracingCallbackArg as depth2ApplyId — groups all depth-2 facts
       made on this local (and its descendants) into a single
       AmbientAsks edge at flush. */
    auto fnIdStr  = fnId.to_string(HashFormat::Base16, false);
    auto argIdStr = argId.to_string(HashFormat::Base16, false);

    /* cb-apply boundary: record the apply's synthetic walk-advance
       edge (= ε) now that we have fnIdStr and argIdStr. See parallel
       call in TracingEvaluator::apply for the principle. */
    if (innerWriter) {
        nlohmann::json applyQ = trace::QueryApply{fnIdStr, argIdStr};
        tracingCacheLog("openApplyBoundary callsite=AmbientApply::run fn=%s arg=%s",
                        fnIdStr.substr(0, 12), argIdStr.substr(0, 12));
        innerWriter->openApplyBoundary(applyQ);
    }
    trace::QueryApply applyQuery{fnIdStr, argIdStr};
    auto resultId = TracingDecisionGraph::computeQueryHash(applyQuery);

    /* Wrap the argObj in TracingCallbackArg so the outer's
       accesses on it during the apply land in the inner trace
       with `from=hex(argId)`. Inherit callScope so sibling cached
       calls' local-args have distinct scope state ids.

       Skip the TLO wrap when argObj is a ReplayCallbackArg. At warm
       replay, the RLO standin reaching `runOn` already encapsulates
       the recorded contract for the cb-arg crossing — the standin's
       primop and its synthetic apply-result handle the per-probe
       AmbientAsks lookups directly. Wrapping the standin in TLO
       would (1) add a redundant recording layer with no new
       information to capture (the writer isn't recording here at
       warm) and (2) convert the standin's primop into the
       `<cached-fn>(TLO)` cascade that bypasses the design's
       lambda-LO mechanism — exactly the bypass diagnosed in
       `tracing-eval-cache-higher-order-replay.md`. At cold, argObj
       is an `InterpreterObject` of a real inner Value and the cast
       returns null, leaving the TLO wrap path unchanged. */
    auto wrappedArg = (innerWriter && outerRootFSRoot
                       && !dynamic_cast<ReplayCallbackArg *>(argObj.get()))
        ? std::shared_ptr<Object>(std::make_shared<TracingCallbackArg>(
              argObj, argSubject, *innerWriter, ref<SourceRoot>(outerRootFSRoot), localCell,
              resolverHandle->callScope, resultId))
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

    /* Result id is queryHash(QueryApply{fn=fnId, arg=argId})
       (already computed above for depth2ApplyId plumbing). */
    registry.registerOuterAt(resultId, std::move(resultObj));

    /* Defer the QueryApply Request and the localArg sidecar to the
       writer's flush at logResult. Pool entries land at the natural
       reqHashes (no substitution under the via-Asks design's
       single-edge default).

       The sidecar carries `localType` so the replay-side walker
       can detect non-reconstructible locals (functions) without
       forcing them. ReplayCallbackArg can serve scalar/structural
       responses from LocalResponseMap, but a function local has
       no recorded body to apply against a divergent argument — so
       the walker bails on dispatch in that case and the depth-1
       fallback (= live re-eval) handles it. */
    if (innerWriter) {
        nlohmann::json applyJson = applyQuery;
        innerWriter->deferRequest(applyJson);
        nlohmann::json localSidecar = {
            {"kind", "localArg"},
            {"applyResultId", resultId.to_string(HashFormat::Base16, false)},
            /* Depth + scope let the replay-side lambda primop compose
               the synthetic apply-result subject as
               `ApplyResultSubject{fn=this.subject, arg=PositionalSeed{depth+1}}`
               with `scope` — matching what the writer's recording
               produced when AmbientObject::queryApply built the apply
               result's subject. Without these fields the synthetic
               falls back to PostulatedIdempotentRead encoding which disagrees
               with the recorder's encoding, breaking CAS reads of
               the apply-result observations. */
            {"depth", localCell->depth},
            {"scope", resolverHandle->callScope.to_string(HashFormat::Base16, false)},
        };
        /* getTypeLazy (not getType) avoids forcing self-referential
           thunks like `args // { extra = true; }` where args is
           defined in terms of the apply itself (= selfref-fn,
           mkOverridable patterns in builtins-cache.sh). It returns
           nThunk for unforced values, which we just don't record.
           Also wrapped in try/catch because dispatch-time ReplayCallbackArg
           may have no recorded type fact. */
        try {
            auto t = argObj->getTypeLazy();
            if (t != nThunk)
                localSidecar["localType"] = objectTypeToString(t);
        } catch (...) {
            /* Replay-side path or unrecorded — skip. */
        }
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
                        auto parentCell = effectiveArgCell(*fnObj);
                        auto seedCell = ArgCell::make(parentCell, /*liveObject set below*/ nullptr);
                        /* argStateId fix: this seed's Subject is the positional
                           handle at this static apply-stack depth.
                           Sibling cb apply invocations share the same
                           Subject and discriminate via their observation
                           factsets, not via state-creep. */
                        Subject seedSubject{PositionalSeed{seedCell->depth}};
                        /* Inherit the resolver's callScope (= argStateId(Q)
                           of this cached call). Sibling cached calls
                           with different Qs get distinct rootIds and
                           therefore distinct subject-derived content
                           ids throughout this cb-apply boundary. */
                        Hash callScope = resolver->callScope;
                        auto rootId = scopeStateIdAfter(seedSubject, callScope, {});
                        /* Per-apply observation context. Captures the
                           outer's probes on the cb arg as they fire
                           through queryFn; the apply-result wrapper
                           uses these observations to compute its
                           evolved scope state id (via cidasks
                           ApplyResultSubject recursion through the
                           arg's evolved scopeStateId). This is what
                           distinguishes sibling apply calls within
                           the same cached call (`inner.f 5` vs
                           `inner.f 2`), per the depth-2 design. */
                        auto applyContext = std::make_shared<ApplyContext>(
                            ApplyContext{seedSubject, callScope, {}});
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
                                                  &innerEnv, applyContext](
                            AmbientId objectId,
                            const trace::QueryVariant & q,
                            Subject subject,
                            Hash inheritedScope) {
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
                                subject,
                                inheritedScope);
                            /* Note: queryFn (= cb-arg side) observations
                               are NOT pushed into applyContext.observations.
                               They would be noise from the apply-result
                               wrapper's perspective (their `fromHash` is
                               the cb-arg seed's argStateId, not the wrapper's,
                               so the cidasks own-loop on the wrapper
                               doesn't fold them in) but the walk's size
                               growing from these silent pushes would
                               diverge writer (queryFn fires before
                               evolvedQueryFrom because inner.method()
                               is called first) from walker (queryFn
                               fires after evolvedQueryFrom because
                               parentHash must be computed first to
                               build the query). The cb-arg
                               AmbientObject's own argStateId uses
                               structuralAddressAfter with empty walk
                               (= content-only) anyway, so dropping
                               these pushes is consistent throughout. */
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
                           LocalResponseMap on replay. */
                        AmbientApplyFn applyFn = [resolver, outerArgObj, rootId](
                            AmbientId fnId,
                            std::shared_ptr<Object> argObj,
                            std::shared_ptr<const ArgCell> callerScope) {
                            /* Boundary-trace-only discipline keeps the
                               cb-arg seed unregistered in
                               AmbientRegistry. When the SEED ITSELF is
                               applied (= inner does `args 5` on the
                               seed AmbientObject), fnId == rootId.
                               `resolver->apply` would try
                               `resolveOuter(rootId)` and throw
                               "unknown value id". Route through
                               `applyOn` with the captured outerArgObj
                               instead — same path as queryFn's
                               `queryOn` shortcut for direct seed
                               queries. */
                            if (fnId == rootId) {
                                auto [argId, resultId] = resolver->applyOn(
                                    outerArgObj, fnId, std::move(argObj), std::move(callerScope));
                                return resultId;
                            }
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
                           ArgCell::liveObject. */
                        seedCell->liveObject = contraArg.get_ptr();
                        contraArg->withScope(seedCell);
                        contraArg->withInheritedScope(callScope);
                        contraArg->withApplyContext(applyContext);
                        tracingCacheLog("makeCachedFnPrimOp.impl: contraArg=%p seedCell=%p callScope=%s outerArg=%p",
                                        (void*)contraArg.get_ptr().get(), (void*)seedCell.get(),
                                        callScope.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                                        (void*)outerArgObj.get());
                        auto result = innerEval->apply(ref<Object>(fnObj), contraArg);
                        tracingCacheLog("makeCachedFnPrimOp.impl: apply result=%p", (void*)result.get_ptr().get());
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
    tracingCacheLog("ExprFromObject::eval type=%s obj=%p", objectTypeToString(type), (void*)obj.get());

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
        /* ReplayCallbackArg reconstructs a lambda LocalObject as a
           primop via its own `toValueOrProxy` (= the
           <replay-local-lambda> mechanism in
           replay-callback-arg.cc). Use it directly so the recorded
           d=2 chain drives apply-time behaviour; the generic
           cached/ambient primops here would dispatch on
           `RLO::queryApply` which throws by design. */
        if (dynamic_cast<ReplayCallbackArg *>(obj.get())) {
            auto val = obj->toValueOrProxy(state, ambientResolver);
            v = **val;
            break;
        }
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

void setAmbientResolverCallScope(AmbientResolver & resolver, Hash callScope)
{
    resolver.callScope = std::move(callScope);
}

Hash getAmbientResolverCallScope(const AmbientResolver & resolver)
{
    return resolver.callScope;
}

void registerAmbientResolverProxy(
    AmbientResolver & resolver,
    Subject subject,
    Hash scope,
    std::shared_ptr<Object> obj)
{
    /* Overwrite-on-conflict for the same (subject, scope) key. The
       primop fires once per cb-apply boundary it covers; re-firing
       with the same args produces the same registration. Different
       boundaries register different subjects (= different
       `applyDepth+1` values), so collisions across boundaries
       within one cache call are non-existent unless sibling
       cb-applies share the same cb-arg seed depth — same
       boundary-trace-only caveat as the previous argStateId-keyed version.

       `Subject` has no `operator==`; the primop only ever
       registers `PositionalSeed{depth}` here, so structural
       equality reduces to comparing the depth field. Asserting on
       the variant tag keeps this collapse honest if a future caller
       passes a different variant. */
    auto * newSeed = std::get_if<PositionalSeed>(&subject.data);
    assert(newSeed && "registerAmbientResolverProxy: subject must be a PositionalSeed");
    for (auto & entry : resolver.liveProxies) {
        auto * existingSeed = std::get_if<PositionalSeed>(&entry.subject.data);
        if (existingSeed && existingSeed->depth == newSeed->depth && entry.scope == scope) {
            entry.obj = std::move(obj);
            return;
        }
    }
    resolver.liveProxies.push_back({std::move(subject), std::move(scope), std::move(obj)});
}

std::shared_ptr<Object> tryResolveAmbientResolverProxy(
    AmbientResolver & resolver,
    const Hash & idHash,
    const std::vector<Edge> & cidasksWalk,
    TracingDecisionGraph * dg)
{
    /* Linear scan over each registered (subject, scope) x K in
       cidasksWalk. The hasSubjectStampSite gate turned out to be a
       tautology (cold stamped every CID walker ever resolves), so
       running the scan unconditionally is equivalent. */
    (void) dg;
    for (auto & entry : resolver.liveProxies) {
        for (size_t k = 0; k <= cidasksWalk.size(); ++k) {
            auto scopeStateId = scopeStateIdAt(entry.subject, entry.scope, cidasksWalk, k);
            if (scopeStateId == idHash)
                return entry.obj;
        }
    }
    return nullptr;
}

} // namespace nix

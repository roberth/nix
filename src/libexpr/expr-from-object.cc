#include "nix/expr/expr-from-object.hh"
#include "nix/expr/outer-object.hh"
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
 * - Arg roots: hashString("arg:N") for outer values entering the
 *   inner; hashString("local:N") for local values reaching back to
 *   the outer through a covariant callback.
 * - Derived ids: the producer query's queryHash. A child Object
 *   reached via getAttr("f") on parent P is identified by
 *   queryHash(QueryGetAttr{name="f", from=hex(P)}). On replay the
 *   walker recovers P from the Requests pool and re-dispatches the
 *   producer query, yielding the same child by Merkle identity.
 * - Apply results: queryHash(QueryApply{fn=hex(fnId), arg=hex(argSubject)})
 *   under which the resolver registers the outer's mkApp Object.
 *   The apply Request is also inserted into the pool so downstream
 *   `from=<apply_qH>` Facts can chase identity back.
 */
/* Pure-storage registry mapping state hashs to live outer
   Objects (values the inner reads through OuterObject). The Local
   direction (inner values the outer reads via callback) doesn't go
   through this registry at all on replay — those are served by
   ReplayCallbackArg standins from InnerValueResponse. Local
   registration on the recording side was previously here as a write-
   only map; dropped because nothing read it back. */
struct OuterRegistry
{
    std::map<OuterId, std::shared_ptr<Object>> outerValues;

    /** Register an outer value under an explicit id (used for
        derived values, where the id is the producer query's
        queryHash, and for apply results). Single-entry contract:
        eval is reproducible, so two distinct entries arriving at
        the same id is a reproducibility bug to surface rather than
        suppress. */
    void registerOuterAt(OuterId id, std::shared_ptr<Object> obj)
    {
        outerValues[id] = std::move(obj);
    }

    std::shared_ptr<Object> resolveOuter(OuterId id)
    {
        auto it = outerValues.find(id);
        if (it != outerValues.end())
            return it->second;
        throw Error("ambient query: unknown value id %s", id.to_string(HashFormat::Base16, false));
    }
};

/* Pure-dispatch wrapper around an Object's query interface. Knows how
   to invoke the right Object method for each QueryVariant alternative,
   how to derive a child's state hash from a producer query's
   payload, and how to register the derived child / delay its settled
   identity into the writer. Stateless apart from the references it
   holds. Constructed on demand from OuterResolver members. */
struct OuterQuery
{
    OuterRegistry & registry;
    TracingWriter * innerWriter;

    /** Dispatch a query against the given outer Object directly,
        bypassing the resolver's id → Object lookup. Boundary-trace-
        only discipline (per the design doc): each cb apply's queryFn
        captures its own outer arg and calls this directly for arg
        observations, so sibling cb invocations don't collide on the
        shared `outerValues` map. */
    OuterQueryResult on(std::shared_ptr<Object> obj, const trace::QueryVariant & q) const
    {
        return std::visit(
            [&](const auto & query) -> OuterQueryResult {
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
   Object* identity (NOT argSubject): two distinct argObjs can share the
   same argSubject hash (e.g. a frozen ReplayCallbackArg built by the
   walker's apply branch and a live InterpreterObject from a fall-back
   inner rerun both arg at depth-marker), and they correctly resolve
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
   ExprFromObject's `outerResolver` field; everything else is by
   reference. */
struct OuterApply
{
    OuterRegistry & registry;
    BridgedThunkCache & bridgedLocals;
    EvalState * outerState;
    std::shared_ptr<Evaluator> innerEvaluator;
    TracingWriter * innerWriter;
    std::shared_ptr<SourceRoot> outerRootFSRoot;
    std::shared_ptr<OuterResolver> resolverHandle;

    std::pair<OuterId, OuterId> run(
        OuterId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope);

    /** Same as run, but the fnObj is provided directly instead of
        being resolved via the registry. Used by makeCachedFnPrimOp's
        applyFn closure when the arg OuterObject itself is being
        applied (= fnId is the arg's state hash, which boundary discipline
        keeps unregistered to avoid sibling collisions; the closure
        captures outerArgObj instead). */
    std::pair<OuterId, OuterId> runOn(
        std::shared_ptr<Object> fnObj, OuterId fnId,
        std::shared_ptr<Object> argObj,
        std::shared_ptr<const ArgCell> callerScope);
};

struct OuterResolver : std::enable_shared_from_this<OuterResolver>
{
    OuterRegistry registry;
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
       ref) so OuterResolver stays default-constructible. */
    std::shared_ptr<SourceRoot> outerRootFSRoot;
    /* Inherited argAncestry (state hash of this cached call's Q) — used by the
       cb-apply boundary to make sibling cached calls' state hashes
       distinct via subject-id inheritance. Zero hash means no
       inheritance (= no argAncestry discrimination). */
    Hash callArgAncestry = Hash(HashAlgorithm::SHA256);

    /* Outer-direction proxies registered live by the ReplayCallbackArg's
       `<replay-local-lambda>` primop (= `registerAmbientResolverProxy`).
       Keyed by `(subject, argAncestry)` so the walker's `resolveStateHash`
       can match the registered arg's subject-id-evolved state hash at any
       walk-edge index, not just the initial one. List rather than
       map because subject equality isn't trivially hashable;
       n_registrations is small (= one per cb-apply boundary the
       primop fires at). */
    struct LiveProxyEntry
    {
        Subject subject;
        Hash argAncestry;
        std::shared_ptr<Object> obj;
    };
    std::vector<LiveProxyEntry> liveProxies;

    OuterQueryResult query(OuterId objectId, const trace::QueryVariant & q)
    {
        return queryOn(registry.resolveOuter(objectId), q);
    }

    OuterQueryResult queryOn(std::shared_ptr<Object> obj, const trace::QueryVariant & q)
    {
        return OuterQuery{registry, innerWriter}.on(std::move(obj), q);
    }

    /** Apply an outer fn (resolved from fnId) to a local argObj.
     *  Returns a pair: (argSubject, resultId). argSubject is the local arg
     *  Hash assigned to argObj; resultId is the producer queryHash
     *  of QueryApply{fn=fnId, arg=argSubject}, under which the
     *  resulting Object is registered as an outer value. The
     *  caller (applyFn closure) records the QueryApply Fact with
     *  the same arg id. */
    std::pair<OuterId, OuterId> apply(
        OuterId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return OuterApply{
            registry, bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.run(fnId, std::move(argObj), std::move(callerScope));
    }

    /** Apply variant where fnObj is provided directly (= callers
        with a captured reference, like makeCachedFnPrimOp's
        applyFn closure for arg-self applies). */
    std::pair<OuterId, OuterId> applyOn(
        std::shared_ptr<Object> fnObj, OuterId fnId,
        std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return OuterApply{
            registry, bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.runOn(std::move(fnObj), fnId, std::move(argObj), std::move(callerScope));
    }
};

std::pair<OuterId, OuterId> OuterApply::run(
    OuterId fnId, std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
{
    auto fnObj = registry.resolveOuter(fnId);
    return runOn(std::move(fnObj), fnId, std::move(argObj), std::move(callerScope));
}

std::pair<OuterId, OuterId> OuterApply::runOn(
    std::shared_ptr<Object> fnObj, OuterId fnId,
    std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
{
    if (!outerState)
        throw Error("ambient apply requires outerState");

    /* Scope-graph cell for the cb arg, rooted at the caller's
       effective argAncestry (which OuterObject::queryApply passes in
       because a resolved fn may be an InterpreterObject without a
       proxy parent chain). The cell carries only topology. */
    auto localCell = ArgCell::make(callerScope, argObj);
    /* Each new value that crosses INTO a cb-apply boundary is
       treated uniformly as a value — no inherited Subject is
       propagated. Identity at this boundary starts fresh as
       Arg at the apply's static (reverse-De-Bruijn)
       depth; the body's own observations on the arg evolve the state hash
       within the Asks structure. This keeps observations at the
       boundary maximally predictable — two cb calls observing the
       same way through their args reach the same trie position
       regardless of where the arg's source came from. */
    Subject argSubject{Arg{localCell->depth}};
    /* Sample resolver->callArgAncestry at fire time. TracingEvaluator::apply
       leaves callArgAncestry at the current sibling's siblingScope (no
       restore), so this sample reflects the CURRENT sibling context
       walker is operating under. Do not freeze at closure-creation
       time — the argAncestry evolves, and freezing would emit stale hashes. */
    Hash argAncestry = resolverHandle->callArgAncestry;
    auto argStateHash = stateHashAfter(argSubject, argAncestry, {});
    tracingCacheLog("OuterApply::run: argAncestry=%s argStateHash=%s",
                    argAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
                    argStateHash.to_string(HashFormat::Base16, false).substr(0, 12));

    /* Compute the resultId early so we can pass it to the
       TracingCallbackArg as ambientApplyId — groups all ambient layer facts
       made on this local (and its descendants) into a single
       AmbientAsks edge at flush. */
    auto fnIdStr  = fnId.to_string(HashFormat::Base16, false);
    auto argStateHashStr = argStateHash.to_string(HashFormat::Base16, false);

    /* cb-apply boundary: record the apply's synthetic walk-advance
       edge (= ε) now that we have fnIdStr and argStateHashStr. See parallel
       call in TracingEvaluator::apply for the principle. */
    if (innerWriter) {
        nlohmann::json applyQ = trace::QueryApply{fnIdStr, argStateHashStr};
        tracingCacheLog("openApplyBoundary callsite=OuterApply::run fn=%s arg=%s",
                        fnIdStr.substr(0, 12), argStateHashStr.substr(0, 12));
        innerWriter->openApplyBoundary(applyQ);
    }
    trace::QueryApply applyQuery{fnIdStr, argStateHashStr};
    auto resultId = TracingDecisionGraph::computeQueryHash(applyQuery);

    /* Wrap the argObj in TracingCallbackArg so the outer's
       accesses on it during the apply land in the inner trace
       with `from=hex(argSubject)`. Inherit callArgAncestry so sibling cached
       calls' local-args have distinct state hashes.

       Skip the TracingCallbackArg wrap when argObj is a ReplayCallbackArg. At warm
       replay, the ReplayCallbackArg reaching `runOn` already encapsulates
       the recorded contract for the cb-arg crossing — the ReplayCallbackArg's
       primop and its synthetic apply-result handle the per-probe
       AmbientAsks lookups directly. Wrapping the ReplayCallbackArg in TracingCallbackArg
       would (1) add a redundant recording layer with no new
       information to capture (the writer isn't recording here at
       warm) and (2) convert the ReplayCallbackArg's primop into the
       `<cached-fn>(TracingCallbackArg)` cascade that bypasses the design's
       lambda-LO mechanism — exactly the bypass diagnosed in
       `tracing-eval-cache-higher-order-replay.md`. At cold, argObj
       is an `InterpreterObject` of a real inner Value and the cast
       returns null, leaving the TracingCallbackArg wrap path unchanged. */
    auto wrappedArg = (innerWriter && outerRootFSRoot
                       && !dynamic_cast<ReplayCallbackArg *>(argObj.get()))
        ? std::shared_ptr<Object>(std::make_shared<TracingCallbackArg>(
              argObj, argSubject, *innerWriter, ref<SourceRoot>(outerRootFSRoot), localCell,
              resolverHandle->callArgAncestry, resultId))
        : argObj;

    /* Bridge local arg via ExprFromObject. The cache memoises by
       argObj identity so cycle detection sees one Value per logical
       arg (see BridgedThunkCache for why pointer-identity, not argSubject). */
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

    /* Result id is queryHash(QueryApply{fn=fnId, arg=argSubject})
       (already computed above for ambientApplyId plumbing). */
    registry.registerOuterAt(resultId, std::move(resultObj));

    /* Defer the QueryApply Request and the localArg sidecar to the
       writer's flush at logResult. Pool entries land at the natural
       reqHashes (no substitution under the via-Asks design's
       single-edge default).

       The sidecar carries `localType` so the replay-side walker
       can detect non-reconstructible locals (functions) without
       forcing them. ReplayCallbackArg can serve scalar/structural
       responses from InnerValueResponse, but a function local has
       no recorded body to apply against a divergent argument — so
       the walker bails on dispatch in that case and the env layer
       fallback (= live re-eval) handles it. */
    if (innerWriter) {
        nlohmann::json applyJson = applyQuery;
        innerWriter->deferRequest(applyJson);
        nlohmann::json localSidecar = {
            {"kind", "localArg"},
            {"applyResultId", resultId.to_string(HashFormat::Base16, false)},
            /* Depth + argAncestry let the replay-side lambda primop compose
               the synthetic apply-result subject as
               `ApplyResultSubject{fn=this.subject, arg=Arg{depth+1}}`
               with `argAncestry` — matching what the writer's recording
               produced when OuterObject::queryApply built the apply
               result's subject. Without these fields the synthetic
               falls back to PostulatedIdempotentRead encoding which disagrees
               with the recorder's encoding, breaking CAS reads of
               the apply-result observations. */
            {"depth", localCell->depth},
            {"argAncestry", resolverHandle->callArgAncestry.to_string(HashFormat::Base16, false)},
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
        innerWriter->deferRequest(localSidecar, argStateHashStr);
    }

    return {argStateHash, resultId};
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
    std::shared_ptr<Object> fnObj, std::shared_ptr<Evaluator> innerEval, std::shared_ptr<OuterResolver> resolver)
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
                        /* Scope-graph cell for this arg. Parent = the
                           fn proxy's cell (so curried applies chain
                           through depth 0, 1, ... naturally).
                           cell.liveObject is set to the OuterObject
                           we're about to construct (below) so chain
                           navigation returns the proxy. */
                        auto parentCell = effectiveArgCell(*fnObj);
                        auto seedCell = ArgCell::make(parentCell, /*liveObject set below*/ nullptr);
                        /* state hash fix: this arg's Subject is the positional
                           handle at this static apply-stack depth.
                           Sibling cb apply invocations share the same
                           Subject and discriminate via their observation
                           factsets, not via state-creep. */
                        Subject argSubject{Arg{seedCell->depth}};
                        /* Inherit the resolver's callArgAncestry (= state hash(Q)
                           of this cached call). Sibling cached calls
                           with different Qs get distinct rootIds and
                           therefore distinct subject-derived content
                           ids throughout this cb-apply boundary. */
                        Hash callArgAncestry = resolver->callArgAncestry;
                        auto rootId = stateHashAfter(argSubject, callArgAncestry, {});
                        /* Per-apply observation context. Captures the
                           outer's probes on the cb arg as they fire
                           through queryFn; the apply-result wrapper
                           uses these observations to compute its
                           evolved state hash (via subject-id
                           ApplyResultSubject recursion through the
                           arg's evolved state hash). This is what
                           distinguishes sibling apply calls within
                           the same cached call (`inner.f 5` vs
                           `inner.f 2`), per the ambient layer design. */
                        auto applyContext = std::make_shared<ApplyContext>(
                            ApplyContext{argSubject, callArgAncestry, {}});
                        /* Boundary-trace-only discipline: do NOT
                           register outerArgObj under rootId in the
                           shared resolver. Sibling cb apply invocations
                           share the same rootId (= cell.contentId() at
                           apply time = depth marker for empty cell), so
                           a shared registration would last-write-wins
                           and queryFn closures would all resolve to the
                           latest outer arg. Instead each invocation's
                           queryFn captures its own outerArgObj and uses
                           it directly for arg (rootId) queries. */
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        OuterQueryFn queryFn = [resolver, outerArgObj, rootId,
                                                  &innerEnv, applyContext](
                            OuterId objectId,
                            const trace::QueryVariant & q,
                            Subject subject,
                            Hash argAncestry) {
                            /* For cb-arg queries (objectId == this cb's
                               rootId), dispatch on the captured
                               outerArgObj directly — bypass the shared
                               resolver lookup. For derived ids (child
                               objects from earlier getAttr/getListElem),
                               delegate to the resolver which has them
                               registered. */
                            OuterQueryResult qr = (objectId == rootId)
                                ? resolver->queryOn(outerArgObj, q)
                                : resolver->query(objectId, q);
                            innerEnv.outerQuery(
                                q,
                                [&](const trace::QueryVariant &) { return qr.result; },
                                subject,
                                argAncestry);
                            /* Note: queryFn (= cb-arg side) observations
                               are NOT pushed into applyContext.observations.
                               They would be noise from the apply-result
                               wrapper's perspective (their `fromHash` is
                               the cb-arg arg's state hash, not the wrapper's,
                               so the subject-id own-loop on the wrapper
                               doesn't fold them in) but the walk's size
                               growing from these silent pushes would
                               diverge writer (queryFn fires before
                               evolvedQueryFrom because inner.method()
                               is called first) from walker (queryFn
                               fires after evolvedQueryFrom because
                               parentHash must be computed first to
                               build the query). The cb-arg
                               OuterObject's own state hash uses
                               stateHashAfterSubject with empty walk
                               (= content-only) anyway, so dropping
                               these pushes is consistent throughout. */
                            return qr;
                        };
                        /* applyFn does NOT record a QueryApply Fact:
                           a fresh app thunk has no result type
                           ("apply" is not a value type). The
                           apply-result Object is still registered in
                           the resolver under
                           queryHash(QueryApply{fn=fnId, arg=argSubject}),
                           and the QueryApply Request itself is
                           inserted into the pool (see
                           OuterResolver::apply) so downstream
                           Facts with `from=<apply_qH>` can have
                           their response payloads located via the
                           InnerValueResponse on replay. */
                        OuterApplyFn applyFn = [resolver, outerArgObj, rootId](
                            OuterId fnId,
                            std::shared_ptr<Object> argObj,
                            std::shared_ptr<const ArgCell> callerScope) {
                            /* Boundary-trace-only discipline keeps the
                               cb-arg arg unregistered in
                               OuterRegistry. When the SEED ITSELF is
                               applied (= inner does `args 5` on the
                               arg OuterObject), fnId == rootId.
                               `resolver->apply` would try
                               `resolveOuter(rootId)` and throw
                               "unknown value id". Route through
                               `applyOn` with the captured outerArgObj
                               instead — same path as queryFn's
                               `queryOn` shortcut for direct arg
                               queries. */
                            if (fnId == rootId) {
                                auto [argSubject, resultId] = resolver->applyOn(
                                    outerArgObj, fnId, std::move(argObj), std::move(callerScope));
                                return resultId;
                            }
                            auto [argSubject, resultId] = resolver->apply(fnId, std::move(argObj), std::move(callerScope));
                            return resultId;
                        };
                        /* lazy-paths: pin OuterObject's path SourceRoot
                           on the outer EvalState's `rootFSRoot` so the
                           SourceRoot outlives the Values the outer
                           evaluator builds from any returned RootedPaths. */
                        auto contraArg =
                            make_ref<OuterObject>(std::move(argSubject), std::move(queryFn), state.rootFSRoot, std::move(applyFn));
                        /* Wire seedCell.liveObject to contraArg now
                           that it exists. This is the deliberate
                           shared_ptr cycle documented on
                           ArgCell::liveObject. */
                        seedCell->liveObject = contraArg.get_ptr();
                        contraArg->withArgCell(seedCell);
                        contraArg->withInheritedScope(callArgAncestry);
                        contraArg->withApplyContext(applyContext);
                        tracingCacheLog("makeCachedFnPrimOp.impl: contraArg=%p seedCell=%p callArgAncestry=%s outerArg=%p",
                                        (void*)contraArg.get_ptr().get(), (void*)seedCell.get(),
                                        callArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
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
 * Calls dispatch through OuterObject::queryApply without an inner evaluator.
 */
static PrimOp * makeOuterFnPrimOp(std::shared_ptr<Object> fnObj, std::shared_ptr<OuterResolver> resolver)
{
    return new
#if NIX_USE_BOEHMGC
        (GC)
#endif
            PrimOp{
                .name = "<outer-fn>",
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
            auto * expr = new ExprFromObjectAttr(obj, name, innerEvaluator, outerResolver);
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
            auto childExpr = new ExprFromObject(std::move(childObj), innerEvaluator, outerResolver);
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
        /* Dispatch on obj's dynamic type. An OuterObject wraps an
           outer value reached via ambient query; its apply must
           route through queryApply (makeOuterFnPrimOp). A concrete
           fn with an inner evaluator goes through innerEval->apply
           (makeCachedFnPrimOp). A concrete fn without an inner
           evaluator falls back to makeOuterFnPrimOp — the impl
           will throw at apply time (matching the prior behaviour
           for that combination, which the unit tests rely on for
           construction-only checks). */
        /* ReplayCallbackArg reconstructs a lambda LocalObject as a
           primop via its own `toValueOrProxy` (= the
           <replay-local-lambda> mechanism in
           replay-callback-arg.cc). Use it directly so the recorded
           ambient chain drives apply-time behaviour; the generic
           cached/ambient primops here would dispatch on
           `ReplayCallbackArg::queryApply` which throws by design. */
        if (dynamic_cast<ReplayCallbackArg *>(obj.get())) {
            auto val = obj->toValueOrProxy(state, outerResolver);
            v = **val;
            break;
        }
        PrimOp * primOp;
        if (dynamic_cast<OuterObject *>(obj.get())) {
            primOp = makeOuterFnPrimOp(obj, outerResolver);
        } else if (innerEvaluator) {
            primOp = makeCachedFnPrimOp(obj, innerEvaluator, outerResolver);
        } else {
            primOp = makeOuterFnPrimOp(obj, outerResolver);
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
    ExprFromObject(std::move(childObj), innerEvaluator, outerResolver).eval(state, env, v);
}

std::shared_ptr<OuterResolver> makeAmbientResolver(
    EvalState * outerState, std::shared_ptr<Evaluator> innerEvaluator, TracingWriter * innerWriter)
{
    auto resolver = std::make_shared<OuterResolver>();
    resolver->outerState = outerState;
    resolver->innerEvaluator = std::move(innerEvaluator);
    resolver->innerWriter = innerWriter;
    if (outerState)
        resolver->outerRootFSRoot = outerState->rootFSRoot.get_ptr();
    return resolver;
}

void setAmbientResolverCallArgAncestry(OuterResolver & resolver, Hash callArgAncestry)
{
    resolver.callArgAncestry = std::move(callArgAncestry);
}

Hash getAmbientResolverCallScope(const OuterResolver & resolver)
{
    return resolver.callArgAncestry;
}

void registerAmbientResolverProxy(
    OuterResolver & resolver,
    Subject subject,
    Hash argAncestry,
    std::shared_ptr<Object> obj)
{
    /* Overwrite-on-conflict for the same (subject, argAncestry) key. The
       primop fires once per cb-apply boundary it covers; re-firing
       with the same args produces the same registration. Different
       boundaries register different subjects (= different
       `applyDepth+1` values), so collisions across boundaries
       within one cache call are non-existent unless sibling
       cb-applies share the same cb-arg arg depth — same
       boundary-trace-only caveat as the previous state hash-keyed version.

       `Subject` has no `operator==`; the primop only ever
       registers `Arg{depth}` here, so structural
       equality reduces to comparing the depth field. Asserting on
       the variant tag keeps this collapse honest if a future caller
       passes a different variant. */
    auto * newSeed = std::get_if<Arg>(&subject.data);
    assert(newSeed && "registerAmbientResolverProxy: subject must be a Arg");
    for (auto & entry : resolver.liveProxies) {
        auto * existingSeed = std::get_if<Arg>(&entry.subject.data);
        if (existingSeed && existingSeed->depth == newSeed->depth && entry.argAncestry == argAncestry) {
            entry.obj = std::move(obj);
            return;
        }
    }
    resolver.liveProxies.push_back({std::move(subject), std::move(argAncestry), std::move(obj)});
}

std::shared_ptr<Object> tryResolveAmbientResolverProxy(
    OuterResolver & resolver,
    const Hash & idHash,
    const std::vector<ObservationSet> & envWalk,
    TracingDecisionGraph * dg)
{
    /* Linear scan over each registered (subject, argAncestry) x K in
       envWalk. The hasSubjectStampSite gate turned out to be a
       tautology (cold stamped every CID walker ever resolves), so
       running the scan unconditionally is equivalent. */
    (void) dg;
    for (auto & entry : resolver.liveProxies) {
        for (size_t k = 0; k <= envWalk.size(); ++k) {
            auto stateHash = stateHashAt(entry.subject, entry.argAncestry, envWalk, k);
            if (stateHash == idHash)
                return entry.obj;
        }
    }
    return nullptr;
}

} // namespace nix

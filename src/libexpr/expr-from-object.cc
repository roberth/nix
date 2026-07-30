#include "nix/expr/expr-from-object.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/observation-set.hh"
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

/* Dispatch a query on the given outer Object directly. Returns the
   response plus (for producer queries) the outer's child Object at
   the queried position. No lookup table, no id round-trip — the
   caller passes the outer Object it already holds. */
static OuterQueryResult dispatchOuterQuery(std::shared_ptr<Object> obj, const trace::SelectorVariant & q)
{
    return std::visit(
        [&](const auto & query) -> OuterQueryResult {
            using Q = std::decay_t<decltype(query)>;
            if constexpr (std::is_same_v<Q, trace::SelectorApply>
                          || std::is_same_v<Q, trace::SelectorCallbackApply>) {
                /* Identity case — OuterObject::whnf dispatches with
                   q=producer, which for callback-applyResult wrappers is
                   SelectorCallbackApply; for other applies it's
                   SelectorApply. obj IS the applyResult; return its WHNF. */
                return {computeWHNFFromObject(*obj), nullptr};
            } else if constexpr (std::is_same_v<Q, trace::SelectorArg>) {
                /* #186: SelectorArg used as identity of the outer arg
                   itself — return its WHNF (no navigation). */
                return {computeWHNFFromObject(*obj), nullptr};
            } else if constexpr (!requires { query.from; }) {
                throw Error("outer query: query type has no 'from' field");
            } else if constexpr (std::is_same_v<Q, trace::SelectorGetAttr>) {
                /* Pure retrieval — assumes existence (caller must
                   have projected membership from parent WHNFAttrs). */
                auto child = obj->maybeGetAttr(query.name);
                if (!child)
                    throw Error("outer getAttr: attr '%s' unexpectedly missing", query.name);
                return {computeWHNFFromObject(*child), std::move(child)};
            } else if constexpr (std::is_same_v<Q, trace::SelectorGetListElem>) {
                auto child = obj->getListElem(query.index);
                return {computeWHNFFromObject(*child), std::move(child)};
            } else if constexpr (std::is_same_v<Q, trace::SelectorGetFunctionInfo>) {
                auto info = obj->getFunctionInfo();
                if (!info)
                    return {trace::ResultFunctionInfo{false, {}, false}, nullptr};
                return {trace::ResultFunctionInfo{true, info->formals, info->ellipsis}, nullptr};
            } else {
                throw Error("unsupported outer query type");
            }
        },
        q);
}

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

/* Orchestrates a covariant-callback apply: opens a cell for the
   inner-supplied arg, wraps the arg in TracingCallbackArg so outer
   accesses on it land in the inner trace, bridges the wrapped arg
   via ExprFromObject into an outer `mkApp` thunk, and defers the
   apply Request + localArg sidecar to the writer's flush.
   Constructed transiently per call; holds refs/copies from the owning
   resolver. The `resolverHandle` shared_ptr is required for
   ExprFromObject's `outerResolver` field; everything else is by
   reference. */
struct OuterApply
{
    BridgedThunkCache & bridgedLocals;
    EvalState * outerState;
    std::shared_ptr<Evaluator> innerEvaluator;
    TracingWriter * innerWriter;
    std::shared_ptr<SourceRoot> outerRootFSRoot;
    std::shared_ptr<OuterResolver> resolverHandle;

    /** Invoke `fnObj` on `argObj`. Returns the raw outer apply-result
        Object plus a producer callable for the wrapping OuterObject.
        For a callback firing the callable synthesises
        `SelectorCallbackApply{fn=<fn hex>, argObsSet=<snapshot hash>}`
        each time it's called, so probes at different moments sample
        the current runningObsSet. */
    OuterApplyResult run(
        std::shared_ptr<Object> fnObj, ref<const trace::Selector> fnProducer,
        std::shared_ptr<Object> argObj,
        std::shared_ptr<const ArgCell> applyCell);
};

struct OuterResolver : std::enable_shared_from_this<OuterResolver>
{
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

    /* Outer-direction proxies registered live by the ReplayCallbackArg's
       `<replay-local-lambda>` primop (= `registerOuterResolverProxy`).
       Keyed by producer content hash so the walker's `resolveIdentity`
       can match the registered arg by its state-hash-hex. */
    struct LiveProxyEntry
    {
        trace::SelectorVariant producer;
        std::shared_ptr<Object> obj;
    };
    std::vector<LiveProxyEntry> liveProxies;

    /** Invoke the outer fn Object `fnObj` on `argObj`. Returns the raw
        outer apply-result plus a producer callable for the wrapping
        OuterObject. */
    OuterApplyResult apply(
        std::shared_ptr<Object> fnObj, ref<const trace::Selector> fnProducer,
        std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return OuterApply{
            bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.run(std::move(fnObj), std::move(fnProducer),
              std::move(argObj), std::move(callerScope));
    }
};

OuterApplyResult OuterApply::run(
    std::shared_ptr<Object> fnObj, ref<const trace::Selector> fnProducer,
    std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> applyCell)
{
    auto fnId = fnProducer->cachedHash;
    if (!outerState)
        throw Error("outer apply requires outerState");

    /* applyCell is the one cell for this apply — created by
       OuterObject::queryApply. `callbackState` populated below marks
       it as a callback firing; the producer callable returned to the
       caller reads runningObsSet from it on demand. */
    auto localCell = applyCell;
    /* Each new value that crosses INTO a cb-apply is
       treated uniformly as a value — no inherited Subject is
       propagated. Identity at this boundary starts fresh as
       Arg at the apply's static (reverse-De-Bruijn)
       depth; the body's own observations on the arg evolve the state hash
       within the Asks structure. This keeps observations at the
       boundary maximally predictable — two cb calls observing the
       same way through their args reach the same trie position
       regardless of where the arg's source came from. */
    /* Contra-arg identity is a hardcoded sentinel — the enclosing
       SelectorCallbackApply already scopes "which firing's contra-arg
       this is", so no cell-topology encoding is needed here. Writer
       and reader (replay-callback-arg.cc, tracing-replay-evaluator.cc)
       agree on the same constant. */
    trace::SelectorArg argProducer{0};
    auto argStateHash = TracingDecisionGraph::computeSelectorHash(argProducer);
    tracingCacheLog("OuterApply::run: argStateHash=%s",
                    argStateHash.to_string(HashFormat::Base16, false).substr(0, 12));

    auto fnIdStr  = fnId.to_string(HashFormat::Base16, false);
    auto argStateHashStr = argStateHash.to_string(HashFormat::Base16, false);

    /* Intern SelectorApply{parent=fnProducer} — the apply's identity. */
    auto & pool = innerWriter->getDecisionGraph()->selectorPool;
    auto applySel = pool.intern(trace::SelectorApply{fnProducer});
    if (innerWriter) {
        nlohmann::json applyQ = trace::toJson(*applySel);
        tracingCacheLog("createCallbackCell callsite=OuterApply::run fn=%s arg=%s",
                        fnIdStr.substr(0, 12), argStateHashStr.substr(0, 12));
        innerWriter->createCallbackCell(applyQ);
        /* Phase D2 companion: populate the callback state on the
           cell (localCell) so the contra-arg observations that
           TracingCallbackArg::recordObservation will accumulate can be
           reached via the cell chain, not just via the writer-owned
           callbackCells vector. Applies to `localCell` directly since
           this cell IS the callback firing's arg cell. */
        localCell->callbackState = std::make_shared<CallbackState>();
        localCell->callbackState->fnStateHashHex = fnIdStr;
        /* Walker-side pre-population: when the arg is a ReplayCallbackArg
           (walker-materialised callback firing), pre-populate the
           runningObsSet with the ReplayCallbackArg's obsSetResponses.
           Otherwise the fresh callback cell would snapshot obs=empty at
           every probe, producing facts that don't match cold's
           recording (where cold's runningObsSet grew via
           TracingCallbackArg::recordObservation as the fn body probed
           the contra-arg). Match cold's snapshot moment by starting
           with the recorded obs. */
        if (auto * rca = dynamic_cast<ReplayCallbackArg *>(argObj.get())) {
            if (auto obsMap = rca->getObsSetResponses()) {
                for (const auto & [selectorHash, responsePayload] : *obsMap) {
                    localCell->callbackState->runningObsSet.push_back(
                        {selectorHash, responsePayload});
                }
                tracingCacheLog(
                    "OuterApply::run: pre-populated runningObsSet with %zu entries from ReplayCallbackArg",
                    obsMap->size());
            }
        }
    }

    /* Wrap the argObj in TracingCallbackArg so the outer's
       accesses on it during the apply land in the inner trace
       with `from=hex(argSubject)`.

       Skip the TracingCallbackArg wrap when argObj is a ReplayCallbackArg.
       At warm replay the ReplayCallbackArg reaching `runOn` already
       encapsulates the recorded contract for the cb-arg crossing — its
       primop and its synthetic apply-result serve probes from the
       recorded obsSet directly. Wrapping the ReplayCallbackArg in
       TracingCallbackArg would (1) add a redundant recording layer with
       no new information to capture (the writer isn't recording here at
       warm) and (2) convert the ReplayCallbackArg's primop into the
       `<cached-fn>(TracingCallbackArg)` cascade that bypasses the
       callback-arg-lambda-primop-fires discipline (see
       tracing-eval-cache-primop.md's "The callback-arg-lambda primop
       must fire when the outer applies it"). At
       cold, argObj is an `InterpreterObject` of a real inner Value and
       the cast returns null, leaving the TracingCallbackArg wrap path
       unchanged. */
    auto argProducerSel = innerWriter
        ? innerWriter->getDecisionGraph()->selectorPool.intern(argProducer)
        : ref<const trace::Selector>(std::make_shared<const trace::Selector>(trace::SelectorVariant{argProducer}));
    auto wrappedArg = (innerWriter && outerRootFSRoot
                       && !dynamic_cast<ReplayCallbackArg *>(argObj.get()))
        ? std::shared_ptr<Object>(std::make_shared<TracingCallbackArg>(
              argObj, argProducerSel, *innerWriter,
              ref<SourceRoot>(outerRootFSRoot), localCell))
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

    /* Defer the SelectorApply Request to the writer's flush. */
    if (innerWriter) {
        nlohmann::json applyJson = trace::toJson(*applySel);
        innerWriter->deferRequest(applyJson);
    }

    /* Build the producer callable for the wrapping OuterObject.
       For a callback firing (innerWriter present, localCell has
       callbackState populated above), the callable synthesises
       `SelectorCallbackApply{fn=<fn hex>, argObsSet=<snapshot hash>}`
       from localCell.callbackState.runningObsSet on demand — probes
       at different moments produce distinct compositional Selectors
       per §7 of the callback model. For non-callback applies (no
       innerWriter), falls back to a stable SelectorApply.

       The obsSet snapshot is inserted into the ObservationSet CAS
       (via decisionGraph) so walker's `resolveIdentity` can decode
       references to this producer at replay. */
    std::function<ref<const trace::Selector>()> producerFn;
    if (innerWriter) {
        auto * dg = innerWriter->getDecisionGraph();
        producerFn = [localCell, fnProducer, applySel, dg]() -> ref<const trace::Selector> {
            if (localCell->callbackState && dg) {
                auto obsSetHash = dg->insertObservationSet(
                    localCell->callbackState->runningObsSet);
                /* Look up cs.fnStateHashHex in pool; fallback to fnProducer. */
                ref<const trace::Selector> fnRef = fnProducer;
                try {
                    auto fnHash = Hash::parseNonSRIUnprefixed(
                        localCell->callbackState->fnStateHashHex, HashAlgorithm::SHA256);
                    if (auto found = dg->selectorPool.find(fnHash))
                        fnRef = *found;
                } catch (...) {}
                auto qcaSel = dg->selectorPool.intern(trace::SelectorCallbackApply{
                    obsSetHash.to_string(HashFormat::Base16, false), fnRef});
                nlohmann::json qcaJson = trace::toJson(*qcaSel);
                dg->insertRequest(qcaSel->cachedHash, jsonToCborString(qcaJson));
                return qcaSel;
            }
            return applySel;
        };
    } else {
        producerFn = [applySel]() -> ref<const trace::Selector> { return applySel; };
    }

    return OuterApplyResult{
        .applyResult = resultObj,
        .producerFn = std::move(producerFn),
    };
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
 * Calls route through the inner evaluator, with the OuterResolver
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
                        /* Selector-is-a-sequence: the arg is identified
                           by the apply that scoped it — SelectorApply
                           whose `fn` is fnObj's CURRENT selector hex.
                           Passed as a live callable so the hex is
                           recomputed on demand (fnObj may be a proxy
                           whose identity evolves). */
                        /* Access the pool via innerEval's decision graph if reachable. */
                        trace::SelectorPool * pool = innerEval->getEvalState().rootDecisionGraph
                            ? &innerEval->getEvalState().rootDecisionGraph->selectorPool
                            : nullptr;
                        auto argProducerFn = [pool]() mutable -> ref<const trace::Selector> {
                            static trace::SelectorPool localPool;
                            auto & p = pool ? *pool : localPool;
                            return p.intern(trace::SelectorArg{0});
                        };
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        OuterQueryFn queryFn = [&innerEnv](
                            std::shared_ptr<Object> outerObj,
                            ref<const trace::Selector> q,
                            ref<const trace::Selector> producer,
                            std::shared_ptr<const ArgCell> callerCell) {
                            std::shared_ptr<const ArgCell> attributionCell = callerCell;
                            OuterQueryResult qr = dispatchOuterQuery(std::move(outerObj), q->node);
                            innerEnv.outerQuery(
                                q,
                                [&](ref<const trace::Selector>) { return qr.result; },
                                producer,
                                attributionCell);
                            return qr;
                        };
                        OuterApplyFn applyFn = [resolver](
                            std::shared_ptr<Object> fnObj,
                            ref<const trace::Selector> fnProducer,
                            std::shared_ptr<Object> argObj,
                            std::shared_ptr<const ArgCell> applyCell) -> OuterApplyResult {
                            return resolver->apply(std::move(fnObj), std::move(fnProducer),
                                                    std::move(argObj), std::move(applyCell));
                        };
                        auto outerArgProxy =
                            make_ref<OuterObject>(argProducerFn, outerArgObj, std::move(queryFn), state.rootFSRoot, pool, std::move(applyFn));
                        /* Wire seedCell.liveObject to outerArgProxy now
                           that it exists. This is the deliberate
                           shared_ptr cycle documented on
                           ArgCell::liveObject. */
                        seedCell->liveObject = outerArgProxy.get_ptr();
                        outerArgProxy->withArgCell(seedCell);
                        tracingCacheLog("makeCachedFnPrimOp.impl: outerArgProxy=%p seedCell=%p outerArg=%p",
                                        (void*)outerArgProxy.get_ptr().get(), (void*)seedCell.get(),
                                        (void*)outerArgObj.get());
                        auto result = innerEval->apply(ref<Object>(fnObj), outerArgProxy);
                        tracingCacheLog("makeCachedFnPrimOp.impl: apply result=%p", (void*)result.get_ptr().get());
                        ExprFromObject(result.get_ptr(), innerEval, resolver).eval(state, state.baseEnv, v);
                    },
                .getFunctionInfo = [fnObj]() -> std::optional<FunctionInfo> { return fnObj->getFunctionInfo(); },
            };
}

/**
 * Create a PrimOp for an outer function (from the outer evaluator).
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
           outer value reached via outer query; its apply must
           route through queryApply (makeOuterFnPrimOp). A concrete
           fn with an inner evaluator goes through innerEval->apply
           (makeCachedFnPrimOp). A concrete fn without an inner
           evaluator falls back to makeOuterFnPrimOp — the impl
           will throw at apply time (matching the prior behaviour
           for that combination, which the unit tests rely on for
           construction-only checks). */
        /* ReplayCallbackArg reconstructs a lambda callback arg as a
           primop via its own `toValueOrProxy`. Use it directly so
           the recorded obsSet drives apply-time behaviour; the
           generic cached/outer primops here would dispatch on
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

std::shared_ptr<OuterResolver> makeOuterResolver(
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

void registerOuterResolverProxy(
    OuterResolver & resolver,
    trace::SelectorVariant producer,
    std::shared_ptr<Object> obj)
{
    /* Overwrite-on-conflict for the same producer key. The primop
       only ever registers `SelectorArg{depth}` here, so structural
       equality reduces to comparing the depth field. Asserting on the
       variant tag keeps this collapse honest if a future caller passes
       a different variant. */
    auto * newSeed = std::get_if<trace::SelectorArg>(&producer);
    assert(newSeed && "registerOuterResolverProxy: producer must be a SelectorArg");
    for (auto & entry : resolver.liveProxies) {
        auto * existingSeed = std::get_if<trace::SelectorArg>(&entry.producer);
        if (existingSeed && existingSeed->depth == newSeed->depth) {
            entry.obj = std::move(obj);
            return;
        }
    }
    resolver.liveProxies.push_back({std::move(producer), std::move(obj)});
}

std::shared_ptr<Object> tryResolveOuterResolverProxy(
    OuterResolver & resolver,
    const Hash & idHash,
    TracingDecisionGraph * dg)
{
    (void) dg;
    for (auto & entry : resolver.liveProxies) {
        auto stateHash = TracingDecisionGraph::computeSelectorHash(entry.producer);
        if (stateHash == idHash)
            return entry.obj;
    }
    return nullptr;
}

} // namespace nix

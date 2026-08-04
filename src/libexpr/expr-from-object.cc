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
OuterQueryResult dispatchOuterQuery(std::shared_ptr<Object> obj, const trace::SelectorNode & q)
{
    /* Each Selector alternative gets its own handler — no field-presence
       shortcuts. A new alternative added to SelectorNode won't compile
       without an explicit case here. */
    auto identityWHNF = [&]() -> OuterQueryResult {
        return {computeWHNFFromObject(*obj), nullptr};
    };
    return std::visit(overloaded{
        [&](const trace::SelectorApply &)         -> OuterQueryResult { return identityWHNF(); },
        [&](const trace::SelectorCallbackApply &) -> OuterQueryResult { return identityWHNF(); },
        [&](const trace::SelectorArg &)           -> OuterQueryResult { return identityWHNF(); },
        [&](const trace::SelectorGetAttr & query) -> OuterQueryResult {
            auto child = obj->maybeGetAttr(query.name);
            if (!child)
                /* dispatchOuterQuery on SelectorGetAttr is only reached
                   after the caller (OuterObject::maybeGetAttr) projected
                   membership from parent's WHNFAttrs.names — so missing
                   here is a contradiction between projection and
                   retrieval. */
                panic("dispatchOuterQuery: SelectorGetAttr child missing after membership projection");
            return {computeWHNFFromObject(*child), std::move(child)};
        },
        [&](const trace::SelectorGetListElem & query) -> OuterQueryResult {
            auto child = obj->getListElem(query.index);
            return {computeWHNFFromObject(*child), std::move(child)};
        },
        [&](const trace::SelectorGetFunctionInfo &) -> OuterQueryResult {
            auto info = obj->getFunctionInfo();
            if (!info)
                return {trace::ResultFunctionInfo{false, {}, false}, nullptr};
            return {trace::ResultFunctionInfo{true, info->formals, info->ellipsis}, nullptr};
        },
        [&](const trace::SelectorExpr &)   -> OuterQueryResult { panic("dispatchOuterQuery: SelectorExpr is a root Selector, not routable through outer probes"); },
        [&](const trace::SelectorImport &) -> OuterQueryResult { panic("dispatchOuterQuery: SelectorImport is a root Selector, not routable through outer probes"); },
    }, q);
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
        the current runningObsSet.

        `callerScope` is the caller's cell — the apply's own cell is
        created here (Callback if `innerWriter` is set, Regular
        otherwise) parented to `callerScope`. */
    OuterApplyResult run(
        std::shared_ptr<Object> fnObj, ref<const trace::Selector> fnProducer,
        std::shared_ptr<Object> argObj,
        std::shared_ptr<ArgCell> callerScope);
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

    /** Invoke the outer fn Object `fnObj` on `argObj`. Returns the raw
        outer apply-result plus a producer callable for the wrapping
        OuterObject. */
    OuterApplyResult apply(
        std::shared_ptr<Object> fnObj, ref<const trace::Selector> fnProducer,
        std::shared_ptr<Object> argObj, std::shared_ptr<ArgCell> callerScope)
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
    std::shared_ptr<Object> argObj, std::shared_ptr<ArgCell> callerScope)
{
    auto fnId = fnProducer->cachedHash;
    auto fnIdStr = fnId.toHex();
    if (!outerState)
        throw Error("outer apply requires outerState");

    /* The apply's own cell. Three shapes:
       - Pure outer apply (no innerWriter): a RegularArgCell — no
         callback firing state at all.
       - Cold callback firing (innerWriter, arg is a live inner Value):
         a RecordingCallbackArgCell — TracingCallbackArg wraps the arg
         and appends into its runningObsSet as the outer probes it.
       - Warm callback firing (innerWriter, arg is a ReplayCallbackArg):
         a ReplayCallbackArgCell — the RCA's recorded obsSet hydrates
         the cell up-front, so warm's QCA hash equals cold's without
         any live accumulation. No TCA wrap in this branch — the RCA
         already encapsulates the recorded contract.

       The `recordingCell` local is set only in the cold-callback
       branch; TCA-wrap and the recording-side producer capture use it. */
    std::shared_ptr<ArgCell> localCell;
    std::shared_ptr<RecordingCallbackArgCell> recordingCell;
    if (!innerWriter) {
        localCell = RegularArgCell::make(callerScope, argObj);
    } else if (auto * rca = argObj->asReplayCallbackArg()) {
        std::vector<TracingDecisionGraph::InlineFact> recordedObs;
        if (auto obsMap = rca->getObsSetResponses()) {
            recordedObs.reserve(obsMap->size());
            for (const auto & [selectorHash, responsePayload] : *obsMap)
                recordedObs.push_back({selectorHash, responsePayload});
        }
        localCell = ReplayCallbackArgCell::make(callerScope, argObj, fnIdStr, std::move(recordedObs));
    } else {
        recordingCell = RecordingCallbackArgCell::make(callerScope, argObj, fnIdStr);
        localCell = recordingCell;
    }
    /* Each new value that crosses INTO a cb-apply is
       treated uniformly as a value — no inherited Subject is
       propagated. Identity at this boundary starts fresh as
       Arg at the apply's static (reverse-De-Bruijn)
       depth; the body's own observations on the arg evolve the state hash
       within the Asks structure. This keeps observations at the
       boundary maximally predictable — two cb calls observing the
       same way through their args reach the same trie position
       regardless of where the arg's source came from. */
    /* Contra-arg identity: SelectorArg{depth} at the firing cell's
       reverse-De-Bruijn depth. Making obsset members globally unique
       across nested firings lets XOR-fold hashes compose without
       collision. Writer and walker agree via localCell.depth ==
       fn.argCell.depth + 1 (see tracing-replay-evaluator.cc's SCA
       branches). */
    trace::SelectorArg argProducer{localCell->depth};
    auto argStateHash = TracingDecisionGraph::computeSelectorHash(argProducer);
    tracingCacheLog("OuterApply::run: argStateHash=%s",
                    argStateHash.toHex().substr(0, 12));

    auto argStateHashStr = argStateHash.toHex();

    /* Intern SelectorApply{parent=fnProducer} — the apply's identity. */
    auto & pool = innerWriter->getDecisionGraph().selectorPool;
    auto applySel = pool.intern(trace::SelectorApply{fnProducer});
    if (innerWriter) {
        nlohmann::json applyQ = trace::toJson(*applySel);
        tracingCacheLog("createCallbackCell callsite=OuterApply::run fn=%s arg=%s",
                        fnIdStr.substr(0, 12), argStateHashStr.substr(0, 12));
        innerWriter->createCallbackCell(applyQ);
    }

    /* TCA wrap is the recording side of contra-arg observation:
       outer's accesses on the wrapped arg during the callback body
       land in `recordingCell->callbackState.runningObsSet` via
       `TracingCallbackArg::recordObservation`. Only fires when we're
       in a cold callback firing (recordingCell set); the warm
       (Replay) branch's cell is already hydrated and the RCA arg
       encapsulates its own contract. */
    auto argProducerSel = innerWriter
        ? innerWriter->getDecisionGraph().selectorPool.intern(argProducer)
        : ref<const trace::Selector>(std::make_shared<const trace::Selector>(trace::SelectorNode{argProducer}));
    auto wrappedArg = (recordingCell && outerRootFSRoot)
        ? std::shared_ptr<Object>(std::make_shared<TracingCallbackArg>(
              ref<Object>(argObj), argProducerSel, *innerWriter,
              ref<SourceRoot>(outerRootFSRoot),
              ref<RecordingCallbackArgCell>(recordingCell)))
        : argObj;

    /* Bridge local arg via ExprFromObject. The cache memoises by
       argObj identity so cycle detection sees one Value per logical
       arg (see BridgedThunkCache for why pointer-identity, not argSubject). */
    auto * argThunk = bridgedLocals.getOrCreate(argObj.get(), [&]() {
        auto * v = outerState->allocValue();
        auto * expr = new ExprFromObject(ref<Object>(wrappedArg), innerEvaluator, resolverHandle);
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
       For a callback firing (innerWriter present), the callable
       synthesises `SelectorCallbackApply{fn, argObsSet=<snapshot hash>}`
       from the callback cell's runningObsSet on demand — cold's cell
       is still accumulating so the snapshot reflects the current
       state; warm's cell is frozen at construction so the snapshot
       reflects the recorded state (matching cold's hash). Read via
       `getCallbackState()` so the same producer works for both
       cold Recording and warm Replay cells. For non-callback applies
       (no innerWriter), falls back to a stable SelectorApply. */
    std::function<ref<const trace::Selector>()> producerFn;
    if (innerWriter) {
        auto & dg = innerWriter->getDecisionGraph();
        producerFn = [localCell, fnProducer, &dg]() -> ref<const trace::Selector> {
            auto * cs = localCell->getCallbackState();
            auto obsSetHash = dg.insertObservationSet(cs->runningObsSet);
            /* Look up cs->initialFnHex in pool; fallback to fnProducer. */
            ref<const trace::Selector> fnRef = fnProducer;
            try {
                auto fnHash = trace::parseTracingHex(cs->initialFnHex);
                if (auto found = dg.selectorPool.find(fnHash))
                    fnRef = *found;
            } catch (...) {}
            auto qcaSel = dg.selectorPool.intern(trace::SelectorCallbackApply{
                obsSetHash, fnRef});
            nlohmann::json qcaJson = trace::toJson(*qcaSel);
            dg.insertRequest(qcaSel->cachedHash, jsonToCborString(qcaJson));
            return qcaSel;
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
void ExprProxy::show(const SymbolTable & /* symbols */, std::ostream & str) const
{
    // Placeholder message doesn't need to resolve any Symbols.
    str << "<proxy>";
}

void ExprProxy::bindVars(EvalState & /* es */, const std::shared_ptr<const StaticEnv> & /* env */)
{
    // No variables to bind — we pull from external sources, not the environment.
}

/**
 * Create a PrimOp for a function defined inside the cache boundary.
 * Calls route through the inner evaluator, with the OuterResolver
 * bridging arguments between outer and inner.
 */
PrimOp * makeCachedFnPrimOp(
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
                    [fnObj, innerEval, resolver](EvalState & state, const PosIdx /* pos */, Value ** args, Value & v) {
                        // Do NOT force args[0] — it may be self-referential.
                        auto outerArgObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                        /* Scope-graph cell for this arg. Parent = the
                           fn proxy's cell (so curried applies chain
                           through depth 0, 1, ... naturally).
                           cell.liveObject is set to the OuterObject
                           we're about to construct (below) so chain
                           navigation returns the proxy.

                           #261: this cell IS the callback firing's arg
                           cell — inner->apply(fnObj, outerArgProxy)
                           reaches TracingEvaluator::apply's non-fnIsTlo
                           branch which requires arg's cell to be a
                           RecordingCallbackArgCell. `initialFnHex` comes from
                           fnObj's Selector, which must exist here (the
                           primop is only synthesised when
                           `obj->getSelector().has_value()`; see
                           `ExprFromObject::eval` dispatch). */
                        auto parentCell = effectiveArgCell(*fnObj);
                        auto fnSelHex = fnObj->getSelectorHashHex().value();
                        auto seedCell = RecordingCallbackArgCell::make(
                            parentCell, /*liveObject set below*/ nullptr, fnSelHex);
                        /* Selector-is-a-sequence: the arg is identified
                           by the apply that scoped it — SelectorApply
                           whose `fn` is fnObj's CURRENT selector hex.
                           Passed as a live callable so the hex is
                           recomputed on demand (fnObj may be a proxy
                           whose identity evolves). */
                        /* Both outer and inner may hold the graph — outer's is set when
                           the outer EvalCommand is running under the tracing-eval-cache
                           OPTION, inner's is set unconditionally by builtins.cache's
                           stack setup. The ExprFromObject::eval dispatch (below) only
                           routes here when one of these is non-null; the reference bind
                           is total under correct routing. */
                        auto & dg = state.rootDecisionGraph
                            ? *state.rootDecisionGraph
                            : *innerEval->getEvalState().rootDecisionGraph;
                        auto & pool = dg.selectorPool;
                        /* Selector-is-a-sequence: the arg is identified by the apply
                           that scoped it — SelectorApply whose `fn` is fnObj's current
                           producer Selector. Live callable so fnObj's identity is
                           recomputed on demand (fnObj may itself be a proxy). The
                           outside/Query surface takes the more descriptive shape;
                           SelectorArg is reserved for INSIDE (contra-arg producer)
                           sites where context is already established by the enclosing
                           callback firing. */
                        auto argProducerFn = [&pool, fnObj]() -> ref<const trace::Selector> {
                            auto fnSel = fnObj->getSelector();
                            return pool.intern(trace::SelectorApply{*fnSel});
                        };
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        OuterQueryFn queryFn = [&innerEnv](
                            std::shared_ptr<Object> outerObj,
                            ref<const trace::Selector> q,
                            ref<const trace::Selector> producer,
                            std::shared_ptr<ArgCell> callerCell) {
                            std::shared_ptr<ArgCell> attributionCell = callerCell;
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
                            std::shared_ptr<ArgCell> callerScope) -> OuterApplyResult {
                            return resolver->apply(std::move(fnObj), std::move(fnProducer),
                                                    std::move(argObj), std::move(callerScope));
                        };
                        auto outerArgProxy =
                            make_ref<OuterObject>(argProducerFn, outerArgObj, std::move(queryFn), state.rootFSRoot, pool, seedCell, std::move(applyFn));
                        /* Wire seedCell.liveObject to outerArgProxy now
                           that it exists. This is the deliberate
                           shared_ptr cycle documented on
                           ArgCell::liveObject. */
                        seedCell->liveObject = outerArgProxy.get_ptr();
                        tracingCacheLog("makeCachedFnPrimOp.impl: outerArgProxy=%p seedCell=%p outerArg=%p",
                                        (void*)outerArgProxy.get_ptr().get(), (void*)seedCell.get(),
                                        (void*)outerArgObj.get());
                        try {
                            auto result = innerEval->apply(ref<Object>(fnObj), outerArgProxy);
                            tracingCacheLog("makeCachedFnPrimOp.impl: apply result=%p", (void*)result.get_ptr().get());
                            ExprFromObject(result, innerEval, resolver).eval(state, state.baseEnv, v);
                        } catch (Error & e) {
                            /* Stamp the trace when a cached-function
                               application fails so the reader knows
                               *which* cache callback firing surfaced
                               the error. `fnObj` is the cached function
                               (the one produced by `builtins.cache`);
                               `<cached-fn>` is our name for its
                               applied form. */
                            e.addTrace(nullptr, HintFmt("while applying a `builtins.cache` result as a function"), TracePrint::Always);
                            throw;
                        }
                    },
                .getFunctionInfo = [fnObj]() -> std::optional<FunctionInfo> { return fnObj->getFunctionInfo(); },
            };
}

/**
 * Create a PrimOp for an outer function (from the outer evaluator).
 * Calls dispatch through OuterObject::queryApply without an inner evaluator.
 */
PrimOp * makeOuterFnPrimOp(std::shared_ptr<Object> fnObj, std::shared_ptr<OuterResolver> resolver)
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
                    [fnObj, resolver](EvalState & state, const PosIdx /* pos */, Value ** args, Value & v) {
                        auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                        auto result = fnObj->queryApply(std::move(argObj));
                        ExprFromObject(ref<Object>(result), nullptr, resolver).eval(state, state.baseEnv, v);
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
            auto childExpr = new ExprFromObject(ref<Object>(std::move(childObj)), innerEvaluator, outerResolver);
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

    case nFunction:
        v = *obj->materialiseAsFunctionValue(state, outerResolver, innerEvaluator);
        break;

    case nExternal:
    case nThunk:
    case nFailed:
        state.error<TypeError>("ExprFromObject: cannot represent type %s", showType(type)).debugThrow();
        break;
    }
}

void ExprFromObjectAttr::eval(EvalState & state, Env & env, Value & v)
{
    try {
        auto childObj = parentObj->maybeGetAttr(name);
        if (!childObj)
            state.error<TypeError>("ExprFromObjectAttr: attribute '%s' missing", name).debugThrow();
        ExprFromObject(ref<Object>(std::move(childObj)), innerEvaluator, outerResolver).eval(state, env, v);
    } catch (Error & e) {
        /* Every navigation across a `builtins.cache` boundary lands
           here; on error, stamp the attr name so the reader knows
           which cache-bridged attribute was being forced when a
           downstream failure (module system, missing attr, infinite
           recursion, …) surfaced. TracePrint::Always so it shows
           even without --show-trace. */
        e.addTrace(nullptr, HintFmt("while forcing cached attribute '%s' across a `builtins.cache` boundary", name), TracePrint::Always);
        throw;
    }
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

} // namespace nix

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
            if constexpr (std::is_same_v<Q, trace::SelectorApply>) {
                /* #185: Identity case — used by OuterObject::whnf when
                   producer is SelectorApply (queryApply-result OuterObject's
                   whnf). obj IS the applyResult; no re-application. */
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

    /** Invoke `fnObj` on `argObj`. `fnObj` is the outer Object to
        apply (passed by the caller who already holds it — no id
        round-trip). `fnProducer` is the calling OuterObject's real
        producer Selector; the SelectorApply payload's `fn` field is
        `hex(computeSelectorHash(fnProducer))`. Returns the outer's
        apply-result Object. */
    std::shared_ptr<Object> run(
        std::shared_ptr<Object> fnObj, trace::SelectorVariant fnProducer,
        std::shared_ptr<Object> argObj,
        std::shared_ptr<const ArgCell> callerScope);
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

    /** Invoke the outer fn Object `fnObj` on `argObj`. `fnProducer`
        is the wrapping OuterObject's real producer Selector. Returns
        the outer's apply-result Object. */
    std::shared_ptr<Object> apply(
        std::shared_ptr<Object> fnObj, trace::SelectorVariant fnProducer,
        std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
    {
        return OuterApply{
            bridgedLocals, outerState, innerEvaluator, innerWriter, outerRootFSRoot,
            shared_from_this(),
        }.run(std::move(fnObj), std::move(fnProducer),
              std::move(argObj), std::move(callerScope));
    }
};

std::shared_ptr<Object> OuterApply::run(
    std::shared_ptr<Object> fnObj, trace::SelectorVariant fnProducer,
    std::shared_ptr<Object> argObj, std::shared_ptr<const ArgCell> callerScope)
{
    /* fnId — the content hash of fn's producer Selector, used for the
       SelectorApply payload's `fn` field. */
    auto fnId = TracingDecisionGraph::computeSelectorHash(fnProducer);
    if (!outerState)
        throw Error("outer apply requires outerState");

    /* Scope-graph cell for the cb arg, rooted at the caller's
       effective argAncestry (which OuterObject::queryApply passes in
       because a resolved fn may be an InterpreterObject without a
       proxy parent chain). The cell carries only topology. */
    auto localCell = ArgCell::make(callerScope, argObj);
    /* Each new value that crosses INTO a cb-apply is
       treated uniformly as a value — no inherited Subject is
       propagated. Identity at this boundary starts fresh as
       Arg at the apply's static (reverse-De-Bruijn)
       depth; the body's own observations on the arg evolve the state hash
       within the Asks structure. This keeps observations at the
       boundary maximally predictable — two cb calls observing the
       same way through their args reach the same trie position
       regardless of where the arg's source came from. */
    trace::SelectorArg argProducer{localCell->depth};
    auto argStateHash = TracingDecisionGraph::computeSelectorHash(argProducer);
    tracingCacheLog("OuterApply::run: argStateHash=%s",
                    argStateHash.to_string(HashFormat::Base16, false).substr(0, 12));

    auto fnIdStr  = fnId.to_string(HashFormat::Base16, false);
    auto argStateHashStr = argStateHash.to_string(HashFormat::Base16, false);

    /* #181: SelectorApply carries fn's Q hash only */
    trace::SelectorApply applyQuery{fnIdStr};
    if (innerWriter) {
        nlohmann::json applyQ = trace::SelectorApply{fnIdStr};
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
    auto wrappedArg = (innerWriter && outerRootFSRoot
                       && !dynamic_cast<ReplayCallbackArg *>(argObj.get()))
        ? std::shared_ptr<Object>(std::make_shared<TracingCallbackArg>(
              argObj, trace::SelectorVariant{argProducer}, *innerWriter,
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

    /* Defer the SelectorApply Request to the writer's flush at
       logResult. Pool entries land at the natural reqHashes. */
    if (innerWriter) {
        nlohmann::json applyJson = applyQuery;
        innerWriter->deferRequest(applyJson);
    }

    /* B7: Wrap the cb-apply result so its whnf fires
       emitCallbackApplyForApplyResult with a producer Selector
       whose `fn` field maps to the CallbackCell we just populated.
       Producer = SelectorApply{fn=hex(computeSelectorHash(fnProducer))}.
       This is the same shape TracingCallbackArg::queryApply and
       TracingEvaluator::apply build for their apply-result wrappers. */
    if (innerWriter) {
        trace::SelectorApply applyProducer{
            TracingDecisionGraph::computeSelectorHash(fnProducer)
                .to_string(HashFormat::Base16, false)};
        auto v = innerWriter->getSink().logSelector(applyQuery);
        TriePosition triePos{
            .resultNodeHash = Hash{HashAlgorithm::SHA256},
            .queryHashStr = fnIdStr,
        };
        auto wrapped = TracingObject::create(
            ref<Object>(resultObj), *innerWriter, v, triePos);
        wrapped->withProducer(trace::SelectorVariant{applyProducer});
        /* Mark as cb-apply root: navigation descendants will inherit
           the producer so their whnf emits QCA (§7). */
        wrapped->withCbApplyOrigin();
        /* #183: thread the callback cell through so emitCallbackApply
           can read its callbackState directly (no LIFO fallback over
           writer.callbackCells). localCell was created just above and
           its callbackState was populated with (applyId, fnStateHashHex)
           for this specific firing. */
        wrapped->withArgCell(localCell);
        return wrapped.get_ptr();
    }

    return resultObj;
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
                        /* state hash fix: this arg's Subject is the positional
                           handle at this static apply-stack depth.
                           Sibling cb apply invocations share the same
                           Subject and discriminate via their observation
                           factsets, not via state-creep. */
                        trace::SelectorArg argProducer{seedCell->depth};
                        auto & innerEnv = *innerEval->getEvalState().environment;
                        /* queryFn: dispatch the query directly on the
                           outer Object the OuterObject was
                           constructed to wrap. No id round-trip, no
                           lookup table — each OuterObject already
                           holds its outerObj, and passes it in. */
                        OuterQueryFn queryFn = [&innerEnv](
                            std::shared_ptr<Object> outerObj,
                            const trace::SelectorVariant & q,
                            trace::SelectorVariant producer) {
                            /* Skip the redundant `innerEnv.outerQuery` when
                               `outerObj` already emits its own recording via
                               a QCA-emitting wrapper. Cold otherwise records
                               two overlapping observations for the same
                               event — a generic whnf-of-arg observation
                               (which warm can't resolve because it has no
                               live contra-arg) and the QCA observation from
                               the wrapper's own whnf. Warm hits the generic
                               one and misses. R2 in status.md flags this
                               redundancy; the QCA alone is what the
                               callback-model design requires. */
                            bool cbApplyOrigin = false;
                            std::shared_ptr<const ArgCell> attributionCell;
                            if (outerObj) {
                                if (auto * to = dynamic_cast<TracingObject *>(outerObj.get()))
                                    cbApplyOrigin = to->isCbApplyOrigin();
                                /* #177 C: this proxy's cell — where its
                                   observations should attribute. */
                                attributionCell = outerObj->getProxyArgCell();
                            }
                            OuterQueryResult qr = dispatchOuterQuery(std::move(outerObj), q);
                            if (!cbApplyOrigin) {
                                innerEnv.outerQuery(
                                    q,
                                    [&](const trace::SelectorVariant &) { return qr.result; },
                                    producer,
                                    attributionCell);
                            }
                            return qr;
                        };
                        /* applyFn: invoke the outer fn on the arg,
                           return the outer's apply-result Object
                           directly. Caller (OuterObject::queryApply)
                           passes fnObj — the outer's fn Object it
                           already holds — plus the wrapping
                           OuterObject's Subject-derived state hash
                           used for the SelectorApply payload. */
                        OuterApplyFn applyFn = [resolver](
                            std::shared_ptr<Object> fnObj,
                            trace::SelectorVariant fnProducer,
                            std::shared_ptr<Object> argObj,
                            std::shared_ptr<const ArgCell> callerScope) {
                            return resolver->apply(std::move(fnObj), std::move(fnProducer),
                                                    std::move(argObj), std::move(callerScope));
                        };
                        /* lazy-paths: pin OuterObject's path SourceRoot
                           on the outer EvalState's `rootFSRoot` so the
                           SourceRoot outlives the Values the outer
                           evaluator builds from any returned RootedPaths. */
                        auto outerArgProxy =
                            make_ref<OuterObject>(trace::SelectorVariant{argProducer}, outerArgObj, std::move(queryFn), state.rootFSRoot, std::move(applyFn));
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

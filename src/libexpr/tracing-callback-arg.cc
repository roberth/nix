#include "nix/expr/tracing-callback-arg.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/primops.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Same-name helper exists in outer-object.cc; both translation
   units are unity-built into libnixexpr, so the helpers must have
   distinct names to avoid ODR collisions. */
static std::string tracingLocalFromOf(OuterId id)
{
    return id.toHex();
}

TracingCallbackArg::TracingCallbackArg(
    ref<Object> inner,
    ref<const trace::Selector> producer_,
    TracingWriter & writer,
    ref<SourceRoot> rootFSRoot,
    ref<RecordingCallbackArgCell> argCell,
    std::optional<trace::ResultWHNF> cachedWHNF_)
    : inner(std::move(inner))
    , producer(std::move(producer_))
    , writer(writer)
    , rootFSRoot(std::move(rootFSRoot))
    , argCell(std::move(argCell))
    , cachedWHNF(std::move(cachedWHNF_))
{
}

std::shared_ptr<Object> TracingCallbackArg::maybeGetAttr(const std::string & name)
{
    /* Existence projects from parent WHNFAttrs.names; only when
       present do we record SelectorGetAttr (retrieval) with child WHNF. */
    auto & w = whnf();
    auto * ap = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!ap)
        /* Not an attrs — delegate so inner throws its
           source-positioned "getAttr on non-set" error. */
        return inner->maybeGetAttr(name);
    if (std::find(ap->names.begin(), ap->names.end(), name) == ap->names.end())
        return nullptr;
    auto child = inner->maybeGetAttr(name);
    if (!child)
        /* WHNF names list said the attr is present but inner
           disagrees — same divergence as TracingObject::maybeGetAttr;
           panic to surface. */
        panic("TracingCallbackArg::maybeGetAttr: WHNF says attr present, inner says missing");
    auto & dg = writer.getDecisionGraph();
    auto querySel = dg.selectorPool.intern(trace::SelectorGetAttr{name, producer});
    recordObservation(querySel, computeWHNFFromObject(*child));
    return std::make_shared<TracingCallbackArg>(
        ref<Object>(std::move(child)), querySel, writer, rootFSRoot, argCell);
}

trace::ResultWHNF & TracingCallbackArg::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto whnfResult = computeWHNFFromObject(*inner);
    /* #186: obsSet entry uses the value's own Selector — SelectorArg
       for a positional callback arg, SelectorGetAttr for a nav
       descendant, etc. The observation IS "this value observed to
       have WHNF X", whose natural Selector is the value's producer. */
    recordObservation(producer, whnfResult);
    cachedWHNF = std::move(whnfResult);
    return *cachedWHNF;
}

std::vector<std::string> TracingCallbackArg::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("tlo getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string TracingCallbackArg::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("tlo getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string TracingCallbackArg::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> TracingCallbackArg::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("tlo getStringWithContext: WHNF payload not string (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath TracingCallbackArg::getPath()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFPath>(&w.payload);
    if (!p)
        throw Error("tlo getPath: WHNF payload not path (type %s)", w.type);
    /* lazy-paths: reuse the cached SourceRoot so the path outlives the
       returned RootedPath. */
    return RootedPath{rootFSRoot, CanonPath{p->path}};
}

bool TracingCallbackArg::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("tlo getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt TracingCallbackArg::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("tlo getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat TracingCallbackArg::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("tlo getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t TracingCallbackArg::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("tlo getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> TracingCallbackArg::getListElem(size_t index)
{
    /* Bounds project from parent WHNFList.size; retrieval records
       SelectorGetListElem with child WHNF. */
    auto & w = whnf();
    auto * lp = std::get_if<trace::WHNFList>(&w.payload);
    if (!lp || index >= lp->size)
        /* Not a list, or index out of bounds — delegate so inner
           throws the source-positioned error. */
        return inner->getListElem(index);
    auto child = inner->getListElem(index);
    auto & dg = writer.getDecisionGraph();
    auto querySel = dg.selectorPool.intern(trace::SelectorGetListElem{index, producer});
    recordObservation(querySel, computeWHNFFromObject(*child));
    return std::make_shared<TracingCallbackArg>(
        ref<Object>(std::move(child)), querySel, writer, rootFSRoot, argCell);
}

ObjectType TracingCallbackArg::getTypeLazy()
{
    return getType();
}

ObjectType TracingCallbackArg::getType()
{
    auto type = stringToObjectType(whnf().type);
    tracingCacheLog("tlo: getType from=%s type=%s",
        tracingLocalFromOf(localId()).substr(0, 12),
        objectTypeToString(type));
    return type;
}

RootValue TracingCallbackArg::defeatCache()
{
    /* Function-typed contra-args: since #217, `toValueOrProxy` is the
       supported entry — it returns a primop that delegates to
       TCA::queryApply for recording. `defeatCache` on nFunction TCA
       would hand back the raw lambda and bypass the tracing machinery
       entirely — no coherent story at replay. Panic: any caller that
       reaches here for nFunction is a bug (they should have called
       toValueOrProxy). */
    if (getType() == nFunction)
        panic("TracingCallbackArg::defeatCache: nFunction reached — use toValueOrProxy");
    /* Pass through unrecorded for non-function types. defeatCache yields
       a concrete RootValue (no observable side effects), and there's no
       incoming-Fact shape for "I gave you my underlying value." */
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingCallbackArg::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(), info ? info->formals : std::map<std::string, bool>{}, info ? info->ellipsis : false};
    auto & dg = writer.getDecisionGraph();
    auto qSel = dg.selectorPool.intern(trace::SelectorGetFunctionInfo{producer});
    recordObservation(qSel, rfi);
    return info;
}

PosIdx TracingCallbackArg::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingCallbackArg::getAttrPath()
{
    return inner->getAttrPath();
}

void TracingCallbackArg::recordObservation(ref<const trace::Selector> query, const trace::ResultVariant & result)
{
    auto qh = query->cachedHash;
    tracingCacheLog(
        "TracingCallbackArg::recordObservation: cell=%p appending q=%s",
        (void *) argCell.get(),
        qh.toHex().substr(0, 12).c_str());
    nlohmann::json rJson = std::visit(
        [](const auto & r) -> nlohmann::json { return r; },
        result);
    auto rPayload = jsonToCborString(rJson);
    auto & dg = writer.getDecisionGraph();
    nlohmann::json qJson = trace::toJson(*query);
    dg.insertRequest(qh, jsonToCborString(qJson));
    argCell->callbackState.runningObsSet.push_back({qh, rPayload});
}

std::shared_ptr<Object> TracingCallbackArg::queryApply(std::shared_ptr<Object> argObj)
{
    /* #217 higher-order callback apply, Object-level. Design per
       callback-model §7: outer applying the contra-arg records a
       compositional SelectorCallbackApply on the enclosing
       runningObsSet — fn = this contra-arg's producer, argObsSet =
       hash of inner-lambda's actual probes on the outer-arg.

       Object-level dispatch avoids the K1 recursion hazard —
       Interpreter::apply → toValueOrProxy → primop would re-enter
       this proxy. Recording lives HERE so the primop wrapper stays
       thin (just Value materialisation from the returned Object). */

    auto & dg = writer.getDecisionGraph();

    /* Layer-2 cell for this apply. Parents to self's cell (enclosing
       callback firing's cell). callbackState carries the runningObsSet
       where inner's probes on the outer-arg accumulate — same shape
       OuterApply::run uses for its localCell, mirrored for this
       direction (outer applies inner-fn rather than inner→outer).
       initialFnHex captured at construction (#261). */
    auto layer2Cell = RecordingCallbackArgCell::make(
        argCell.get_ptr(), argObj, producer->cachedHash.toHex());

    /* Producer for the wrapped outer-arg — SelectorArg{depth} at the
       layer-2 firing cell's reverse-De-Bruijn depth. Global uniqueness
       across nested firings lets XOR-fold hashes compose without
       collision. Walker mirrors via fnObj.argCell.depth + 1 (see
       tracing-replay-evaluator.cc). */
    auto argProducerSel = dg.selectorPool.intern(trace::SelectorArg{layer2Cell->depth});

    /* queryFn: execute the probe on the outer-arg live and append
       (Selector, response) to layer2Cell's runningObsSet. Not
       writer.logOuterObservation — that routes to cell.facts for
       factSet composition; we want the snapshot-into-ObservationSet-CAS
       path used by callback firings. */
    OuterQueryFn queryFn = [layer2Cell, &dg](
        std::shared_ptr<Object> outerObj,
        ref<const trace::Selector> q,
        ref<const trace::Selector> /*producer*/,
        std::shared_ptr<ArgCell> /*callerCell*/) {
        auto qr = dispatchOuterQuery(std::move(outerObj), q->node);
        nlohmann::json rJson = std::visit(
            [](const auto & r) -> nlohmann::json { return r; }, qr.result);
        auto rPayload = jsonToCborString(rJson);
        dg.insertRequest(q->cachedHash, jsonToCborString(trace::toJson(*q)));
        layer2Cell->callbackState.runningObsSet.push_back({q->cachedHash, rPayload});
        return qr;
    };

    /* applyFn: nested higher-order apply. Fires when inner-lambda body
       applies wrappedArg further (`wrappedArg X` inside inner's body).
       Per callback-model §7 curried compose: the further apply's
       producer must be a compositional SCA{fn=<enclosing SCA>,
       argObsSet=<layer-N obs>}, not plain SelectorApply — otherwise
       nested applies collapse identities and warm can't discriminate.

       Runs the same shape as TCA::queryApply itself, one layer deeper:
       populate the passed-in applyCell as a callback firing (its own
       callbackState + runningObsSet), wrap argObj2 with a queryFn
       recording to that cell, invoke fnObj->queryApply live, return
       producerFn that snapshots the cell's runningObsSet on demand
       (§7's "at the moment the identity is queried"). Recursive via
       captured `applyFn` shared_ptr so deeper nestings work too. */
    auto applyFn = std::make_shared<OuterApplyFn>();
    auto rootFSRootCopy = rootFSRoot;
    *applyFn = [applyFn, &dg, rootFSRootCopy](
        std::shared_ptr<Object> fnObj,
        ref<const trace::Selector> fnProducer,
        std::shared_ptr<Object> argObj2,
        std::shared_ptr<ArgCell> callerScope) -> OuterApplyResult {
        /* #261: Cell for THIS nested apply — created directly as
           RecordingCallbackArgCell (was previously a Regular cell created by
           OuterObject::queryApply then mutated in-place with
           callbackState). initialFnHex populated at construction. */
        auto nestedCell = RecordingCallbackArgCell::make(
            callerScope, argObj2, fnProducer->cachedHash.toHex());

        auto nestedArgProducerSel = dg.selectorPool.intern(trace::SelectorArg{nestedCell->depth});

        OuterQueryFn nestedQueryFn = [nestedCell, &dg](
            std::shared_ptr<Object> outerObj,
            ref<const trace::Selector> q,
            ref<const trace::Selector> /*producer*/,
            std::shared_ptr<ArgCell> /*callerCell*/) {
            auto qr = dispatchOuterQuery(std::move(outerObj), q->node);
            nlohmann::json rJson = std::visit(
                [](const auto & r) -> nlohmann::json { return r; }, qr.result);
            auto rPayload = jsonToCborString(rJson);
            dg.insertRequest(q->cachedHash, jsonToCborString(trace::toJson(*q)));
            nestedCell->callbackState.runningObsSet.push_back({q->cachedHash, rPayload});
            return qr;
        };

        auto nestedWrappedArg = make_ref<OuterObject>(
            [nestedArgProducerSel]() { return nestedArgProducerSel; },
            argObj2,
            nestedQueryFn,
            rootFSRootCopy,
            dg.selectorPool,
            nestedCell,
            *applyFn);

        auto applyResultObj = fnObj->queryApply(nestedWrappedArg.get_ptr());
        if (!applyResultObj)
            /* Object::queryApply contract: non-null return or throw.
               Null violates the contract — panic. */
            panic("<cb-apply> nested applyFn: queryApply returned null");

        /* producerFn snapshots nestedCell's runningObsSet on demand
           per §7: producer identity is queried at each probe moment,
           so distinct probes at distinct moments produce distinct
           producer Selectors when the obsSet has grown. */
        auto producerFn = [nestedCell, fnProducer, &dg]() -> ref<const trace::Selector> {
            auto obsSetHash = dg.insertObservationSet(
                nestedCell->callbackState.runningObsSet);
            return dg.selectorPool.intern(
                trace::SelectorCallbackApply{obsSetHash, fnProducer});
        };
        return OuterApplyResult{
            .applyResult = std::move(applyResultObj),
            .producerFn = std::move(producerFn),
        };
    };

    auto wrappedArg = make_ref<OuterObject>(
        [argProducerSel]() { return argProducerSel; },
        argObj,
        queryFn,
        rootFSRoot,
        dg.selectorPool,
        layer2Cell,
        *applyFn);

    /* Invoke inner-lambda live via inner->queryApply. Inner's probes
       on wrappedArg flow through queryFn → layer2Cell's runningObsSet.
       Using inner->queryApply (not toValueOrProxy) is what avoids K1. */
    auto resultObj = inner->queryApply(wrappedArg.get_ptr());
    if (!resultObj)
        /* Object::queryApply contract: non-null return or throw. */
        panic("TracingCallbackArg::queryApply: inner returned null");
    auto applyResultWhnf = computeWHNFFromObject(*resultObj);

    /* Snapshot layer-2 obs into ObservationSet CAS and construct the
       compositional SelectorCallbackApply for record on enclosing
       runningObsSet. */
    auto layer2ObsHash = dg.insertObservationSet(
        layer2Cell->callbackState.runningObsSet);
    tracingCacheLog(
        "TCA::queryApply: layer-2 obsSet=%s (%zu probes)",
        layer2ObsHash.toHex().substr(0, 12).c_str(),
        layer2Cell->callbackState.runningObsSet.size());
    auto scaSel = dg.selectorPool.intern(
        trace::SelectorCallbackApply{layer2ObsHash, producer});
    recordObservation(scaSel, applyResultWhnf);

    /* H2: wrap the applyResult in a TCA with producer=scaSel and
       argCell=this->argCell (enclosing firing's cell). Subsequent
       applies on the applyResult then re-enter TCA::queryApply and
       record a compositional SCA{fn=scaSel, ...} on the SAME enclosing
       cell. Same for nav probes on non-function results — they go
       through TCA::maybeGetAttr etc., producing SelectorGetAttr{from=
       scaSel}. This is what makes the callback-model chain inductive:
       one wrap step per apply/getter, no ad-hoc "second apply" case.

       Without this, the applyResult was bare and returned to
       <cb-apply>'s primop impl, which handed it to ExprFromObject —
       which for nFunction values produces a <cached-fn> primop that
       routes the subsequent apply as plain SelectorApply. Warm then
       has no compositional SCA to look up. Doc §6a's "deferred cases"
       note. */
    /* Pre-populate cachedWHNF so wrapper->whnf() returns the value we
       already computed. Otherwise the first probe (typically getType()
       via toValueOrProxy) fires whnf() → recordObservation(scaSel,
       applyResultWhnf) — a second copy of the same Fact we recorded on
       the enclosing cell four lines up. */
    return std::make_shared<TracingCallbackArg>(
        ref<Object>(std::move(resultObj)), scaSel, writer, rootFSRoot, argCell, applyResultWhnf);
}

RootValue TracingCallbackArg::toValueOrProxy(EvalState & evalState, std::shared_ptr<struct OuterResolver> resolver)
{
    /* Non-function values: hand back the underlying Value directly.
       There's nothing higher-order to intercept. */
    if (getType() != nFunction)
        return inner->defeatCache();

    /* Function-typed contra-arg values: return a thin primop that
       delegates to TCA::queryApply — the recording site for the
       higher-order apply. Object-level dispatch (via queryApply)
       avoids the K1 recursion hazard that routing through
       Interpreter::apply would trigger. */
    auto self = std::static_pointer_cast<TracingCallbackArg>(shared_from_this());
    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<cb-apply>",
            .args = {"arg"},
            .arity = 1,
            .impl = [self, resolver](EvalState & state, const PosIdx, Value ** args, Value & v) {
                auto argObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                auto resultObj = self->queryApply(argObj);
                ExprFromObject(ref<Object>(resultObj), nullptr, resolver).eval(state, state.baseEnv, v);
            },
            /* Parallel to makeCachedFnPrimOp / makeOuterFnPrimOp: expose the
               inner value's formal-args so `builtins.functionArgs` reports the
               real formals through the primop wrapper. Without this, patterns
               like `callPackageWith autoArgs fn args` compute
               `intersectAttrs (functionArgs fn) autoArgs` as `{}` because the
               primop has no formals, and downstream `fn allArgs` sees only the
               empty override. */
            .getFunctionInfo = [self]() -> std::optional<FunctionInfo> { return self->getFunctionInfo(); },
        };
    auto * val = evalState.allocValue();
    val->mkPrimOp(primOp);
    return allocRootValue(val);
}

} // namespace nix

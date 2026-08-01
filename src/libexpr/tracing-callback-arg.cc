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
    return id.to_string(HashFormat::Base16, false);
}

TracingCallbackArg::TracingCallbackArg(
    std::shared_ptr<Object> inner,
    ref<const trace::Selector> producer_,
    TracingWriter & writer,
    ref<SourceRoot> rootFSRoot,
    std::shared_ptr<const ArgCell> argCell)
    : inner(std::move(inner))
    , producer(std::move(producer_))
    , writer(writer)
    , rootFSRoot(std::move(rootFSRoot))
    , argCell(std::move(argCell))
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
        return nullptr;
    auto & dg = writer.getDecisionGraph();
    auto querySel = dg.selectorPool.intern(trace::SelectorGetAttr{name, producer});
    recordObservation(querySel, computeWHNFFromObject(*child));
    return std::make_shared<TracingCallbackArg>(
        std::move(child), querySel, writer, rootFSRoot, argCell);
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
        std::move(child), querySel, writer, rootFSRoot, argCell);
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
    /* Reject function-typed contra-arg values: handing back the real
       lambda would let outer's Nix mkApp invoke it directly, bypassing
       the tracing machinery. That path only produces a correct answer
       "by accident" and has no coherent story at replay. */
    if (getType() == nFunction)
        throw Error(
            "tracing eval-cache: applying a function reached through a "
            "callback's contra-arg is not currently supported");
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
    if (!argCell || !argCell->callbackState) {
        tracingCacheLog(
            "TracingCallbackArg::recordObservation: no argCell/callbackState — observation dropped");
        return;
    }
    auto qh = query->cachedHash;
    tracingCacheLog(
        "TracingCallbackArg::recordObservation: cell=%p appending q=%s",
        (void *) argCell.get(),
        qh.to_string(HashFormat::Base16, false).substr(0, 12).c_str());
    nlohmann::json rJson = std::visit(
        [](const auto & r) -> nlohmann::json { return r; },
        result);
    auto rPayload = jsonToCborString(rJson);
    auto & dg = writer.getDecisionGraph();
    nlohmann::json qJson = trace::toJson(*query);
    dg.insertRequest(qh, jsonToCborString(qJson));
    argCell->callbackState->runningObsSet.push_back({qh, rPayload});
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
       direction (outer applies inner-fn rather than inner→outer). */
    auto layer2Cell = ArgCell::make(argCell, nullptr);
    layer2Cell->callbackState = std::make_shared<CallbackState>();
    layer2Cell->callbackState->fnStateHashHex =
        producer->cachedHash.to_string(HashFormat::Base16, false);
    layer2Cell->liveObject = argObj;

    /* Producer for the wrapped outer-arg — SelectorArg{0} scoped by
       the enclosing SelectorCallbackApply constructed below embedding
       the layer-2 argObsSet (§6: contra-arg identity is hardcoded
       sentinel scoped by enclosing SCA). */
    auto argProducerSel = dg.selectorPool.intern(trace::SelectorArg{0});

    /* queryFn: execute the probe on the outer-arg live and append
       (Selector, response) to layer2Cell's runningObsSet. Not
       writer.logOuterObservation — that routes to cell.facts for
       factSet composition; we want the snapshot-into-ObservationSet-CAS
       path used by callback firings. */
    OuterQueryFn queryFn = [layer2Cell, &dg](
        std::shared_ptr<Object> outerObj,
        ref<const trace::Selector> q,
        ref<const trace::Selector> /*producer*/,
        std::shared_ptr<const ArgCell> /*callerCell*/) {
        auto qr = dispatchOuterQuery(std::move(outerObj), q->node);
        nlohmann::json rJson = std::visit(
            [](const auto & r) -> nlohmann::json { return r; }, qr.result);
        auto rPayload = jsonToCborString(rJson);
        dg.insertRequest(q->cachedHash, jsonToCborString(trace::toJson(*q)));
        layer2Cell->callbackState->runningObsSet.push_back({q->cachedHash, rPayload});
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
        std::shared_ptr<const ArgCell> applyCell) -> OuterApplyResult {
        /* Cell for THIS nested apply. applyCell was created by
           OuterObject::queryApply; treat it as writable via
           mutable callbackState (see ArgCell). liveObject also
           mutable-shaped in practice for OuterApply::run's pattern. */
        auto nestedCell = std::const_pointer_cast<ArgCell>(applyCell);
        nestedCell->callbackState = std::make_shared<CallbackState>();
        nestedCell->callbackState->fnStateHashHex =
            fnProducer->cachedHash.to_string(HashFormat::Base16, false);
        nestedCell->liveObject = argObj2;

        auto nestedArgProducerSel = dg.selectorPool.intern(trace::SelectorArg{0});

        OuterQueryFn nestedQueryFn = [nestedCell, &dg](
            std::shared_ptr<Object> outerObj,
            ref<const trace::Selector> q,
            ref<const trace::Selector> /*producer*/,
            std::shared_ptr<const ArgCell> /*callerCell*/) {
            auto qr = dispatchOuterQuery(std::move(outerObj), q->node);
            nlohmann::json rJson = std::visit(
                [](const auto & r) -> nlohmann::json { return r; }, qr.result);
            auto rPayload = jsonToCborString(rJson);
            dg.insertRequest(q->cachedHash, jsonToCborString(trace::toJson(*q)));
            nestedCell->callbackState->runningObsSet.push_back({q->cachedHash, rPayload});
            return qr;
        };

        auto nestedWrappedArg = make_ref<OuterObject>(
            [nestedArgProducerSel]() { return nestedArgProducerSel; },
            argObj2,
            nestedQueryFn,
            rootFSRootCopy,
            dg.selectorPool,
            *applyFn);
        nestedWrappedArg->withArgCell(nestedCell);

        auto applyResultObj = fnObj->queryApply(nestedWrappedArg.get_ptr());
        if (!applyResultObj)
            throw Error("<cb-apply> nested applyFn: queryApply returned null");

        /* producerFn snapshots nestedCell's runningObsSet on demand
           per §7: producer identity is queried at each probe moment,
           so distinct probes at distinct moments produce distinct
           producer Selectors when the obsSet has grown. */
        auto producerFn = [nestedCell, fnProducer, &dg]() -> ref<const trace::Selector> {
            auto obsSetHash = dg.insertObservationSet(
                nestedCell->callbackState->runningObsSet);
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
        *applyFn);
    wrappedArg->withArgCell(layer2Cell);

    /* Invoke inner-lambda live via inner->queryApply. Inner's probes
       on wrappedArg flow through queryFn → layer2Cell's runningObsSet.
       Using inner->queryApply (not toValueOrProxy) is what avoids K1. */
    auto resultObj = inner->queryApply(wrappedArg.get_ptr());
    if (!resultObj)
        throw Error("TracingCallbackArg::queryApply: inner returned null");
    auto applyResultWhnf = computeWHNFFromObject(*resultObj);

    /* Snapshot layer-2 obs into ObservationSet CAS and construct the
       compositional SelectorCallbackApply for record on enclosing
       runningObsSet. */
    auto layer2ObsHash = dg.insertObservationSet(
        layer2Cell->callbackState->runningObsSet);
    tracingCacheLog(
        "TCA::queryApply: layer-2 obsSet=%s (%zu probes)",
        layer2ObsHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
        layer2Cell->callbackState->runningObsSet.size());
    auto scaSel = dg.selectorPool.intern(
        trace::SelectorCallbackApply{layer2ObsHash, producer});
    recordObservation(scaSel, applyResultWhnf);

    return resultObj;
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
                ExprFromObject(resultObj, nullptr, resolver).eval(state, state.baseEnv, v);
            },
        };
    auto * val = evalState.allocValue();
    val->mkPrimOp(primOp);
    return allocRootValue(val);
}

} // namespace nix

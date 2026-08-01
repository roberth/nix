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

std::shared_ptr<Object> TracingCallbackArg::queryApply(std::shared_ptr<Object> /*argObj*/)
{
    /* Applying a callback-produced value (a function reached through
       the contra-arg, e.g. `g.foo 10` where `g` is a callback's
       contra-arg) is not currently supported. */
    throw Error(
        "tracing eval-cache: applying a function reached through a "
        "callback's contra-arg is not currently supported");
}

RootValue TracingCallbackArg::toValueOrProxy(EvalState & evalState, std::shared_ptr<struct OuterResolver> resolver)
{
    /* #217: Higher-order callback apply. Design per callback-model §7:
       when outer applies the contra-arg to some outer-supplied value,
       record a compositional SelectorCallbackApply — fn = this contra-arg's
       producer, argObsSet = inner-lambda's probes on the outer-supplied
       value during this apply. Warm reconstructs the argObsSet by
       replaying recorded probes on the live arg; divergence yields
       different argObsSet → different SelectorCallbackApply hash →
       walker miss. */
    auto self = std::static_pointer_cast<TracingCallbackArg>(shared_from_this());
    auto * primOp = new
#if NIX_USE_BOEHMGC
        (GC)
#endif
        PrimOp{
            .name = "<cb-arg-apply>",
            .args = {"arg"},
            .arity = 1,
            .impl = [self, resolver](EvalState & state, const PosIdx, Value ** args, Value & v) {
                auto & dg = self->writer.getDecisionGraph();

                /* Layer-2 cell for this apply. Parents to self->argCell
                   (the enclosing callback firing's cell). callbackState
                   carries the runningObsSet where inner's probes on the
                   outer-arg accumulate — same shape as OuterApply::run
                   uses for its localCell, mirrored for this direction. */
                auto layer2Cell = ArgCell::make(self->argCell, nullptr);
                layer2Cell->callbackState = std::make_shared<CallbackState>();
                layer2Cell->callbackState->fnStateHashHex =
                    self->producer->cachedHash.to_string(HashFormat::Base16, false);

                /* Producer for the wrapped outer-arg — SelectorArg{0}
                   scoped by the enclosing SelectorCallbackApply
                   (constructed below embedding the layer-2 argObsSet). */
                auto argProducerSel = dg.selectorPool.intern(trace::SelectorArg{0});
                auto outerArgObj = std::make_shared<InterpreterObject>(state, allocRootValue(args[0]));
                layer2Cell->liveObject = outerArgObj;

                /* queryFn: execute the probe on the outer-arg live and
                   append (Selector, response) to layer2Cell's runningObsSet.
                   Not the writer.logOuterObservation path — that goes into
                   cell.facts for factSet composition; we want the
                   snapshot-into-ObservationSet-CAS path used by callback
                   firings (see TracingCallbackArg::recordObservation). */
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

                /* applyFn: fires when inner applies the wrapped outer-arg
                   further (nested higher-order apply). Returns the
                   applyResult and a producerFn — the wrapping OuterObject
                   (constructed by OuterObject::queryApply) carries the
                   same queryFn+applyFn so probes on the applyResult land
                   in layer2Cell's runningObsSet, and further nested applies
                   recurse the same machinery. */
                auto applyFn = std::make_shared<OuterApplyFn>();
                *applyFn = [applyFn, &dg](
                    std::shared_ptr<Object> fnObj,
                    ref<const trace::Selector> fnProducer,
                    std::shared_ptr<Object> argObj,
                    std::shared_ptr<const ArgCell> /*applyCell*/) -> OuterApplyResult {
                    auto applyResultObj = fnObj->queryApply(std::move(argObj));
                    if (!applyResultObj)
                        throw Error("<cb-arg-apply> nested applyFn: queryApply returned null");
                    auto applySel = dg.selectorPool.intern(trace::SelectorApply{fnProducer});
                    return OuterApplyResult{
                        .applyResult = std::move(applyResultObj),
                        .producerFn = [applySel]() { return applySel; },
                    };
                };

                auto wrappedArg = make_ref<OuterObject>(
                    [argProducerSel]() { return argProducerSel; },
                    outerArgObj,
                    queryFn,
                    self->rootFSRoot,
                    dg.selectorPool,
                    *applyFn);
                wrappedArg->withArgCell(layer2Cell);

                /* Invoke inner-lambda live via inner->queryApply. Inner's
                   probes on wrappedArg flow through queryFn → layer2Cell's
                   runningObsSet. */
                auto resultObj = self->inner->queryApply(wrappedArg.get_ptr());
                if (!resultObj)
                    throw Error("TracingCallbackArg::<cb-arg-apply>: queryApply returned null");
                auto applyResultWhnf = computeWHNFFromObject(*resultObj);

                /* Snapshot layer-2 obs into ObservationSet CAS. */
                auto layer2ObsHash = dg.insertObservationSet(
                    layer2Cell->callbackState->runningObsSet);
                tracingCacheLog(
                    "<cb-arg-apply>: layer-2 obsSet=%s (%zu probes)",
                    layer2ObsHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                    layer2Cell->callbackState->runningObsSet.size());

                /* Construct compositional SelectorCallbackApply and
                   record on enclosing runningObsSet. */
                auto scaSel = dg.selectorPool.intern(
                    trace::SelectorCallbackApply{layer2ObsHash, self->producer});
                self->recordObservation(scaSel, applyResultWhnf);

                /* Materialise the result into v via ExprFromObject. */
                ExprFromObject(resultObj, nullptr, resolver).eval(state, state.baseEnv, v);
            },
        };
    auto * val = evalState.allocValue();
    val->mkPrimOp(primOp);
    return allocRootValue(val);
}

} // namespace nix

#include "nix/expr/tracing-object.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/object-type.hh"
#include "nix/util/error.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>

namespace nix {

/* Compute a value's WHNF in one pass by calling the Object's
   per-type getters. Used by wrappers' whnf() when they need to
   materialise a ResultWHNF payload, and by the walker's live
   dispatch to compare against recorded responses. */
trace::ResultWHNF computeWHNFFromObject(Object & obj)
{
    auto type = obj.getType();
    trace::ResultWHNF r;
    r.type = objectTypeToString(type);
    switch (type) {
        case nInt:
            r.payload = trace::WHNFInt{obj.getInt().value};
            break;
        case nFloat:
            r.payload = trace::WHNFFloat{obj.getFloat()};
            break;
        case nBool:
            r.payload = trace::WHNFBool{obj.getBool()};
            break;
        case nString: {
            auto pair = obj.getStringWithContext();
            std::vector<std::string> ctxStrs;
            for (auto & c : pair.second)
                ctxStrs.push_back(c.to_string());
            r.payload = trace::WHNFString{std::move(pair.first), std::move(ctxStrs)};
            break;
        }
        case nPath:
            r.payload = trace::WHNFPath{obj.getPath().path.abs()};
            break;
        case nAttrs:
            r.payload = trace::WHNFAttrs{obj.getAttrNames()};
            break;
        case nList:
            r.payload = trace::WHNFList{obj.getListSize()};
            break;
        case nFunction:
            /* Function identity is represented indirectly through
               subsequent queries (getFunctionInfo, apply,
               callbackApply); the payload is just the tag. */
            r.payload = trace::WHNFFunction{};
            break;
        case nNull:
            r.payload = trace::WHNFNull{};
            break;
        case nThunk:
        case nExternal:
        case nFailed:
            throw Error(
                "cannot record WHNF for %s — not representable in the trace cache; "
                "caller must fall back to the interpreter",
                objectTypeToString(type));
    }
    return r;
}

TracingObject::TracingObject(
    ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos)
    : inner(inner)
    , writer(writer)
    , valueNum(valueNum)
    , triePos(triePos)
{
}

ref<TracingObject> TracingObject::create(
    ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, writer, valueNum, triePos)));
}

/* Parent identity for building child Selectors: `triePos->queryHashStr`
   (parent Q's stable hash) when available; falls back to the trace-only
   decimal ValueHandle for evalExprLazy wrappers without a triePos. */
static std::string parentQOrValueHandle(const std::optional<TriePosition> & triePos, ValueHandle valueNum)
{
    return triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
}

std::optional<ref<const trace::Selector>> TracingObject::getSelector() const
{
    return producer;
}

std::optional<std::string> TracingObject::getProducerSelectorHex(TracingWriter & w)
{
    /* Callback-produced wrapper: identity is a SelectorCallbackApply
       snapshotting the enclosing callback cell's runningObsSet at
       this moment. Insert its payload into the Requests pool so
       walker's resolveIdentity can decode `from` references at
       replay. Not folded as a Fact — the getAttr / apply Selector
       that references it becomes the Fact (callback-model §7). */
    if (cbApplyOrigin && argCell) {
        if (auto * cbState = argCell->getCallbackState()) {
            auto & cs = *cbState;
            auto & dg = w.getDecisionGraph();
            auto obsSetHash = dg.insertObservationSet(cs.runningObsSet);
            /* initialFnHex captures fn's Q-space identity — set from the
               hash of a Selector the writer just interned into the pool
               (TCA::queryApply, OuterApply::run). The pool lookup must
               succeed; a nullopt here means someone populated
               initialFnHex without the corresponding Selector, which is
               a bug in the setter. */
            auto fnRef = dg.selectorPool.findByHex(cs.initialFnHex);
            if (!fnRef)
                panic("TracingObject::getProducerSelectorHex: initialFnHex not in selector pool");
            auto qcaSel = dg.selectorPool.intern(trace::SelectorCallbackApply{
                obsSetHash, *fnRef});
            nlohmann::json qcaJson = trace::toJson(*qcaSel);
            dg.insertRequest(qcaSel->cachedHash, jsonToCborString(qcaJson));
            return qcaSel->cachedHash.toHex();
        }
    }
    /* Under the Selector-is-a-sequence model, the wrapper's producer
       (the SelectorApply value that scoped this apply-result) is a
       legitimate descriptive prefix: children compose `from` = our
       producer hex, meaning "attribute of the applyResult of this
       apply". Fall through to `triePos.queryHashStr` which carries
       the producer hex for apply-result wrappers, or valueNum for
       the evalExprLazy case. */
    return parentQOrValueHandle(triePos, valueNum);
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    /* Existence folds through whnf(): parent's WHNFAttrs.names answers
       "does this attr exist?" for any name — no has-attr observation
       is recorded, and multiple maybeGetAttr calls on the same parent
       share one whnf recording. Only when the attr is known to exist
       do we issue a pure-retrieval SelectorGetAttr, whose result is the
       child's WHNF. */
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        /* Parent isn't an attrs — delegate so inner can throw its
           usual "getAttr on non-set" error with source position. */
        return inner->maybeGetAttr(name);
    if (std::find(p->names.begin(), p->names.end(), name) == p->names.end())
        return nullptr;
    auto innerChild = inner->maybeGetAttr(name);
    if (!innerChild)
        /* WHNF said the attr is present but inner disagrees. Under
           matching-until-divergence, WHNF's names list and inner's
           lookup share a source — divergence here is a bug in whoever
           produced our WHNF or in inner's iteration. */
        panic("TracingObject::maybeGetAttr: WHNF says attr present, inner says missing");
    /* Force child WHNF first — for a callback-produced wrapper, this
       runs the callback body's attribute expression, firing contra-arg
       probes that grow the callback cell's runningObsSet. */
    auto childWHNF = computeWHNFFromObject(*innerChild);
    /* Now build SelectorGetAttr with `from` = our producer at the
       post-force moment. For cbApplyOrigin wrappers this snapshots the
       grown runningObsSet into a SelectorCallbackApply (§7's per-probe
       sampling); non-callback wrappers return their stable Q hex.
       This override of getProducerSelectorHex is total — dereferencing
       enforces that (bad_optional_access if the invariant breaks). */
    auto fromHex = getProducerSelectorHex(writer).value();
    auto & dg = writer.getDecisionGraph();
    /* CODE SMELL (see identical note in getListElem): fromHex may be
       a decimal valueNum, not a Selector hex — findByHex fails and
       we delegate to inner. Real fallback for evalExprLazy wrappers
       without a Selector. */
    auto fromSel = dg.selectorPool.findByHex(fromHex);
    if (!fromSel)
        return innerChild;
    auto querySel = dg.selectorPool.intern(trace::SelectorGetAttr{name, *fromSel});
    auto & query = std::get<trace::SelectorGetAttr>(querySel->node);
    auto queryHash = querySel->cachedHash;
    tracingCacheLog(
        "TO::maybeGetAttr '%s' -> Q=%s (from=%s, cbApplyOrigin=%d)",
        name.c_str(),
        queryHash.toHex().substr(0, 12).c_str(),
        fromHex.substr(0, 12).c_str(),
        (int) cbApplyOrigin);
    /* Phase D2: getter as Query — logQuery/logQueryResult, no push.
       Observations dispatched during innerChild's evaluation
       attribute to the argCell (the enclosing apply/root cell). */
    auto [valueId, qh] = writer.logQuery(query);
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    auto childTriePos = writer.logQueryResult(valueId, childWHNF, qh, anchorCur, argCell);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(innerChild), writer, valueId, childTriePos));
    child->cachedWHNF = std::move(childWHNF);
    child->withArgCell(argCell);
    /* The nav child's producer identity IS SelectorGetAttr{name, parent=self}
       — symmetric to TracingReplayObject::maybeGetAttr's warm-side propagation.
       Set unconditionally so downstream code (ExprFromObject fn dispatch,
       makeCachedFnPrimOp's argProducerFn) has a real Selector to compose. */
    child->withProducer(querySel);
    if (cbApplyOrigin) {
        child->withCbApplyOrigin();
    }
    return child;
}

trace::ResultWHNF & TracingObject::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto whnfResult = computeWHNFFromObject(*inner);
    /* Cell-migration Phase B moved QCA emission from here to
       TracingEvaluator::apply. But TE::apply fires only for the
       OUTER-side apply; the callback firing itself goes through
       OuterApply::run. Re-emit here when the wrapper is a
       callback-origin apply result. */
    if (cbApplyOrigin && producer)
        writer.emitCallbackApplyForApplyResult(argCell, *producer, whnfResult);
    /* #185: no separate whnf Fact — nav descendants + root wrappers
       decode WHNF from triePos.resultNodeHash (parent Selector's
       Terminal). Callback-origin wrappers emit QCA above. */
    cachedWHNF = std::move(whnfResult);
    return *cachedWHNF;
}

std::vector<std::string> TracingObject::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("getAttrNames on non-set value (type %s)", w.type);
    return p->names;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("getStringIgnoreContext on non-string value (type %s)", w.type);
    return p->value;
}

std::string TracingObject::getStringWithoutContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("getStringWithoutContext on non-string value (type %s)", w.type);
    if (!p->context.empty())
        throw Error("string has unexpected context (= %zu elements)", p->context.size());
    return p->value;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("getStringWithContext on non-string value (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath TracingObject::getPath()
{
    /* WHNF records that this is a path; the actual RootedPath needs
       a SourceRoot from inner. */
    whnf();
    return inner->getPath();
}

bool TracingObject::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("getBool on non-bool value (type %s)", w.type);
    return p->value;
}

NixInt TracingObject::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("getInt on non-int value (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat TracingObject::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("getFloat on non-float value (type %s)", w.type);
    return p->value;
}

size_t TracingObject::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("getListSize on non-list value (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> TracingObject::getListElem(size_t index)
{
    /* Bounds fold through whnf(): parent's WHNFList.size answers
       "is this index valid?" for any index — no bounds-check
       observation is recorded. Only the retrieval itself
       (SelectorGetListElem, returning the child's WHNF) is a distinct
       observation. */
    auto & w = whnf();
    auto * lp = std::get_if<trace::WHNFList>(&w.payload);
    if (!lp || index >= lp->size)
        /* Not a list, or index out of bounds — delegate so the
           interpreter throws the source-positioned error. */
        return inner->getListElem(index);
    /* Total on TracingObject; see maybeGetAttr for the reasoning. */
    auto fromHex = getProducerSelectorHex(writer).value();
    auto & dg = writer.getDecisionGraph();
    /* CODE SMELL (audit deferral): fromHex is either a real Selector
       hex OR a decimal valueNum from parentQOrValueHandle for
       evalExprLazy wrappers that never got a Selector. findByHex on
       the decimal never finds anything and we fall back to inner. The
       hex/decimal conflation lives in getProducerSelectorHex — API
       needs sorting out (return an optional Selector directly, not a
       "hex" that isn't always a hex). Not touched in this pass. */
    auto fromSel = dg.selectorPool.findByHex(fromHex);
    if (!fromSel)
        return inner->getListElem(index);
    auto querySel = dg.selectorPool.intern(trace::SelectorGetListElem{index, *fromSel});
    auto & query = std::get<trace::SelectorGetListElem>(querySel->node);
    auto [valueId, qh] = writer.logQuery(query);
    auto result = inner->getListElem(index);
    trace::ResultWHNF childWHNF = computeWHNFFromObject(*result);
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    auto childTriePos = writer.logQueryResult(valueId, childWHNF, qh, anchorCur, argCell);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    child->cachedWHNF = std::move(childWHNF);
    child->withArgCell(argCell);
    /* Mirror maybeGetAttr: the nav child's producer identity IS
       SelectorGetListElem{index, parent=self}. Set unconditionally
       so downstream code (ExprFromObject fn dispatch, TE::apply's
       fn-identity chain) has a real Selector. Previously this was
       gated on cbApplyOrigin and used self's producer (wrong shape),
       leaving non-callback list-element TracingObjects with
       getSelector() = nullopt — which routed nFunction elements to
       makeOuterFnPrimOp's fallback and hit TO::queryApply's identity
       check. */
    child->withProducer(querySel);
    if (cbApplyOrigin) {
        child->withCbApplyOrigin();
    }
    return child;
}

ObjectType TracingObject::getTypeLazy()
{
    auto lazyType = inner->getTypeLazy();
    if (lazyType == nThunk)
        return nThunk;
    return stringToObjectType(whnf().type);
}

ObjectType TracingObject::getType()
{
    return stringToObjectType(whnf().type);
}

RootValue TracingObject::defeatCache()
{
    return inner->defeatCache();
}

Value * TracingObject::maybeMaterialiseAsFunctionValue(
    EvalState & state,
    std::shared_ptr<OuterResolver> resolver,
    std::shared_ptr<Evaluator> innerEvaluator)
{
    /* Need the cache infrastructure to wrap: an inner evaluator, a
       decision graph, and a real producer Selector. Missing any → let
       the caller fall back to the generic outer-fn wrap. */
    auto hasGraph = state.rootDecisionGraph
        || (innerEvaluator && innerEvaluator->getEvalState().rootDecisionGraph);
    if (!innerEvaluator || !hasGraph || !getSelector().has_value())
        return nullptr;
    auto * v = state.allocValue();
    v->mkPrimOp(makeCachedFnPrimOp(
        shared_from_this(), std::move(innerEvaluator), std::move(resolver)));
    return v;
}

std::optional<FunctionInfo> TracingObject::getFunctionInfo()
{
    /* Task #110: push ActiveSelector before forcing, uniform with other
       TracingObject methods (whnf/maybeGetAttr/getListElem/…).
       Whether or not inner->getFunctionInfo() actually fires sub-
       observations, the swap costs at most an extra push/pop and
       eliminates the unverified assumption. */
    /* Total on TracingObject; see maybeGetAttr for the reasoning. */
    auto fromHex = getProducerSelectorHex(writer).value();
    auto & dg = writer.getDecisionGraph();
    /* CODE SMELL (see identical note in getListElem). */
    auto fromSel = dg.selectorPool.findByHex(fromHex);
    if (!fromSel)
        return inner->getFunctionInfo();
    auto querySel = dg.selectorPool.intern(trace::SelectorGetFunctionInfo{*fromSel});
    auto & query = std::get<trace::SelectorGetFunctionInfo>(querySel->node);
    auto [valueId, qh] = writer.logQuery(query);
    auto result = inner->getFunctionInfo();
    trace::ResultFunctionInfo traceResult;
    if (result) {
        traceResult = {.hasInfo = true, .formals = result->formals, .ellipsis = result->ellipsis};
    } else {
        traceResult = {.hasInfo = false};
    }
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    writer.logQueryResult(valueId, traceResult, qh, anchorCur, argCell);
    return result;
}

PosIdx TracingObject::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingObject::getAttrPath()
{
    return inner->getAttrPath();
}

} // namespace nix

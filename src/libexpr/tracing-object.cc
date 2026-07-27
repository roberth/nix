#include "nix/expr/tracing-object.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/object-type.hh"
#include "nix/util/error.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>

namespace nix {

/* Compute a value's WHNF in one pass by calling the Object's
   per-type getters. Used by TracingObject::whnf to record a single
   SelectorGetWHNF observation, and by the walker's dispatch to compute
   the live response for a recorded SelectorGetWHNF. */
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

std::string TracingObject::evolvedQueryFrom() const
{
    /* #178: state-hash evolution retires. Parent identity carried
       via triePos->queryHashStr (parent Q's stable hash). Callback-
       arg path used to fold applyContext observations into a state
       hash; that state hash is now unnecessary since cur at (Q, cur)
       discriminates. */
    return triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
}

void TracingObject::pushObservation(const std::string & fromHex, const Hash & selectorHash, const Hash & responseHash)
{
    if (!applyContext) return;
    Hash fromHash{HashAlgorithm::SHA256};
    try {
        fromHash = Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);
    } catch (...) {
        return;
    }
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), selectorHash, responseHash);
    applyContext->observations.push_back({fromHash, elementHash});
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
        /* WHNF said the attr is present but inner disagrees — shouldn't
           happen under matching-until-divergence. */
        return nullptr;
    auto parentHash = evolvedQueryFrom();
    trace::SelectorGetAttr query{name, parentHash};
    /* Phase D2: getter as Query — logQuery/logQueryResult, no push.
       Observations dispatched during innerChild's evaluation
       attribute to whatever's on activeQueryStack (the enclosing
       apply/root cell), not a getter-specific frame. */
    auto [valueId, qh] = writer.logQuery(query, triePos, argCell);
    auto childWHNF = computeWHNFFromObject(*innerChild);
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    auto childTriePos = writer.logQueryResult(valueId, childWHNF, qh, anchorCur, argCell);
    if (qh.selectorHash && childTriePos)
        pushObservation(parentHash, *qh.selectorHash, childTriePos->resultNodeHash);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(innerChild), writer, valueId, childTriePos));
    child->cachedWHNF = std::move(childWHNF);
    child->withArgCell(argCell);
    if (applyContext) child->withApplyContext(applyContext);
    /* Cb-apply-origin descendants propagate the marks so their whnf
       emits QCA per §7 of the callback model. */
    if (cbApplyOrigin) {
        child->withCbApplyOrigin();
        if (producer)
            child->withProducer(*producer);
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
    /* #185: SelectorGetWHNF emission fully retired. Warm never looks
       these Terminals up:
         - TRO nav descendants + root wrappers have valid
           triePos.resultNodeHash → decode from parent Selector's
           Terminal directly.
         - Warm SelectorCallbackApply dispatch is a live
           fn->queryApply invocation, not a Terminal lookup.
       Recording GetWHNF Terminals wrote unread rows to the DB. */
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
    auto parentHash = evolvedQueryFrom();
    trace::SelectorGetListElem query{parentHash, index};
    /* Phase D2: getter — no push, direct Terminal. */
    auto [valueId, qh] = writer.logQuery(query, triePos, argCell);
    auto result = inner->getListElem(index);
    trace::ResultWHNF childWHNF = computeWHNFFromObject(*result);
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    auto childTriePos = writer.logQueryResult(valueId, childWHNF, qh, anchorCur, argCell);
    if (qh.selectorHash && childTriePos)
        pushObservation(parentHash, *qh.selectorHash, childTriePos->resultNodeHash);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    child->cachedWHNF = std::move(childWHNF);
    child->withArgCell(argCell);
    if (applyContext) child->withApplyContext(applyContext);
    /* B3 / B7 remaining: same cb-apply-origin gating as maybeGetAttr. */
    if (cbApplyOrigin) {
        child->withCbApplyOrigin();
        if (producer)
            child->withProducer(*producer);
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

std::optional<FunctionInfo> TracingObject::getFunctionInfo()
{
    /* Task #110: push ActiveSelector before forcing, uniform with other
       TracingObject methods (whnf/maybeGetAttr/getListElem/…).
       Whether or not inner->getFunctionInfo() actually fires sub-
       observations, the swap costs at most an extra push/pop and
       eliminates the unverified assumption. */
    auto parentHash = evolvedQueryFrom();
    trace::SelectorGetFunctionInfo query{parentHash};
    /* Phase D2: getter — no push, direct Terminal. */
    auto [valueId, qh] = writer.logQuery(query, triePos, argCell);
    auto result = inner->getFunctionInfo();
    trace::ResultFunctionInfo traceResult;
    if (result) {
        traceResult = {.hasInfo = true, .formals = result->formals, .ellipsis = result->ellipsis};
    } else {
        traceResult = {.hasInfo = false};
    }
    auto anchorCur = triePos ? triePos->factSetHash : TracingDecisionGraph::emptySetHash();
    auto tp = writer.logQueryResult(valueId, traceResult, qh, anchorCur, argCell);
    if (qh.selectorHash && tp) pushObservation(parentHash, *qh.selectorHash, tp->resultNodeHash);
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

std::shared_ptr<Object> TracingObject::queryApply(std::shared_ptr<Object> argObj)
{
    /* Object-method counterpart of TracingEvaluator::apply. See
       parallel commentary there for the subject-id routing of the
       apply's triePos and the applyResultSubject attachment. */
    auto fnIdOpt = getSelectorHashHex();
    auto argIdOpt = argObj->getSelectorHashHex();
    if (!fnIdOpt || !argIdOpt)
        throw Error("TracingObject::queryApply: fn/arg lacks a state hash");

    /* cb-apply: record an explicit ε edge for this apply.
       See parallel call in TracingEvaluator::apply. */
    /* #181: SelectorApply carries fn's Q hash only; arg observed by value */
    nlohmann::json applyBoundaryJson = trace::SelectorApply{*fnIdOpt};
    tracingCacheLog("createCallbackCell callsite=TracingObject::queryApply fn=%s arg=%s",
                    fnIdOpt->substr(0, 12), argIdOpt->substr(0, 12));
    writer.createCallbackCell(applyBoundaryJson);

    /* SelectorApply.fn = fn's identity hex. `getSelectorHashHex()` on this
       TracingObject returns the content hash of its stored producer
       when apply-result, or triePos.queryHashStr when non-apply-result.
       Falls back to the raw fnIdOpt for Objects without an internal
       state-hash (shouldn't happen for TracingObject, defensive). */
    auto fnQHex = getSelectorHashHex().value_or(*fnIdOpt);
    trace::SelectorApply resultProducer{fnQHex};

    /* apply-result state hash is content-only — see commentary in
       TracingEvaluator::apply. */
    auto applyArgAncestryStateHash = TracingDecisionGraph::computeSelectorHash(resultProducer);
    auto applyArgAncestryStateHashHex = applyArgAncestryStateHash.to_string(HashFormat::Base16, false);

    /* Record the apply Request payload at the subject-id hash so dispatch
       and the legacy SelectorApply{fn, arg} payload coincide. The legacy
       fnId/argSubject fields remain for the dispatcher's resolveIdentity
       chain. */
    trace::SelectorApply applyQ{*fnIdOpt};
    auto v = writer.getSink().logSelector(applyQ);
    auto result = inner->queryApply(argObj);
    TriePosition applyTriePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyArgAncestryStateHashHex,
    };
    auto child = std::shared_ptr<TracingObject>(
        new TracingObject(ref<Object>(result), writer, v, applyTriePos));
    auto cell = ArgCell::make(argCell, argObj);
    child->withArgCell(std::move(cell));
    child->withProducer(trace::SelectorVariant{std::move(resultProducer)});
    if (auto * argAmb = dynamic_cast<OuterObject *>(argObj.get())) {
        if (auto ctx = argAmb->getApplyContext())
            child->withApplyContext(std::move(ctx));
    } else if (applyContext) {
        child->withApplyContext(applyContext);
    }
    return child;
}

} // namespace nix

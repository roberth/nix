#include "nix/expr/tracing-object.hh"
#include "nix/expr/ambient-object.hh"
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
   QueryGetWHNF observation, and by the walker's dispatch to compute
   the live response for a recorded QueryGetWHNF. */
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
        case nNull:
        case nThunk:
        case nExternal:
        case nFailed:
            r.payload = trace::WHNFEmpty{};
            break;
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
    if (applyResultSubject && applyContext) {
        std::vector<cidasks::Edge> walk;
        walk.reserve(applyContext->observations.size());
        for (auto & obs : applyContext->observations) {
            cidasks::Edge edge;
            edge.observations.push_back(obs);
            walk.push_back(std::move(edge));
        }
        /* Path 3: emit fold-step stamps into SubjectEvolutionEdges
           so walker can navigate subject's evolution edge-by-edge
           rather than iterating K. Uses the subject's Merkle
           content hash as the trie root key. */
        Hash subjectSelfHash = cidasks::scopeStateIdAt(
            *applyResultSubject, Hash(HashAlgorithm::SHA256), {}, 0);
        auto evolved = cidasks::scopeStateIdAtWithHook(
            *applyResultSubject, applyScope, walk, walk.size(),
            [&](const cidasks::EvolutionStep & step) {
                writer.insertSubjectEvolutionEdge(
                    subjectSelfHash, step.curBefore,
                    step.obsFromHash, step.obsElementHash,
                    step.curAfter);
            });
        auto hex = evolved.to_string(HashFormat::Base16, false);
        if (!applyFnIdHex.empty() && !applyArgIdHex.empty()) {
            try {
                auto fnH = Hash::parseNonSRIUnprefixed(applyFnIdHex, HashAlgorithm::SHA256);
                auto argH = Hash::parseNonSRIUnprefixed(applyArgIdHex, HashAlgorithm::SHA256);
                writer.bufferApplyProducer(evolved, fnH, argH);
            } catch (...) {}
        }
        return hex;
    }
    return triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
}

void TracingObject::pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash)
{
    if (!applyContext) return;
    Hash fromHash{HashAlgorithm::SHA256};
    try {
        fromHash = Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);
    } catch (...) {
        return;
    }
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), queryHash, responseHash);
    applyContext->observations.push_back({fromHash, elementHash});
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    auto result = inner->maybeGetAttr(name);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttr query{name, parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    if (result) {
        trace::ResultMaybeType resJson{std::string("deferred")};
        auto childTriePos = writer.logResult(valueId, resJson, qh);
        if (qh.queryHash && childTriePos)
            pushObservation(parentHash, *qh.queryHash, childTriePos->resultNodeHash);
        auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
        child->withScope(argScope);
        if (applyContext) child->withApplyContext(applyContext);
        return child;
    }
    trace::ResultMaybeType resJson{std::nullopt};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp)
        pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return nullptr;
}

trace::ResultWHNF & TracingObject::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto whnfResult = computeWHNFFromObject(*inner);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetWHNF query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto tp = writer.logResult(valueId, whnfResult, qh);
    if (qh.queryHash && tp)
        pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
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
    auto result = inner->getListElem(index);
    auto type = result->getType();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListElem query{parentHash, index};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultType resJson{objectTypeToString(type)};
    auto childTriePos = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && childTriePos)
        pushObservation(parentHash, *qh.queryHash, childTriePos->resultNodeHash);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    child->withScope(argScope);
    if (applyContext) child->withApplyContext(applyContext);
    return child;
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto result = inner->getListOfStringsNoCtx();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListOfStrings query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListOfStrings resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
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
    auto result = inner->getFunctionInfo();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetFunctionInfo query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultFunctionInfo traceResult;
    if (result) {
        traceResult = {.hasInfo = true, .formals = result->formals, .ellipsis = result->ellipsis};
    } else {
        traceResult = {.hasInfo = false};
    }
    auto tp = writer.logResult(valueId, traceResult, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
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
       parallel commentary there for the cidasks routing of the
       apply's triePos and the applyResultSubject attachment. */
    auto fnIdOpt = getScopeStateIdHex();
    auto argIdOpt = argObj->getScopeStateIdHex();
    if (!fnIdOpt || !argIdOpt)
        throw Error("TracingObject::queryApply: fn/arg lacks a content-defined identity");

    /* cb-apply boundary: record an explicit ε edge for this apply.
       See parallel call in TracingEvaluator::apply. */
    nlohmann::json applyBoundaryJson = trace::QueryApply{*fnIdOpt, *argIdOpt};
    tracingCacheLog("markApplyBoundary callsite=TracingObject::queryApply fn=%s arg=%s",
                    fnIdOpt->substr(0, 12), argIdOpt->substr(0, 12));
    writer.markApplyBoundary(applyBoundaryJson);

    auto fnIdHash = Hash::parseNonSRIUnprefixed(*fnIdOpt, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(*argIdOpt, HashAlgorithm::SHA256);

    /* Build ApplyResultSubject from fn/arg via polymorphic
       `getSubject()`. fn = `this`: when this TracingObject is itself
       an apply result, `getSubject()` surfaces its
       applyResultSubject — the next apply sees an evolving
       ApplyResultSubject constituent instead of `PostulatedIdempotentRead{
       this.triePos}` which would freeze the argStateId. Plain TracingObjects
       (= from evalFile, navigation children) return null and the
       PostulatedIdempotentRead fallback fires as a fixed-atom identity. */
    cidasks::Subject fnSubj = getSubject()
        ? *getSubject()
        : cidasks::Subject{cidasks::PostulatedIdempotentRead{fnIdHash}};
    Hash applyScopeLocal = getSubject() ? getInheritedScope() : applyScope;
    cidasks::Subject argSubj = argObj->getSubject()
        ? *argObj->getSubject()
        : cidasks::Subject{cidasks::PostulatedIdempotentRead{argIdHash}};
    if (argObj->getSubject())
        applyScopeLocal = argObj->getInheritedScope();
    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubj)),
    }};

    /* apply-result argStateId is content-only — see commentary in
       TracingEvaluator::apply. */
    auto applyScopeStateId = cidasks::scopeStateIdAfter(resultSubject, applyScopeLocal, {});
    auto applyScopeStateIdHex = applyScopeStateId.to_string(HashFormat::Base16, false);
    writer.bufferApplyProducer(applyScopeStateId,
        Hash::parseNonSRIUnprefixed(*fnIdOpt, HashAlgorithm::SHA256),
        Hash::parseNonSRIUnprefixed(*argIdOpt, HashAlgorithm::SHA256));

    /* Record the apply Request payload at the cidasks hash so dispatch
       and the legacy QueryApply{fn, arg} payload coincide. The legacy
       fnId/argId fields remain for the dispatcher's resolveCdiId
       chain. */
    trace::QueryApply applyQ{*fnIdOpt, *argIdOpt};
    auto v = writer.getSink().logQuery(applyQ);
    auto result = inner->queryApply(argObj);
    TriePosition applyTriePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyScopeStateIdHex,
    };
    auto child = std::shared_ptr<TracingObject>(
        new TracingObject(ref<Object>(result), writer, v, applyTriePos));
    auto cell = ArgScopeCell::make(argScope, argObj);
    child->withScope(std::move(cell));
    child->withApplyResultSubject(std::move(resultSubject), applyScopeLocal);
    child->withApplyProducerIds(*fnIdOpt, *argIdOpt);
    if (auto * argAmb = dynamic_cast<AmbientObject *>(argObj.get())) {
        if (auto ctx = argAmb->getApplyContext())
            child->withApplyContext(std::move(ctx));
    } else if (applyContext) {
        child->withApplyContext(applyContext);
    }
    return child;
}

} // namespace nix

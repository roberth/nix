#include "nix/expr/tracing-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/object-type.hh"
#include "nix/util/hash.hh"

#include <nlohmann/json.hpp>

namespace nix {

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
        auto evolved = cidasks::scopeStateIdAt(*applyResultSubject, applyScope, walk, walk.size());
        auto hex = evolved.to_string(HashFormat::Base16, false);
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

std::vector<std::string> TracingObject::getAttrNames()
{
    auto result = inner->getAttrNames();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttrNames query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListOfStrings resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto result = inner->getStringIgnoreContext();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultString resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

std::string TracingObject::getStringWithoutContext()
{
    auto result = inner->getStringWithoutContext();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultString resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto result = inner->getStringWithContext();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetStringWithContext query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    std::vector<std::string> ctxStrings;
    for (auto & elem : result.second)
        ctxStrings.push_back(elem.to_string());
    trace::ResultStringWithContext resJson{result.first, std::move(ctxStrings)};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

RootedPath TracingObject::getPath()
{
    auto result = inner->getPath();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetPath query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultPath resJson{result.path.abs()};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto result = inner->getBool(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetBool query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultBool resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto result = inner->getInt(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetInt query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultInt resJson{result.value};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

NixFloat TracingObject::getFloat(std::string_view errorCtx)
{
    auto result = inner->getFloat(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetFloat query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultFloat resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

size_t TracingObject::getListSize()
{
    auto result = inner->getListSize();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListSize query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListSize resJson{result};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
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
    auto result = inner->getTypeLazy();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultType resJson{objectTypeToString(result)};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
}

ObjectType TracingObject::getType()
{
    auto result = inner->getType();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultType resJson{objectTypeToString(result)};
    auto tp = writer.logResult(valueId, resJson, qh);
    if (qh.queryHash && tp) pushObservation(parentHash, *qh.queryHash, tp->resultNodeHash);
    return result;
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
    writer.markApplyBoundary(applyBoundaryJson);

    auto fnIdHash = Hash::parseNonSRIUnprefixed(*fnIdOpt, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(*argIdOpt, HashAlgorithm::SHA256);

    /* Build ApplyResultSubject from fn/arg via polymorphic
       `getSubject()`. fn = `this`: when this TracingObject is itself
       an apply result, `getSubject()` surfaces its
       applyResultSubject — the next apply sees an evolving
       ApplyResultSubject constituent instead of `OpaqueContent{
       this.triePos}` which would freeze the argStateId. Plain TracingObjects
       (= from evalFile, navigation children) return null and the
       OpaqueContent fallback fires as a fixed-atom identity. */
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
    if (auto * argAmb = dynamic_cast<AmbientObject *>(argObj.get())) {
        if (auto ctx = argAmb->getApplyContext())
            child->withApplyContext(std::move(ctx));
    } else if (applyContext) {
        child->withApplyContext(applyContext);
    }
    return child;
}

} // namespace nix

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
    /* Bisect: writer-side matches walker — static triePos.queryHashStr
       for both apply-result and non-apply-result wrappers. The cidasks
       evolution happens only at the apply triePos site (=
       TracingEvaluator::apply), not at the child-query emission point. */
    return triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    /* Force inner FIRST so that any ambient observations the body
       makes get flushed into d1CidasksWalk by an intervening logResult
       before we hash this query's `from`. evolvedQueryFrom reads the
       walk's tail and produces a hash that matches what the walker
       computes when it re-traverses the same chain on warm replay. */
    auto result = inner->maybeGetAttr(name);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttr query{name, parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    if (result) {
        // Don't call getType() here — that would force thunks and break laziness.
        // The type is discovered later via a separate getType query on the child.
        trace::ResultMaybeType resJson{std::string("deferred")};
        auto childTriePos = writer.logResult(valueId, resJson, qh);
        auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
        /* Navigation child inherits parent's argScope cell. */
        child->withScope(argScope);
        return child;
    }
    trace::ResultMaybeType resJson{std::nullopt};
    writer.logResult(valueId, resJson, qh);
    return nullptr;
}

std::vector<std::string> TracingObject::getAttrNames()
{
    auto result = inner->getAttrNames();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttrNames query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListOfStrings resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto result = inner->getStringIgnoreContext();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultString resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::string TracingObject::getStringWithoutContext()
{
    auto result = inner->getStringWithoutContext();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultString resJson{result};
    writer.logResult(valueId, resJson, qh);
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
    writer.logResult(valueId, resJson, qh);
    return result;
}

RootedPath TracingObject::getPath()
{
    auto result = inner->getPath();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetPath query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultPath resJson{result.path.abs()};
    writer.logResult(valueId, resJson, qh);
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto result = inner->getBool(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetBool query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultBool resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto result = inner->getInt(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetInt query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultInt resJson{result.value};
    writer.logResult(valueId, resJson, qh);
    return result;
}

NixFloat TracingObject::getFloat(std::string_view errorCtx)
{
    auto result = inner->getFloat(errorCtx);
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetFloat query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultFloat resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

size_t TracingObject::getListSize()
{
    auto result = inner->getListSize();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListSize query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListSize resJson{result};
    writer.logResult(valueId, resJson, qh);
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
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    /* Navigation child inherits parent's argScope cell. */
    child->withScope(argScope);
    return child;
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto result = inner->getListOfStringsNoCtx();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListOfStrings query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultListOfStrings resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

ObjectType TracingObject::getTypeLazy()
{
    auto result = inner->getTypeLazy();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultType resJson{objectTypeToString(result)};
    writer.logResult(valueId, resJson, qh);
    return result;
}

ObjectType TracingObject::getType()
{
    auto result = inner->getType();
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    trace::ResultType resJson{objectTypeToString(result)};
    writer.logResult(valueId, resJson, qh);
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
    writer.logResult(valueId, traceResult, qh);
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
    auto fnIdOpt = getCdiHex();
    auto argIdOpt = argObj->getCdiHex();
    if (!fnIdOpt || !argIdOpt)
        throw Error("TracingObject::queryApply: fn/arg lacks a content-defined identity");

    /* cb-apply boundary: split the writer's flush so applyCdi is
       computed at the post-flush walk index. See parallel call in
       TracingEvaluator::apply. */
    writer.splitFlush();

    auto fnIdHash = Hash::parseNonSRIUnprefixed(*fnIdOpt, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(*argIdOpt, HashAlgorithm::SHA256);

    /* Build ApplyResultSubject from fn/arg. fn here is `this` — a
       TracingObject without a structural Subject of its own; we wrap
       its triePos hash. arg may carry a proper Subject (AmbientObject)
       or fall back similarly. */
    cidasks::Subject fnSubj{cidasks::OpaqueContentSubject{fnIdHash}};
    cidasks::Subject argSubj;
    Hash applyScopeLocal = applyScope;
    if (auto * argAmb = dynamic_cast<AmbientObject *>(argObj.get())) {
        if (auto * s = argAmb->getSubject())
            argSubj = *s;
        else
            argSubj = cidasks::Subject{cidasks::OpaqueContentSubject{argIdHash}};
        applyScopeLocal = argAmb->getInheritedScope();
    } else {
        argSubj = cidasks::Subject{cidasks::OpaqueContentSubject{argIdHash}};
    }
    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubj)),
    }};

    /* Apply triePos goes through cidasks: same formula at apply time
       as evolvedQueryFrom at child-query time, evaluated at the
       current d1CidasksWalk tail. */
    auto & walk = writer.getD1CidasksWalk();
    auto applyCdi = cidasks::contentIdAt(resultSubject, applyScopeLocal, walk, walk.size());
    auto applyCdiHex = applyCdi.to_string(HashFormat::Base16, false);

    /* Record the apply Request payload at the cidasks hash so dispatch
       and the legacy QueryApply{fn, arg} payload coincide. The legacy
       fnId/argId fields remain for the dispatcher's resolveCdiId
       chain. */
    trace::QueryApply applyQ{*fnIdOpt, *argIdOpt};
    auto v = writer.getSink().logQuery(applyQ);
    auto result = inner->queryApply(argObj);
    TriePosition applyTriePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyCdiHex,
    };
    auto child = std::shared_ptr<TracingObject>(
        new TracingObject(ref<Object>(result), writer, v, applyTriePos));
    /* Apply-result scope cell rooted at fn's scope. */
    auto cell = ArgScopeCell::make(argScope, argObj);
    child->withScope(std::move(cell));
    child->withApplyResultSubject(std::move(resultSubject), applyScopeLocal);
    return child;
}

} // namespace nix

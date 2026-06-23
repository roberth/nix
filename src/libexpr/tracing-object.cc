#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/object-type.hh"

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

std::string TracingObject::evolvedQueryFrom() const
{
    /* Mirror of TracingReplayObject's evolvedQueryFrom. For apply-
       result wrappers with an applyContext attached, compute the
       evolved cdi from observations the apply's body has made on
       the arg so far. cidasks composes ApplyResultSubject through
       both fn and arg subjects' content ids at the running walk.
       Without a finalized context we still evolve — the observations
       buffer grows as the lambda body runs, so each child query's
       `from` reflects observations accumulated up to that point. */
    if (applyContext && applyResultSubject && !applyContext->observations.empty()) {
        cidasks::Edge edge{.facts = applyContext->observations};
        std::vector<cidasks::Edge> walk{std::move(edge)};
        auto evolved = cidasks::contentIdAfter(*applyResultSubject, applyContext->scope, walk);
        return evolved.to_string(HashFormat::Base16, false);
    }
    if (triePos)
        return triePos->queryHashStr;
    return std::to_string(valueNum.value());
}

ref<TracingObject> TracingObject::create(
    ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, writer, valueNum, triePos)));
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    /* Force `inner` first so the apply's body (if any) accumulates
       observations into applyContext, then compute `parentHash` from
       the evolved cdi. Pre-force `parentHash` would use the static
       initial cdi and the child fact's `from` would not reflect
       observations made by the apply body — recorder and walker
       would land at different trie positions on warm replay. */
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
    /* Object-method counterpart of TracingEvaluator::apply. Mirrors
       its logic: compute fnId/argId, hash QueryApply, log the
       Request, delegate to inner->queryApply, wrap as TracingObject
       with the apply's triePos so further accesses on the result
       parent on the apply's queryHash. */
    auto fnIdOpt = getCdiHex();
    auto argIdOpt = argObj->getCdiHex();
    if (!fnIdOpt || !argIdOpt)
        throw Error("TracingObject::queryApply: fn/arg lacks a content-defined identity");
    auto queryHash = TracingDecisionGraph::computeQueryHash(trace::QueryApply{*fnIdOpt, *argIdOpt});
    auto v = writer.getSink().logQuery(trace::QueryApply{*fnIdOpt, *argIdOpt});
    auto result = inner->queryApply(argObj);
    TriePosition applyTriePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
    };
    auto child = std::shared_ptr<TracingObject>(
        new TracingObject(ref<Object>(result), writer, v, applyTriePos));
    /* Apply-result scope cell rooted at fn's scope. */
    auto cell = ArgScopeCell::make(argScope, argObj);
    child->withScope(std::move(cell));
    return child;
}

} // namespace nix

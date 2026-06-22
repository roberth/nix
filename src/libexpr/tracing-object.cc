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

ref<TracingObject> TracingObject::create(
    ref<Object> inner, TracingWriter & writer, ValueHandle valueNum, std::optional<TriePosition> triePos)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, writer, valueNum, triePos)));
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetAttr query{name, parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->maybeGetAttr(name);
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
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetAttrNames query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getAttrNames();
    trace::ResultListOfStrings resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getStringIgnoreContext();
    trace::ResultString resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::string TracingObject::getStringWithoutContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetString query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getStringWithoutContext();
    trace::ResultString resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetStringWithContext query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getStringWithContext();
    std::vector<std::string> ctxStrings;
    for (auto & elem : result.second)
        ctxStrings.push_back(elem.to_string());
    trace::ResultStringWithContext resJson{result.first, std::move(ctxStrings)};
    writer.logResult(valueId, resJson, qh);
    return result;
}

RootedPath TracingObject::getPath()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetPath query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getPath();
    trace::ResultPath resJson{result.path.abs()};
    writer.logResult(valueId, resJson, qh);
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetBool query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getBool(errorCtx);
    trace::ResultBool resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetInt query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getInt(errorCtx);
    trace::ResultInt resJson{result.value};
    writer.logResult(valueId, resJson, qh);
    return result;
}

NixFloat TracingObject::getFloat(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetFloat query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getFloat(errorCtx);
    trace::ResultFloat resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

size_t TracingObject::getListSize()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetListSize query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getListSize();
    trace::ResultListSize resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

std::shared_ptr<Object> TracingObject::getListElem(size_t index)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetListElem query{parentHash, index};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getListElem(index);
    auto type = result->getType();
    trace::ResultType resJson{objectTypeToString(type)};
    auto childTriePos = writer.logResult(valueId, resJson, qh);
    auto child = std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    /* Navigation child inherits parent's argScope cell. */
    child->withScope(argScope);
    return child;
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetListOfStrings query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getListOfStringsNoCtx();
    trace::ResultListOfStrings resJson{result};
    writer.logResult(valueId, resJson, qh);
    return result;
}

ObjectType TracingObject::getTypeLazy()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getTypeLazy();
    trace::ResultType resJson{objectTypeToString(result)};
    writer.logResult(valueId, resJson, qh);
    return result;
}

ObjectType TracingObject::getType()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetType query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getType();
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
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum.value());
    trace::QueryGetFunctionInfo query{parentHash};
    auto [valueId, qh] = writer.logQuery(query, triePos);
    auto result = inner->getFunctionInfo();
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

} // namespace nix

#include "nix/expr/tracing-object.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs: return "set";
    case nList: return "list";
    case nString: return "string";
    case nPath: return "path";
    case nInt: return "int";
    case nFloat: return "float";
    case nBool: return "bool";
    case nNull: return "null";
    case nFunction: return "lambda";
    case nThunk: return "thunk";
    case nExternal: return "external";
    case nFailed: return "failed";
    }
    return "unknown";
}

TracingObject::TracingObject(
    ref<Object> inner, TracingWriter & writer, uint64_t valueNum, std::optional<TriePosition> triePos)
    : inner(inner)
    , writer(writer)
    , valueNum(valueNum)
    , triePos(triePos)
{
}

ref<TracingObject>
TracingObject::create(ref<Object> inner, TracingWriter & writer, uint64_t valueNum, std::optional<TriePosition> triePos)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, writer, valueNum, triePos)));
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetAttr{name, parentHash}, triePos);
    auto result = inner->maybeGetAttr(name);
    if (result) {
        auto type = result->getType();
        auto childTriePos = writer.logResult(valueId, trace::ResultMaybeType{objectTypeToString(type)});
        return std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
    }
    writer.logResult(valueId, trace::ResultMaybeType{std::nullopt});
    return nullptr;
}

std::vector<std::string> TracingObject::getAttrNames()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetAttrNames{parentHash}, triePos);
    auto result = inner->getAttrNames();
    writer.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetString{parentHash}, triePos);
    auto result = inner->getStringIgnoreContext();
    writer.logResult(valueId, trace::ResultString{result});
    return result;
}

std::string TracingObject::getStringWithoutContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetString{parentHash}, triePos);
    auto result = inner->getStringWithoutContext();
    writer.logResult(valueId, trace::ResultString{result});
    return result;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetStringWithContext{parentHash}, triePos);
    auto result = inner->getStringWithContext();
    std::vector<std::string> ctxStrings;
    for (auto & elem : result.second)
        ctxStrings.push_back(elem.to_string());
    writer.logResult(valueId, trace::ResultStringWithContext{result.first, std::move(ctxStrings)});
    return result;
}

SourcePath TracingObject::getPath()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetPath{parentHash}, triePos);
    auto result = inner->getPath();
    writer.logResult(valueId, trace::ResultPath{result.path.abs()});
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetBool{parentHash}, triePos);
    auto result = inner->getBool(errorCtx);
    writer.logResult(valueId, trace::ResultBool{result});
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetInt{parentHash}, triePos);
    auto result = inner->getInt(errorCtx);
    writer.logResult(valueId, trace::ResultInt{result.value});
    return result;
}

NixFloat TracingObject::getFloat(std::string_view errorCtx)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetFloat{parentHash}, triePos);
    auto result = inner->getFloat(errorCtx);
    writer.logResult(valueId, trace::ResultFloat{result});
    return result;
}

size_t TracingObject::getListSize()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetListSize{parentHash}, triePos);
    auto result = inner->getListSize();
    writer.logResult(valueId, trace::ResultListSize{result});
    return result;
}

std::shared_ptr<Object> TracingObject::getListElem(size_t index)
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetListElem{parentHash, index}, triePos);
    auto result = inner->getListElem(index);
    auto type = result->getType();
    auto childTriePos = writer.logResult(valueId, trace::ResultType{objectTypeToString(type)});
    return std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), writer, valueId, childTriePos));
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetListOfStrings{parentHash}, triePos);
    auto result = inner->getListOfStringsNoCtx();
    writer.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

ObjectType TracingObject::getTypeLazy()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetType{parentHash}, triePos);
    auto result = inner->getTypeLazy();
    writer.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

ObjectType TracingObject::getType()
{
    auto parentHash = triePos ? triePos->queryHashStr : std::to_string(valueNum);
    auto [valueId, _] = writer.logQuery(trace::QueryGetType{parentHash}, triePos);
    auto result = inner->getType();
    writer.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

RootValue TracingObject::defeatCache()
{
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingObject::getFunctionInfo()
{
    return inner->getFunctionInfo();
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

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

TracingObject::TracingObject(ref<Object> inner, TraceSink & sink, uint64_t valueNum)
    : inner(inner)
    , sink(sink)
    , valueNum(valueNum)
{
}

ref<TracingObject> TracingObject::create(ref<Object> inner, TraceSink & sink, uint64_t valueNum)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, sink, valueNum)));
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    auto valueId = sink.logQuery(trace::QueryGetAttr{name, std::to_string(valueNum)});
    auto result = inner->maybeGetAttr(name);
    if (result) {
        auto type = result->getType();
        sink.logResult(valueId, trace::ResultMaybeType{objectTypeToString(type)});
        return std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), sink, valueId));
    }
    sink.logResult(valueId, trace::ResultMaybeType{std::nullopt});
    return nullptr;
}

std::vector<std::string> TracingObject::getAttrNames()
{
    auto valueId = sink.logQuery(trace::QueryGetAttrNames{std::to_string(valueNum)});
    auto result = inner->getAttrNames();
    sink.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto valueId = sink.logQuery(trace::QueryGetString{std::to_string(valueNum)});
    auto result = inner->getStringIgnoreContext();
    sink.logResult(valueId, trace::ResultString{result});
    return result;
}

std::string TracingObject::getStringWithoutContext()
{
    auto valueId = sink.logQuery(trace::QueryGetString{std::to_string(valueNum)});
    auto result = inner->getStringWithoutContext();
    sink.logResult(valueId, trace::ResultString{result});
    return result;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto valueId = sink.logQuery(trace::QueryGetStringWithContext{std::to_string(valueNum)});
    auto result = inner->getStringWithContext();
    // Serialize context elements as strings
    std::vector<std::string> ctxStrings;
    for (auto & elem : result.second)
        ctxStrings.push_back(elem.to_string());
    sink.logResult(valueId, trace::ResultStringWithContext{result.first, std::move(ctxStrings)});
    return result;
}

SourcePath TracingObject::getPath()
{
    auto valueId = sink.logQuery(trace::QueryGetPath{std::to_string(valueNum)});
    auto result = inner->getPath();
    sink.logResult(valueId, trace::ResultPath{result.path.abs()});
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto valueId = sink.logQuery(trace::QueryGetBool{std::to_string(valueNum)});
    auto result = inner->getBool(errorCtx);
    sink.logResult(valueId, trace::ResultBool{result});
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto valueId = sink.logQuery(trace::QueryGetInt{std::to_string(valueNum)});
    auto result = inner->getInt(errorCtx);
    sink.logResult(valueId, trace::ResultInt{result.value});
    return result;
}

NixFloat TracingObject::getFloat(std::string_view errorCtx)
{
    auto valueId = sink.logQuery(trace::QueryGetFloat{std::to_string(valueNum)});
    auto result = inner->getFloat(errorCtx);
    sink.logResult(valueId, trace::ResultFloat{result});
    return result;
}

size_t TracingObject::getListSize()
{
    auto valueId = sink.logQuery(trace::QueryGetListSize{std::to_string(valueNum)});
    auto result = inner->getListSize();
    sink.logResult(valueId, trace::ResultListSize{result});
    return result;
}

std::shared_ptr<Object> TracingObject::getListElem(size_t index)
{
    auto valueId = sink.logQuery(trace::QueryGetListElem{std::to_string(valueNum), index});
    auto result = inner->getListElem(index);
    auto type = result->getType();
    sink.logResult(valueId, trace::ResultType{objectTypeToString(type)});
    return std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), sink, valueId));
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto valueId = sink.logQuery(trace::QueryGetListOfStrings{std::to_string(valueNum)});
    auto result = inner->getListOfStringsNoCtx();
    sink.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

ObjectType TracingObject::getTypeLazy()
{
    // getTypeLazy doesn't force — trace but note it may return nThunk
    auto valueId = sink.logQuery(trace::QueryGetType{std::to_string(valueNum)});
    auto result = inner->getTypeLazy();
    sink.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

ObjectType TracingObject::getType()
{
    auto valueId = sink.logQuery(trace::QueryGetType{std::to_string(valueNum)});
    auto result = inner->getType();
    sink.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

RootValue TracingObject::defeatCache()
{
    // defeatCache bypasses tracing — it's for escaping to raw Values
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingObject::getFunctionInfo()
{
    // Delegate without tracing — function reflection is metadata, not evaluation
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

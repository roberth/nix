#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-database.hh"
#include "nix/expr/trace-types.hh"

#include <nlohmann/json.hpp>

namespace nix {

static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs:
        return "set";
    case nList:
        return "list";
    case nString:
        return "string";
    case nPath:
        return "path";
    case nInt:
        return "int";
    case nFloat:
        return "float";
    case nBool:
        return "bool";
    case nNull:
        return "null";
    case nFunction:
        return "lambda";
    case nThunk:
        return "thunk";
    case nExternal:
        return "external";
    }
}

TracingObject::TracingObject(ref<Object> inner, TraceFile & traceFile, uint64_t valueNum)
    : inner(inner)
    , traceFile(traceFile)
    , valueNum(valueNum)
{
}

ref<TracingObject> TracingObject::create(ref<Object> inner, TraceFile & traceFile, uint64_t valueNum)
{
    return ref<TracingObject>(std::shared_ptr<TracingObject>(new TracingObject(inner, traceFile, valueNum)));
}

std::shared_ptr<Object> TracingObject::maybeGetAttr(const std::string & name)
{
    auto valueId = traceFile.logQuery(trace::QueryGetAttr{name, valueNum});
    auto result = inner->maybeGetAttr(name);
    if (result) {
        auto type = result->getType();
        traceFile.logResult(valueId, trace::ResultMaybeType{objectTypeToString(type)});
        return std::shared_ptr<TracingObject>(new TracingObject(ref<Object>(result), traceFile, valueId));
    }
    traceFile.logResult(valueId, trace::ResultMaybeType{std::nullopt});
    return nullptr;
}

std::vector<std::string> TracingObject::getAttrNames()
{
    auto valueId = traceFile.logQuery(trace::QueryGetAttrNames{valueNum});
    auto result = inner->getAttrNames();
    traceFile.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

std::string TracingObject::getStringIgnoreContext()
{
    auto valueId = traceFile.logQuery(trace::QueryGetString{valueNum});
    auto result = inner->getStringIgnoreContext();
    traceFile.logResult(valueId, trace::ResultString{result});
    return result;
}

std::pair<std::string, NixStringContext> TracingObject::getStringWithContext()
{
    auto valueId = traceFile.logQuery(trace::QueryGetStringWithContext{valueNum});
    auto result = inner->getStringWithContext();
    std::vector<std::string> ctx;
    for (const auto & elem : result.second)
        ctx.push_back(elem.to_string());
    traceFile.logResult(valueId, trace::ResultStringWithContext{result.first, std::move(ctx)});
    return result;
}

SourcePath TracingObject::getPath()
{
    auto valueId = traceFile.logQuery(trace::QueryGetPath{valueNum});
    auto result = inner->getPath();
    traceFile.logResult(valueId, trace::ResultPath{result.path.abs()});
    return result;
}

bool TracingObject::getBool(std::string_view errorCtx)
{
    auto valueId = traceFile.logQuery(trace::QueryGetBool{valueNum});
    auto result = inner->getBool(errorCtx);
    traceFile.logResult(valueId, trace::ResultBool{result});
    return result;
}

NixInt TracingObject::getInt(std::string_view errorCtx)
{
    auto valueId = traceFile.logQuery(trace::QueryGetInt{valueNum});
    auto result = inner->getInt(errorCtx);
    traceFile.logResult(valueId, trace::ResultInt{result.value});
    return result;
}

std::vector<std::string> TracingObject::getListOfStringsNoCtx()
{
    auto valueId = traceFile.logQuery(trace::QueryGetListOfStrings{valueNum});
    auto result = inner->getListOfStringsNoCtx();
    traceFile.logResult(valueId, trace::ResultListOfStrings{result});
    return result;
}

ObjectType TracingObject::getTypeLazy()
{
    auto valueId = traceFile.logQuery(trace::QueryGetType{valueNum});
    auto result = inner->getTypeLazy();
    traceFile.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

ObjectType TracingObject::getType()
{
    auto valueId = traceFile.logQuery(trace::QueryGetType{valueNum});
    auto result = inner->getType();
    traceFile.logResult(valueId, trace::ResultType{objectTypeToString(result)});
    return result;
}

RootValue TracingObject::defeatCache()
{
    // defeatCache doesn't need tracing - it's for escaping to EvalState
    return inner->defeatCache();
}

} // namespace nix

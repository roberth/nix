#include "nix/expr/contra-object.hh"

namespace nix {

// TODO: share with tracing-replay-object.cc
static ObjectType stringToObjectType(const std::string & type)
{
    if (type == "set")
        return nAttrs;
    if (type == "list")
        return nList;
    if (type == "string")
        return nString;
    if (type == "path")
        return nPath;
    if (type == "int")
        return nInt;
    if (type == "float")
        return nFloat;
    if (type == "bool")
        return nBool;
    if (type == "null")
        return nNull;
    if (type == "lambda")
        return nFunction;
    throw Error("unknown object type: %s", type);
}

ContraObject::ContraObject(std::string id, ContraQueryFn queryFn)
    : id(std::move(id))
    , queryFn(std::move(queryFn))
{
}

std::shared_ptr<Object> ContraObject::maybeGetAttr(const std::string & name)
{
    auto result = queryFn(trace::QueryGetAttr{name, id});
    auto * r = std::get_if<trace::ResultMaybeType>(&result);
    if (!r || !r->type)
        return nullptr;
    // Child is structurally identified: parent id + attr name
    return std::make_shared<ContraObject>(id + "." + name, queryFn);
}

std::vector<std::string> ContraObject::getAttrNames()
{
    auto result = queryFn(trace::QueryGetAttrNames{id});
    auto * r = std::get_if<trace::ResultListOfStrings>(&result);
    if (!r)
        throw Error("contra-query getAttrNames: unexpected result type");
    return r->values;
}

std::string ContraObject::getStringIgnoreContext()
{
    auto result = queryFn(trace::QueryGetString{id});
    auto * r = std::get_if<trace::ResultString>(&result);
    if (!r)
        throw Error("contra-query getString: unexpected result type");
    return r->value;
}

std::string ContraObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ContraObject::getStringWithContext()
{
    auto result = queryFn(trace::QueryGetStringWithContext{id});
    auto * r = std::get_if<trace::ResultStringWithContext>(&result);
    if (!r)
        throw Error("contra-query getStringWithContext: unexpected result type");
    NixStringContext ctx;
    for (auto & s : r->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r->value, std::move(ctx)};
}

SourcePath ContraObject::getPath()
{
    auto result = queryFn(trace::QueryGetPath{id});
    auto * r = std::get_if<trace::ResultPath>(&result);
    if (!r)
        throw Error("contra-query getPath: unexpected result type");
    // ContraObject can't reconstruct a full SourcePath from just a
    // string path — this would need the accessor. For now, throw.
    throw Error("contra-query getPath: not yet supported");
}

bool ContraObject::getBool(std::string_view)
{
    auto result = queryFn(trace::QueryGetBool{id});
    auto * r = std::get_if<trace::ResultBool>(&result);
    if (!r)
        throw Error("contra-query getBool: unexpected result type");
    return r->value;
}

NixInt ContraObject::getInt(std::string_view)
{
    auto result = queryFn(trace::QueryGetInt{id});
    auto * r = std::get_if<trace::ResultInt>(&result);
    if (!r)
        throw Error("contra-query getInt: unexpected result type");
    return NixInt{r->value};
}

NixFloat ContraObject::getFloat(std::string_view)
{
    auto result = queryFn(trace::QueryGetFloat{id});
    auto * r = std::get_if<trace::ResultFloat>(&result);
    if (!r)
        throw Error("contra-query getFloat: unexpected result type");
    return r->value;
}

size_t ContraObject::getListSize()
{
    auto result = queryFn(trace::QueryGetListSize{id});
    auto * r = std::get_if<trace::ResultListSize>(&result);
    if (!r)
        throw Error("contra-query getListSize: unexpected result type");
    return r->size;
}

std::shared_ptr<Object> ContraObject::getListElem(size_t index)
{
    auto result = queryFn(trace::QueryGetListElem{id, index});
    // Child is structurally identified: parent id + index
    return std::make_shared<ContraObject>(id + "[" + std::to_string(index) + "]", queryFn);
}

ObjectType ContraObject::getTypeLazy()
{
    return getType();
}

ObjectType ContraObject::getType()
{
    auto result = queryFn(trace::QueryGetType{id});
    auto * r = std::get_if<trace::ResultType>(&result);
    if (!r)
        throw Error("contra-query getType: unexpected result type");
    return stringToObjectType(r->type);
}

RootValue ContraObject::defeatCache()
{
    throw Error("contra-query defeatCache: not supported on virtual values");
}

std::optional<FunctionInfo> ContraObject::getFunctionInfo()
{
    // TODO: could issue a contra-query for function info
    return std::nullopt;
}

PosIdx ContraObject::getPos()
{
    return noPos;
}

std::optional<std::vector<std::string>> ContraObject::getAttrPath()
{
    return std::nullopt;
}

} // namespace nix

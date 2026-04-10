#include "nix/expr/ambient-object.hh"

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

AmbientObject::AmbientObject(std::string id, AmbientQueryFn queryFn, AmbientRegisterLocalFn registerLocal)
    : id(std::move(id))
    , queryFn(std::move(queryFn))
    , registerLocal(std::move(registerLocal))
{
}

std::shared_ptr<Object> AmbientObject::maybeGetAttr(const std::string & name)
{
    auto result = queryFn(trace::QueryGetAttr{name, id});
    auto * r = std::get_if<trace::ResultMaybeType>(&result);
    if (!r || !r->type)
        return nullptr;
    // Child is structurally identified: parent id + attr name
    return std::make_shared<AmbientObject>(id + "." + name, queryFn, registerLocal);
}

std::vector<std::string> AmbientObject::getAttrNames()
{
    auto result = queryFn(trace::QueryGetAttrNames{id});
    auto * r = std::get_if<trace::ResultListOfStrings>(&result);
    if (!r)
        throw Error("contra-query getAttrNames: unexpected result type");
    return r->values;
}

std::string AmbientObject::getStringIgnoreContext()
{
    auto result = queryFn(trace::QueryGetString{id});
    auto * r = std::get_if<trace::ResultString>(&result);
    if (!r)
        throw Error("contra-query getString: unexpected result type");
    return r->value;
}

std::string AmbientObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> AmbientObject::getStringWithContext()
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

SourcePath AmbientObject::getPath()
{
    auto result = queryFn(trace::QueryGetPath{id});
    auto * r = std::get_if<trace::ResultPath>(&result);
    if (!r)
        throw Error("contra-query getPath: unexpected result type");
    // AmbientObject can't reconstruct a full SourcePath from just a
    // string path — this would need the accessor. For now, throw.
    throw Error("contra-query getPath: not yet supported");
}

bool AmbientObject::getBool(std::string_view)
{
    auto result = queryFn(trace::QueryGetBool{id});
    auto * r = std::get_if<trace::ResultBool>(&result);
    if (!r)
        throw Error("contra-query getBool: unexpected result type");
    return r->value;
}

NixInt AmbientObject::getInt(std::string_view)
{
    auto result = queryFn(trace::QueryGetInt{id});
    auto * r = std::get_if<trace::ResultInt>(&result);
    if (!r)
        throw Error("contra-query getInt: unexpected result type");
    return NixInt{r->value};
}

NixFloat AmbientObject::getFloat(std::string_view)
{
    auto result = queryFn(trace::QueryGetFloat{id});
    auto * r = std::get_if<trace::ResultFloat>(&result);
    if (!r)
        throw Error("contra-query getFloat: unexpected result type");
    return r->value;
}

size_t AmbientObject::getListSize()
{
    auto result = queryFn(trace::QueryGetListSize{id});
    auto * r = std::get_if<trace::ResultListSize>(&result);
    if (!r)
        throw Error("contra-query getListSize: unexpected result type");
    return r->size;
}

std::shared_ptr<Object> AmbientObject::getListElem(size_t index)
{
    auto result = queryFn(trace::QueryGetListElem{id, index});
    // Child is structurally identified: parent id + index
    return std::make_shared<AmbientObject>(id + "[" + std::to_string(index) + "]", queryFn, registerLocal);
}

ObjectType AmbientObject::getTypeLazy()
{
    return getType();
}

ObjectType AmbientObject::getType()
{
    auto result = queryFn(trace::QueryGetType{id});
    auto * r = std::get_if<trace::ResultType>(&result);
    if (!r)
        throw Error("contra-query getType: unexpected result type");
    return stringToObjectType(r->type);
}

RootValue AmbientObject::defeatCache()
{
    throw Error("contra-query defeatCache: not supported on virtual values");
}

std::optional<FunctionInfo> AmbientObject::getFunctionInfo()
{
    auto result = queryFn(trace::QueryGetFunctionInfo{id});
    auto * r = std::get_if<trace::ResultFunctionInfo>(&result);
    if (!r || !r->hasInfo)
        return std::nullopt;
    return FunctionInfo{.formals = r->formals, .ellipsis = r->ellipsis};
}

PosIdx AmbientObject::getPos()
{
    return noPos;
}

std::optional<std::vector<std::string>> AmbientObject::getAttrPath()
{
    return std::nullopt;
}

std::shared_ptr<Object> AmbientObject::queryApply(const std::string & argId, std::shared_ptr<Object> argObj)
{
    if (!registerLocal)
        throw Error("ambient apply: no registerLocal callback");

    auto localId = registerLocal(std::move(argObj));
    auto result = queryFn(trace::QueryApply{id, localId});
    auto * r = std::get_if<trace::ResultType>(&result);
    if (!r)
        throw Error("ambient apply: unexpected result type");

    auto resultId = id + ".apply(" + localId + ")";
    return std::make_shared<AmbientObject>(resultId, queryFn, registerLocal);
}

} // namespace nix

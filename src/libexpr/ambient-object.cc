#include "nix/expr/ambient-object.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

static ObjectType stringToObjectType(const std::string & type)
{
    if (type == "set") return nAttrs;
    if (type == "list") return nList;
    if (type == "string") return nString;
    if (type == "path") return nPath;
    if (type == "int") return nInt;
    if (type == "float") return nFloat;
    if (type == "bool") return nBool;
    if (type == "null") return nNull;
    if (type == "lambda") return nFunction;
    throw Error("unknown object type: %s", type);
}

AmbientObject::AmbientObject(int id, AmbientQueryFn queryFn, AmbientApplyFn applyFn)
    : id(id)
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
{
}

std::shared_ptr<Object> AmbientObject::maybeGetAttr(const std::string & name)
{
    auto qr = queryFn(id, trace::QueryGetAttr{name, std::to_string(id)});
    auto * r = std::get_if<trace::ResultMaybeType>(&qr.result);
    if (!r || !r->type)
        return nullptr;
    if (!qr.childId)
        throw Error("ambient maybeGetAttr: resolver didn't return child id");
    return std::make_shared<AmbientObject>(*qr.childId, queryFn, applyFn);
}

std::vector<std::string> AmbientObject::getAttrNames()
{
    auto qr = queryFn(id, trace::QueryGetAttrNames{std::to_string(id)});
    auto * r = std::get_if<trace::ResultListOfStrings>(&qr.result);
    if (!r)
        throw Error("ambient getAttrNames: unexpected result type");
    return r->values;
}

std::string AmbientObject::getStringIgnoreContext()
{
    auto qr = queryFn(id, trace::QueryGetString{std::to_string(id)});
    auto * r = std::get_if<trace::ResultString>(&qr.result);
    if (!r)
        throw Error("ambient getString: unexpected result type");
    return r->value;
}

std::string AmbientObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> AmbientObject::getStringWithContext()
{
    auto qr = queryFn(id, trace::QueryGetStringWithContext{std::to_string(id)});
    auto * r = std::get_if<trace::ResultStringWithContext>(&qr.result);
    if (!r)
        throw Error("ambient getStringWithContext: unexpected result type");
    NixStringContext ctx;
    for (auto & s : r->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r->value, std::move(ctx)};
}

SourcePath AmbientObject::getPath()
{
    auto qr = queryFn(id, trace::QueryGetPath{std::to_string(id)});
    auto * r = std::get_if<trace::ResultPath>(&qr.result);
    if (!r)
        throw Error("ambient getPath: unexpected result type");
    return SourcePath(getFSSourceAccessor(), CanonPath(r->path));
}

bool AmbientObject::getBool(std::string_view)
{
    auto qr = queryFn(id, trace::QueryGetBool{std::to_string(id)});
    auto * r = std::get_if<trace::ResultBool>(&qr.result);
    if (!r)
        throw Error("ambient getBool: unexpected result type");
    return r->value;
}

NixInt AmbientObject::getInt(std::string_view)
{
    auto qr = queryFn(id, trace::QueryGetInt{std::to_string(id)});
    auto * r = std::get_if<trace::ResultInt>(&qr.result);
    if (!r)
        throw Error("ambient getInt: unexpected result type");
    return NixInt{r->value};
}

NixFloat AmbientObject::getFloat(std::string_view)
{
    auto qr = queryFn(id, trace::QueryGetFloat{std::to_string(id)});
    auto * r = std::get_if<trace::ResultFloat>(&qr.result);
    if (!r)
        throw Error("ambient getFloat: unexpected result type");
    return r->value;
}

size_t AmbientObject::getListSize()
{
    auto qr = queryFn(id, trace::QueryGetListSize{std::to_string(id)});
    auto * r = std::get_if<trace::ResultListSize>(&qr.result);
    if (!r)
        throw Error("ambient getListSize: unexpected result type");
    return r->size;
}

std::shared_ptr<Object> AmbientObject::getListElem(size_t index)
{
    auto qr = queryFn(id, trace::QueryGetListElem{std::to_string(id), index});
    if (!qr.childId)
        throw Error("ambient getListElem: resolver didn't return child id");
    return std::make_shared<AmbientObject>(*qr.childId, queryFn, applyFn);
}

ObjectType AmbientObject::getTypeLazy()
{
    return getType();
}

ObjectType AmbientObject::getType()
{
    auto qr = queryFn(id, trace::QueryGetType{std::to_string(id)});
    auto * r = std::get_if<trace::ResultType>(&qr.result);
    if (!r)
        throw Error("ambient getType: unexpected result type");
    return stringToObjectType(r->type);
}

RootValue AmbientObject::defeatCache()
{
    throw Error("ambient defeatCache: not supported on virtual values");
}

std::optional<FunctionInfo> AmbientObject::getFunctionInfo()
{
    auto qr = queryFn(id, trace::QueryGetFunctionInfo{std::to_string(id)});
    auto * r = std::get_if<trace::ResultFunctionInfo>(&qr.result);
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

std::shared_ptr<Object> AmbientObject::queryApply(std::shared_ptr<Object> argObj)
{
    if (!applyFn)
        throw Error("ambient apply: no apply callback");
    auto resultId = applyFn(id, std::move(argObj));
    return std::make_shared<AmbientObject>(resultId, queryFn, applyFn);
}

} // namespace nix

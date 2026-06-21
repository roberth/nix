#include "nix/expr/ambient-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Under Step C, AmbientId is a Hash. The wire format puts the
   hex representation in the query's `from` field. */
static std::string fromOf(AmbientId cdi)
{
    return cdi.to_string(HashFormat::Base16, false);
}

AmbientObject::AmbientObject(
    AmbientId cdi, AmbientQueryFn queryFn, ref<SourceRoot> ambientRootFSRoot, AmbientApplyFn applyFn)
    : cdi(cdi)
    , queryFn(std::move(queryFn))
    , applyFn(std::move(applyFn))
    , ambientRootFSRoot(std::move(ambientRootFSRoot))
{
}

std::shared_ptr<Object> AmbientObject::maybeGetAttr(const std::string & name)
{
    auto qr = queryFn(cdi, trace::QueryGetAttr{name, fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultMaybeType>(&qr.result);
    if (!r || !r->type)
        return nullptr;
    if (!qr.childId)
        throw Error("ambient maybeGetAttr: resolver didn't return child id");
    auto child = std::make_shared<AmbientObject>(*qr.childId, queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    return child;
}

std::vector<std::string> AmbientObject::getAttrNames()
{
    auto qr = queryFn(cdi, trace::QueryGetAttrNames{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultListOfStrings>(&qr.result);
    if (!r)
        throw Error("ambient getAttrNames: unexpected result type");
    return r->values;
}

std::string AmbientObject::getStringIgnoreContext()
{
    auto qr = queryFn(cdi, trace::QueryGetString{fromOf(cdi)}, effectiveArgScope(*this));
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
    auto qr = queryFn(cdi, trace::QueryGetStringWithContext{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultStringWithContext>(&qr.result);
    if (!r)
        throw Error("ambient getStringWithContext: unexpected result type");
    NixStringContext ctx;
    for (auto & s : r->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r->value, std::move(ctx)};
}

RootedPath AmbientObject::getPath()
{
    auto qr = queryFn(cdi, trace::QueryGetPath{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultPath>(&qr.result);
    if (!r)
        throw Error("ambient getPath: unexpected result type");
    /* lazy-paths: reuse the outer EvalState's `rootFSRoot` so the
       SourceRoot outlives the Value the outer evaluator constructs
       from this path. A one-off `SourceRoot::make` here would be
       freed when the returned RootedPath drops, leaving Value's raw
       SourceRoot pointer dangling. */
    return RootedPath{ambientRootFSRoot, CanonPath(r->path)};
}

bool AmbientObject::getBool(std::string_view)
{
    auto qr = queryFn(cdi, trace::QueryGetBool{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultBool>(&qr.result);
    if (!r)
        throw Error("ambient getBool: unexpected result type");
    return r->value;
}

NixInt AmbientObject::getInt(std::string_view)
{
    auto qr = queryFn(cdi, trace::QueryGetInt{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultInt>(&qr.result);
    if (!r)
        throw Error("ambient getInt: unexpected result type");
    return NixInt{r->value};
}

NixFloat AmbientObject::getFloat(std::string_view)
{
    auto qr = queryFn(cdi, trace::QueryGetFloat{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultFloat>(&qr.result);
    if (!r)
        throw Error("ambient getFloat: unexpected result type");
    return r->value;
}

size_t AmbientObject::getListSize()
{
    auto qr = queryFn(cdi, trace::QueryGetListSize{fromOf(cdi)}, effectiveArgScope(*this));
    auto * r = std::get_if<trace::ResultListSize>(&qr.result);
    if (!r)
        throw Error("ambient getListSize: unexpected result type");
    return r->size;
}

std::shared_ptr<Object> AmbientObject::getListElem(size_t index)
{
    auto qr = queryFn(cdi, trace::QueryGetListElem{fromOf(cdi), index}, effectiveArgScope(*this));
    if (!qr.childId)
        throw Error("ambient getListElem: resolver didn't return child id");
    auto child = std::make_shared<AmbientObject>(*qr.childId, queryFn, ambientRootFSRoot, applyFn);
    /* Navigation child inherits parent's argScope cell directly. */
    child->withScope(argScope);
    return child;
}

ObjectType AmbientObject::getTypeLazy()
{
    return getType();
}

ObjectType AmbientObject::getType()
{
    auto qr = queryFn(cdi, trace::QueryGetType{fromOf(cdi)}, effectiveArgScope(*this));
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
    auto qr = queryFn(cdi, trace::QueryGetFunctionInfo{fromOf(cdi)}, effectiveArgScope(*this));
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
    /* Thread the caller's effective scope into applyFn so the cb
       apply's new local cell can chain off the right depth, even
       when `resolve(fnId)` returns an InterpreterObject without a
       proxy parent chain. Keep a copy of argObj for the result's
       cell before moving it into applyFn. */
    auto callerScope = effectiveArgScope(*this);
    auto argForScope = argObj;
    auto resultId = applyFn(cdi, std::move(argObj), callerScope);
    auto result = std::make_shared<AmbientObject>(resultId, queryFn, ambientRootFSRoot, applyFn);
    /* Apply-result: open a new intrinsic cell for this apply's
       argument, rooted at the same caller scope the applyFn used. */
    auto cell = ArgScopeCell::make(callerScope, std::move(argForScope));
    result->withScope(std::move(cell));
    return result;
}

} // namespace nix

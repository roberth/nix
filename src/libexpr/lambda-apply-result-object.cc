#include "nix/expr/lambda-apply-result-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix {

LambdaApplyResultObject::LambdaApplyResultObject(
    ref<Object> inner_,
    TracingWriter & writer_,
    cidasks::Subject applyResultSubject_,
    Hash applyScope_,
    Hash depth2ApplyId_)
    : inner(std::move(inner_))
    , writer(writer_)
    , applyResultSubject(std::move(applyResultSubject_))
    , applyScope(std::move(applyScope_))
    , depth2ApplyId(std::move(depth2ApplyId_))
{
    auto cdi = cidasks::contentIdAfter(applyResultSubject, applyScope, {});
    applyCdiHex = cdi.to_string(HashFormat::Base16, false);
}

void LambdaApplyResultObject::recordD2(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Route through the depth-2 entry point: every observation on
       this apply-result is grouped with the recursive apply Fact
       under the enclosing cb-apply boundary so that the d=2 chain
       has [recursiveApplyFact, this_obs, next_obs, ...] in the
       order they're appended. flushPendingAmbient's d=2 loop
       stamps each `from` at `edgeIndex = i` (= position in the
       boundary's facts vector), matching the walker's stamping at
       `walkFacts.size()` after the synthetic-side primop pushed
       the apply Fact. */
    writer.logDepth2Observation(query, result, applyResultSubject, applyScope, depth2ApplyId);
}

std::shared_ptr<Object> LambdaApplyResultObject::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    trace::QueryGetAttr q{name, std::string{}};
    trace::ResultMaybeType r{
        child ? std::optional<std::string>{objectTypeToString(child->getType())} : std::nullopt};
    recordD2(q, r);
    return child;
}

std::vector<std::string> LambdaApplyResultObject::getAttrNames()
{
    auto names = inner->getAttrNames();
    recordD2(trace::QueryGetAttrNames{std::string{}}, trace::ResultListOfStrings{names});
    return names;
}

std::string LambdaApplyResultObject::getStringIgnoreContext()
{
    auto value = inner->getStringIgnoreContext();
    recordD2(trace::QueryGetString{std::string{}}, trace::ResultString{value});
    return value;
}

std::string LambdaApplyResultObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> LambdaApplyResultObject::getStringWithContext()
{
    auto [str, ctx] = inner->getStringWithContext();
    std::vector<std::string> ctxStrings;
    for (auto & c : ctx)
        ctxStrings.push_back(c.to_string());
    recordD2(
        trace::QueryGetStringWithContext{std::string{}},
        trace::ResultStringWithContext{str, std::move(ctxStrings)});
    return {str, std::move(ctx)};
}

RootedPath LambdaApplyResultObject::getPath()
{
    auto path = inner->getPath();
    recordD2(trace::QueryGetPath{std::string{}}, trace::ResultPath{path.path.abs()});
    return path;
}

bool LambdaApplyResultObject::getBool(std::string_view errorCtx)
{
    auto value = inner->getBool(errorCtx);
    recordD2(trace::QueryGetBool{std::string{}}, trace::ResultBool{value});
    return value;
}

NixInt LambdaApplyResultObject::getInt(std::string_view errorCtx)
{
    auto value = inner->getInt(errorCtx);
    recordD2(trace::QueryGetInt{std::string{}}, trace::ResultInt{value.value});
    return value;
}

NixFloat LambdaApplyResultObject::getFloat(std::string_view errorCtx)
{
    auto value = inner->getFloat(errorCtx);
    recordD2(trace::QueryGetFloat{std::string{}}, trace::ResultFloat{value});
    return value;
}

size_t LambdaApplyResultObject::getListSize()
{
    auto size = inner->getListSize();
    recordD2(trace::QueryGetListSize{std::string{}}, trace::ResultListSize{size});
    return size;
}

std::shared_ptr<Object> LambdaApplyResultObject::getListElem(size_t index)
{
    auto child = inner->getListElem(index);
    auto type = child->getType();
    recordD2(
        trace::QueryGetListElem{std::string{}, index},
        trace::ResultType{objectTypeToString(type)});
    return child;
}

ObjectType LambdaApplyResultObject::getTypeLazy()
{
    /* Delegate to `inner` for the type, but skip the d=2 recording —
       `getType` records the same `QueryGetType` payload, and the
       depth-2 chain has no dedup (= same fact appended twice cancels
       via XOR-fold at flush, breaking AmbientResult). Callers that
       need both `getTypeLazy` and `getType` get exactly one
       observation through the `getType` call. */
    return inner->getTypeLazy();
}

ObjectType LambdaApplyResultObject::getType()
{
    auto type = inner->getType();
    recordD2(trace::QueryGetType{std::string{}}, trace::ResultType{objectTypeToString(type)});
    tracingCacheLog("laro: getType applyCdi=%s type=%s",
        applyCdiHex.substr(0, 16), objectTypeToString(type));
    return type;
}

RootValue LambdaApplyResultObject::defeatCache()
{
    return inner->defeatCache();
}

std::optional<FunctionInfo> LambdaApplyResultObject::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(),
        info ? info->formals : std::map<std::string, bool>{},
        info ? info->ellipsis : false};
    recordD2(trace::QueryGetFunctionInfo{std::string{}}, rfi);
    return info;
}

PosIdx LambdaApplyResultObject::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> LambdaApplyResultObject::getAttrPath()
{
    return inner->getAttrPath();
}

std::shared_ptr<Object> LambdaApplyResultObject::queryApply(std::shared_ptr<Object> argObj)
{
    /* The apply-result is a fresh value crossing the cb-apply boundary
       back to the cached body. Subsequent applies on it go through the
       inner Object's `queryApply` (= delegating to whatever the inner
       Object's apply semantics are). Recording for that apply, if
       needed, is the inner Object's responsibility. */
    return inner->queryApply(std::move(argObj));
}

} // namespace nix

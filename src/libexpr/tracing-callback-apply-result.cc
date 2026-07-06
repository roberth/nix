#include "nix/expr/tracing-callback-apply-result.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/util/error.hh"

namespace nix {

TracingCallbackApplyResult::TracingCallbackApplyResult(
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
    auto scopeStateId = cidasks::scopeStateIdAfter(applyResultSubject, applyScope, {});
    applyScopeStateIdHex = scopeStateId.to_string(HashFormat::Base16, false);
}

void TracingCallbackApplyResult::recordD2(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Route through the depth-2 entry point: every observation on
       this apply-result is grouped with the recursive apply Fact
       under the enclosing cb-apply boundary so that the d=2 chain
       has [recursiveApplyFact, this_obs, next_obs, ...] in the
       order they're appended. flushAmbient's d=2 loop
       stamps each `from` at `edgeIndex = i` (= position in the
       boundary's facts vector), matching the walker's stamping at
       `walkFacts.size()` after the synthetic-side primop pushed
       the apply Fact. */
    writer.logAmbientObservation(query, result, applyResultSubject, applyScope, depth2ApplyId);
}

std::shared_ptr<Object> TracingCallbackApplyResult::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    trace::QueryGetAttr q{name, std::string{}};
    trace::ResultMaybeType r{
        child ? std::optional<std::string>{objectTypeToString(child->getType())} : std::nullopt};
    recordD2(q, r);
    return child;
}

trace::ResultWHNF & TracingCallbackApplyResult::whnf()
{
    if (cachedWHNF)
        return *cachedWHNF;
    auto whnfResult = computeWHNFFromObject(*inner);
    recordD2(trace::QueryGetWHNF{std::string{}}, whnfResult);
    cachedWHNF = std::move(whnfResult);
    return *cachedWHNF;
}

std::vector<std::string> TracingCallbackApplyResult::getAttrNames()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!p)
        throw Error("laro getAttrNames: WHNF payload not attrs (type %s)", w.type);
    return p->names;
}

std::string TracingCallbackApplyResult::getStringIgnoreContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("laro getStringIgnoreContext: WHNF payload not string (type %s)", w.type);
    return p->value;
}

std::string TracingCallbackApplyResult::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> TracingCallbackApplyResult::getStringWithContext()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFString>(&w.payload);
    if (!p)
        throw Error("laro getStringWithContext: WHNF payload not string (type %s)", w.type);
    NixStringContext ctx;
    for (auto & s : p->context)
        ctx.insert(NixStringContextElem::parse(s));
    return {p->value, std::move(ctx)};
}

RootedPath TracingCallbackApplyResult::getPath()
{
    /* WHNF records that this is a path; the actual RootedPath needs the
       inner's SourceRoot. */
    whnf();
    return inner->getPath();
}

bool TracingCallbackApplyResult::getBool(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFBool>(&w.payload);
    if (!p)
        throw Error("laro getBool: WHNF payload not bool (type %s)", w.type);
    return p->value;
}

NixInt TracingCallbackApplyResult::getInt(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFInt>(&w.payload);
    if (!p)
        throw Error("laro getInt: WHNF payload not int (type %s)", w.type);
    return NixInt{p->value};
}

NixFloat TracingCallbackApplyResult::getFloat(std::string_view)
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFFloat>(&w.payload);
    if (!p)
        throw Error("laro getFloat: WHNF payload not float (type %s)", w.type);
    return p->value;
}

size_t TracingCallbackApplyResult::getListSize()
{
    auto & w = whnf();
    auto * p = std::get_if<trace::WHNFList>(&w.payload);
    if (!p)
        throw Error("laro getListSize: WHNF payload not list (type %s)", w.type);
    return p->size;
}

std::shared_ptr<Object> TracingCallbackApplyResult::getListElem(size_t index)
{
    auto child = inner->getListElem(index);
    auto type = child->getType();
    recordD2(
        trace::QueryGetListElem{std::string{}, index},
        trace::ResultType{objectTypeToString(type)});
    return child;
}

ObjectType TracingCallbackApplyResult::getTypeLazy()
{
    /* Delegate to `inner` for the type, but skip the d=2 recording —
       `getType` goes through `whnf()` which records the same
       QueryGetWHNF payload, and the depth-2 chain has no dedup (= same
       fact appended twice cancels via XOR-fold at flush, breaking
       AmbientResult). Callers that need both `getTypeLazy` and
       `getType` get exactly one observation through the `getType`
       call. */
    return inner->getTypeLazy();
}

ObjectType TracingCallbackApplyResult::getType()
{
    auto type = stringToObjectType(whnf().type);
    tracingCacheLog("laro: getType applyScopeStateId=%s type=%s",
        applyScopeStateIdHex.substr(0, 16), objectTypeToString(type));
    return type;
}

RootValue TracingCallbackApplyResult::defeatCache()
{
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingCallbackApplyResult::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(),
        info ? info->formals : std::map<std::string, bool>{},
        info ? info->ellipsis : false};
    recordD2(trace::QueryGetFunctionInfo{std::string{}}, rfi);
    return info;
}

PosIdx TracingCallbackApplyResult::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingCallbackApplyResult::getAttrPath()
{
    return inner->getAttrPath();
}

std::shared_ptr<Object> TracingCallbackApplyResult::queryApply(std::shared_ptr<Object> argObj)
{
    /* The apply-result is a fresh value crossing the cb-apply boundary
       back to the cached body. Subsequent applies on it go through the
       inner Object's `queryApply` (= delegating to whatever the inner
       Object's apply semantics are). Recording for that apply, if
       needed, is the inner Object's responsibility. */
    return inner->queryApply(std::move(argObj));
}

} // namespace nix

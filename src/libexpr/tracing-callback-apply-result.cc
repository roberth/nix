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
    Subject applyResultSubject_,
    Hash applyScope_,
    Hash applyId_)
    : inner(std::move(inner_))
    , writer(writer_)
    , applyResultSubject(std::move(applyResultSubject_))
    , applyArgAncestry(std::move(applyScope_))
    , applyId(std::move(applyId_))
{
    auto stateHash = stateHashAfter(applyResultSubject, applyArgAncestry, {});
    applyArgAncestryStateHashHex = stateHash.to_string(HashFormat::Base16, false);
}

void TracingCallbackApplyResult::recordD2(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Route every observation on this apply-result into the enclosing
       CallbackCell's runningObsSet via logCallbackObservation. The
       observations are later snapshotted (by value) into an
       ObservationSet referenced from a QueryCallbackApply request.
       See tracing-cache-callback-model.md for the recording
       protocol. */
    writer.logCallbackObservation(query, result, applyResultSubject, applyArgAncestry, applyId);
}

std::shared_ptr<Object> TracingCallbackApplyResult::maybeGetAttr(const std::string & name)
{
    /* Existence is projected from parent WHNFAttrs.names; only when
       present do we record the pure-retrieval QueryGetAttr with the
       child's WHNF. Absence still requires a whnf recording so the
       apply-result's WHNFAttrs.names is on the trace — that's what
       future warm replays will project membership from. */
    auto & w = whnf();
    auto * ap = std::get_if<trace::WHNFAttrs>(&w.payload);
    if (!ap)
        /* Not an attrs — delegate so inner throws its
           source-positioned "getAttr on non-set" error. */
        return inner->maybeGetAttr(name);
    if (std::find(ap->names.begin(), ap->names.end(), name) == ap->names.end())
        return nullptr;
    auto child = inner->maybeGetAttr(name);
    if (!child)
        return nullptr;
    trace::QueryGetAttr q{name, std::string{}};
    auto childWHNF = computeWHNFFromObject(*child);
    recordD2(q, childWHNF);
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
    /* Bounds are projected from parent WHNFList.size; retrieval is
       QueryGetListElem returning the child's WHNF. */
    auto & w = whnf();
    auto * lp = std::get_if<trace::WHNFList>(&w.payload);
    if (!lp || index >= lp->size)
        /* Not a list, or index out of bounds — delegate so inner
           throws the source-positioned error. */
        return inner->getListElem(index);
    auto child = inner->getListElem(index);
    recordD2(
        trace::QueryGetListElem{std::string{}, index},
        computeWHNFFromObject(*child));
    return child;
}

ObjectType TracingCallbackApplyResult::getTypeLazy()
{
    /* Delegate to `inner` for the type without recording — `getType`
       goes through `whnf()` which records the same QueryGetWHNF
       payload; recording here too would produce a duplicate. Callers
       that need both `getTypeLazy` and `getType` get exactly one
       observation through the `getType` call. */
    return inner->getTypeLazy();
}

ObjectType TracingCallbackApplyResult::getType()
{
    auto type = stringToObjectType(whnf().type);
    tracingCacheLog("laro: getType applyArgAncestryStateHash=%s type=%s",
        applyArgAncestryStateHashHex.substr(0, 16), objectTypeToString(type));
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
    /* The apply-result is a fresh value crossing the cb-apply
       back to the cached body. Subsequent applies on it go through the
       inner Object's `queryApply` (= delegating to whatever the inner
       Object's apply semantics are). Recording for that apply, if
       needed, is the inner Object's responsibility. */
    return inner->queryApply(std::move(argObj));
}

} // namespace nix

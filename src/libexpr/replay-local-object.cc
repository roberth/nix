#include "nix/expr/replay-local-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/error.hh"

namespace nix {

/* These mirror the same-named helpers in tracing-local-object.cc.
   Both translation units are unity-built into libnixexpr, so the
   helpers must have distinct names to avoid ODR collisions. */
static std::string replayFromOf(AmbientId id)
{
    return id.to_string(HashFormat::Base16, false);
}

template<typename Q>
static AmbientId replayDerivedLocalId(const Q & query)
{
    return TracingDecisionGraph::computeQueryHash(query);
}

template<typename Q>
static nlohmann::json readResponse(TracingDecisionGraph & dg, const Q & query)
{
    auto reqHash = TracingDecisionGraph::computeQueryHash(query);
    auto payload = dg.getResponsePayload(reqHash);
    if (!payload)
        throw Error("ReplayLocalObject: no recorded response for %s on local %s",
            Q::tag, query.from);
    return cborStringToJson(*payload);
}

std::shared_ptr<Object> ReplayLocalObject::maybeGetAttr(const std::string & name)
{
    trace::QueryGetAttr query{name, replayFromOf(localId)};
    auto rJson = readResponse(decisionGraph, query);
    trace::ResultMaybeType r = rJson;
    if (!r.type)
        return nullptr;
    return std::make_shared<ReplayLocalObject>(replayDerivedLocalId(query), decisionGraph, rootFSRoot);
}

std::vector<std::string> ReplayLocalObject::getAttrNames()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetAttrNames{replayFromOf(localId)});
    trace::ResultListOfStrings r = rJson;
    return r.values;
}

std::string ReplayLocalObject::getStringIgnoreContext()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetString{replayFromOf(localId)});
    trace::ResultString r = rJson;
    return r.value;
}

std::string ReplayLocalObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> ReplayLocalObject::getStringWithContext()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetStringWithContext{replayFromOf(localId)});
    trace::ResultStringWithContext r = rJson;
    /* Context strings were serialised as raw strings; rebuild a
       (possibly-empty) NixStringContext placeholder. The replayed
       outer doesn't need to interpret context elements beyond their
       string form. */
    NixStringContext ctx;
    for (auto & s : r.context)
        ctx.insert(NixStringContextElem::parse(s));
    return {r.value, std::move(ctx)};
}

RootedPath ReplayLocalObject::getPath()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetPath{replayFromOf(localId)});
    trace::ResultPath r = rJson;
    return RootedPath{rootFSRoot, CanonPath{r.path}};
}

bool ReplayLocalObject::getBool(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetBool{replayFromOf(localId)});
    trace::ResultBool r = rJson;
    return r.value;
}

NixInt ReplayLocalObject::getInt(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetInt{replayFromOf(localId)});
    trace::ResultInt r = rJson;
    return NixInt{r.value};
}

NixFloat ReplayLocalObject::getFloat(std::string_view)
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetFloat{replayFromOf(localId)});
    trace::ResultFloat r = rJson;
    return r.value;
}

size_t ReplayLocalObject::getListSize()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetListSize{replayFromOf(localId)});
    trace::ResultListSize r = rJson;
    return r.size;
}

std::shared_ptr<Object> ReplayLocalObject::getListElem(size_t index)
{
    trace::QueryGetListElem query{replayFromOf(localId), index};
    /* The recorded response only carries the child's type, not
       value. We still derive an id for the child so the outer can
       chain further accesses on it. */
    readResponse(decisionGraph, query);
    return std::make_shared<ReplayLocalObject>(replayDerivedLocalId(query), decisionGraph, rootFSRoot);
}

ObjectType ReplayLocalObject::getType()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetType{replayFromOf(localId)});
    trace::ResultType r = rJson;
    return stringToObjectType(r.type);
}

ObjectType ReplayLocalObject::getTypeLazy()
{
    return getType();
}

RootValue ReplayLocalObject::defeatCache()
{
    /* No live Value to defeatCache to. Callers that need a Value
       should bridge via ExprFromObject (handled by Interpreter::apply's
       try/catch for virtual Objects). */
    throw Error("ReplayLocalObject::defeatCache: no live Value (this is a recorded frozen image)");
}

std::optional<FunctionInfo> ReplayLocalObject::getFunctionInfo()
{
    auto rJson = readResponse(decisionGraph, trace::QueryGetFunctionInfo{replayFromOf(localId)});
    trace::ResultFunctionInfo r = rJson;
    if (!r.hasInfo)
        return std::nullopt;
    return FunctionInfo{r.formals, r.ellipsis};
}

} // namespace nix

#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/object-type.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/source-accessor.hh"

namespace nix {

/* Same-name helper exists in ambient-object.cc; both translation
   units are unity-built into libnixexpr, so the helpers must have
   distinct names to avoid ODR collisions. */
static std::string tracingLocalFromOf(AmbientId id)
{
    return id.to_string(HashFormat::Base16, false);
}

TracingLocalObject::TracingLocalObject(
    std::shared_ptr<Object> inner,
    AmbientId localId,
    TracingWriter & writer,
    ref<SourceRoot> rootFSRoot,
    std::shared_ptr<const ArgScopeCell> argScope)
    : inner(std::move(inner))
    , localId(localId)
    , writer(writer)
    , rootFSRoot(std::move(rootFSRoot))
    , argScope(std::move(argScope))
{
}

/* Derived local id for children produced by child-producing queries.
   Mirrors the outgoing-side convention: id = producer query's
   queryHash. On replay the dispatcher reads the recorded response
   payload directly from the Responses pool (the inner isn't running
   to recompute against), so the producer chain is only used for the
   QueryGetAttr/QueryGetListElem case where a child Fact's `from`
   refers to a derived id. */
template<typename Q>
static AmbientId derivedLocalId(const Q & query)
{
    return TracingDecisionGraph::computeQueryHash(query);
}

std::shared_ptr<Object> TracingLocalObject::maybeGetAttr(const std::string & name)
{
    auto child = inner->maybeGetAttr(name);
    trace::QueryGetAttr query{name, tracingLocalFromOf(localId)};
    auto resultJson = child
        ? trace::ResultMaybeType{std::optional{objectTypeToString(child->getType())}}
        : trace::ResultMaybeType{std::nullopt};
    recordObservation(query, resultJson);
    if (!child)
        return nullptr;
    /* The child is structurally derived from the parent via this
       query. Register the derivation with the writer so flush can
       compute the child's final localId from parent's final
       intrinsic — the same hash replay computes from the parent
       standin's localId. The query template carries the parent's
       placeholder hex; substitution at flush rewrites it to the
       parent's final intrinsic, then the child's final localId is
       the hash of that substituted query. */
    auto childLocalId = derivedLocalId(query);
    /* Navigation child shares the parent's cell: observations on
       descendants contribute to the same scope's intrinsic
       (state creep). */
    return std::make_shared<TracingLocalObject>(std::move(child), childLocalId, writer, rootFSRoot, argScope);
}

std::vector<std::string> TracingLocalObject::getAttrNames()
{
    auto names = inner->getAttrNames();
    recordObservation(
        trace::QueryGetAttrNames{tracingLocalFromOf(localId)}, trace::ResultListOfStrings{names});
    return names;
}

std::string TracingLocalObject::getStringIgnoreContext()
{
    auto value = inner->getStringIgnoreContext();
    recordObservation(trace::QueryGetString{tracingLocalFromOf(localId)}, trace::ResultString{value});
    return value;
}

std::string TracingLocalObject::getStringWithoutContext()
{
    return getStringIgnoreContext();
}

std::pair<std::string, NixStringContext> TracingLocalObject::getStringWithContext()
{
    auto [str, ctx] = inner->getStringWithContext();
    std::vector<std::string> ctxStrings;
    for (auto & c : ctx)
        ctxStrings.push_back(c.to_string());
    recordObservation(
        trace::QueryGetStringWithContext{tracingLocalFromOf(localId)},
        trace::ResultStringWithContext{str, std::move(ctxStrings)});
    return {str, std::move(ctx)};
}

RootedPath TracingLocalObject::getPath()
{
    auto path = inner->getPath();
    recordObservation(trace::QueryGetPath{tracingLocalFromOf(localId)}, trace::ResultPath{path.path.abs()});
    /* lazy-paths: reuse the cached SourceRoot so the path outlives the
       returned RootedPath. */
    return RootedPath{rootFSRoot, path.path};
}

bool TracingLocalObject::getBool(std::string_view)
{
    auto value = inner->getBool();
    recordObservation(trace::QueryGetBool{tracingLocalFromOf(localId)}, trace::ResultBool{value});
    return value;
}

NixInt TracingLocalObject::getInt(std::string_view)
{
    auto value = inner->getInt();
    recordObservation(trace::QueryGetInt{tracingLocalFromOf(localId)}, trace::ResultInt{value.value});
    return value;
}

NixFloat TracingLocalObject::getFloat(std::string_view)
{
    auto value = inner->getFloat();
    recordObservation(trace::QueryGetFloat{tracingLocalFromOf(localId)}, trace::ResultFloat{value});
    return value;
}

size_t TracingLocalObject::getListSize()
{
    auto size = inner->getListSize();
    recordObservation(trace::QueryGetListSize{tracingLocalFromOf(localId)}, trace::ResultListSize{size});
    return size;
}

std::shared_ptr<Object> TracingLocalObject::getListElem(size_t index)
{
    auto child = inner->getListElem(index);
    trace::QueryGetListElem query{tracingLocalFromOf(localId), index};
    recordObservation(query, trace::ResultType{objectTypeToString(child->getType())});
    auto childLocalId = derivedLocalId(query);
    /* Navigation child shares the parent's cell: observations on
       descendants contribute to the same scope's intrinsic
       (state creep). */
    return std::make_shared<TracingLocalObject>(std::move(child), childLocalId, writer, rootFSRoot, argScope);
}

ObjectType TracingLocalObject::getTypeLazy()
{
    return getType();
}

ObjectType TracingLocalObject::getType()
{
    auto type = inner->getType();
    recordObservation(
        trace::QueryGetType{tracingLocalFromOf(localId)}, trace::ResultType{objectTypeToString(type)});
    return type;
}

RootValue TracingLocalObject::defeatCache()
{
    /* Pass through unrecorded. defeatCache yields a concrete RootValue
       (no observable side effects), and there's no incoming-Fact shape
       for "I gave you my underlying value." */
    return inner->defeatCache();
}

std::optional<FunctionInfo> TracingLocalObject::getFunctionInfo()
{
    auto info = inner->getFunctionInfo();
    trace::ResultFunctionInfo rfi{
        info.has_value(), info ? info->formals : std::map<std::string, bool>{}, info ? info->ellipsis : false};
    recordObservation(trace::QueryGetFunctionInfo{tracingLocalFromOf(localId)}, rfi);
    return info;
}

PosIdx TracingLocalObject::getPos()
{
    return inner->getPos();
}

std::optional<std::vector<std::string>> TracingLocalObject::getAttrPath()
{
    return inner->getAttrPath();
}

void TracingLocalObject::recordObservation(const trace::QueryVariant & query, const trace::ResultVariant & result)
{
    /* Hash the query with `from` blanked: the observation's
       contribution to the cell's intrinsic depends only on the
       observation's content, not on which placeholder this local
       holds. This is what makes §2 same-shape collapse work —
       extensionally-equivalent locals get identical intrinsics. */
    nlohmann::json queryJson;
    std::visit([&](const auto & q) { queryJson = q; }, query);
    if (queryJson.is_object() && queryJson.contains("params")) {
        auto & params = queryJson["params"];
        if (params.is_object() && params.contains("from"))
            params["from"] = "";
    }
    auto queryHashBlanked = hashString(HashAlgorithm::SHA256, queryJson.dump());

    nlohmann::json resultJson;
    std::visit([&](const auto & r) { resultJson = r; }, result);
    auto responseHash = TracingDecisionGraph::computeResponseHash(jsonToCborString(resultJson));

    /* CDI fix: stop mutating cell.intrinsic and stop pushing to
       placeholderToIntrinsic. Under the new design (see
       doc/design/tracing-eval-cache-content-identity-via-asks.md),
       content ids are pure functions of (subject, factset); the
       cell isn't where they live. */
    writer.logAmbientInteraction(query, result);
}

} // namespace nix

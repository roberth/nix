#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"
#include "nix/expr/object-type.hh"

#include <nlohmann/json.hpp>
#include <set>

namespace nix {

TracingReplayObject::TracingReplayObject(
    TracingReplayEvaluator & evaluator, TriePosition triePos, std::function<ref<Object>()> getInner)
    : evaluator(evaluator)
    , triePos(triePos)
    , getInner(std::move(getInner))
{
}

ref<Object> TracingReplayObject::ensureInner() const
{
    if (!inner) {
        tracingCacheLog("replay fallback: activating inner");
        tracingCacheStats().fallbacks++;
        inner = getInner();
    }
    return *inner;
}

std::string TracingReplayObject::evolvedQueryFrom() const
{
    if (applyResultSubject && applyContext) {
        std::vector<cidasks::Edge> walk;
        walk.reserve(applyContext->observations.size());
        for (auto & obs : applyContext->observations) {
            cidasks::Edge edge;
            edge.observations.push_back(obs);
            walk.push_back(std::move(edge));
        }
        auto evolved = cidasks::contentIdAt(*applyResultSubject, applyScope, walk, walk.size());
        auto hex = evolved.to_string(HashFormat::Base16, false);
        return hex;
    }
    return triePos.queryHashStr;
}

void TracingReplayObject::pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash)
{
    if (!applyContext) return;
    Hash fromHash{HashAlgorithm::SHA256};
    try {
        fromHash = Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);
    } catch (...) {
        return;
    }
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), queryHash, responseHash);
    applyContext->observations.push_back({fromHash, elementHash});
}

/**
 * Cascading Lookup Strategy (see doc/tracing-index-data-model.md)
 *
 * For each lookup, we try three strategies in order:
 *
 * 1. **Trie following** — temporal children whose afterHash equals our result.
 *    Fastest when the access pattern matches the recorded order.
 *    Validates incrementally from our known-valid position.
 *
 * 2. **Structural lookup** — structural children whose structuralParent equals
 *    our result. Handles same operations in different order.
 *    Validates incrementally from our known-valid position.
 *
 * 3. **Shortcut lookup** — global shortcut table keyed by queryHash.
 *    Can switch to entirely different traces.
 *    Requires full validation from root.
 */
template<typename Q, typename R>
std::optional<std::pair<R, Hash>> TracingReplayObject::lookupResult(const Q & query) const
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog("walker lookup: %s Q=%s",
                    Q::tag,
                    queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
    auto v13 = evaluator.v13Walk(queryHash, const_cast<TracingReplayObject *>(this)->shared_from_this());
    if (!v13) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(v13->first);
        tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
        return std::make_pair(j.template get<R>(), v13->second);
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: v13 payload parse failed: %s", e.what());
        return std::nullopt;
    }
}

template<typename Q, typename R>
std::optional<std::pair<R, TriePosition>> TracingReplayObject::lookupStructuralChild(const Q & query) const
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    tracingCacheLog("walker lookup: %s Q=%s",
                    Q::tag,
                    queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
    auto v13 = evaluator.v13Walk(queryHash, const_cast<TracingReplayObject *>(this)->shared_from_this());
    if (!v13) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(v13->first);
        tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
        return std::make_pair(
            j.template get<R>(),
            TriePosition{
                .resultNodeHash = v13->second,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            });
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: v13 payload parse failed: %s", e.what());
        return std::nullopt;
    }
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttr query{name, parentHash};

    if (auto result = lookupStructuralChild<trace::QueryGetAttr, trace::ResultMaybeType>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), result->second.resultNodeHash);
        if (!result->first.type) {
            tracingCacheLog("replay hit: getAttr '%s' -> missing", name);
            return nullptr;
        }

        tracingCacheLog("replay hit: getAttr '%s' -> found", name);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        auto child = std::make_shared<TracingReplayObject>(
            evaluator, result->second, [self, name]() { return ref<Object>(self->ensureInner()->maybeGetAttr(name)); });
        child->withScope(argScope);
        if (applyContext) child->withApplyContextOnly(applyContext);
        return child;
    }

    tracingCacheLog("replay fallback: maybeGetAttr '%s'", name);
    return ensureInner()->maybeGetAttr(name);
}

std::vector<std::string> TracingReplayObject::getAttrNames()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetAttrNames query{parentHash};
    if (auto r = lookupResult<trace::QueryGetAttrNames, trace::ResultListOfStrings>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.values;
    }
    tracingCacheLog("replay fallback: getAttrNames");
    return ensureInner()->getAttrNames();
}

std::string TracingReplayObject::getStringIgnoreContext()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetString query{parentHash};
    if (auto r = lookupResult<trace::QueryGetString, trace::ResultString>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.value;
    }
    tracingCacheLog("replay fallback: getStringIgnoreContext");
    return ensureInner()->getStringIgnoreContext();
}

std::string TracingReplayObject::getStringWithoutContext()
{
    // getStringWithoutContext checks for empty context which the cache doesn't track
    tracingCacheLog("replay fallback: getStringWithoutContext");
    return ensureInner()->getStringWithoutContext();
}

std::pair<std::string, NixStringContext> TracingReplayObject::getStringWithContext()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetStringWithContext query{parentHash};
    if (auto r = lookupResult<trace::QueryGetStringWithContext, trace::ResultStringWithContext>(query)) {
        NixStringContext ctx;
        for (const auto & s : r->first.context)
            ctx.insert(NixStringContextElem::parse(s));

        auto & store = evaluator.getStore();
        for (const auto & elem : ctx) {
            const StorePath & path = std::visit(
                overloaded{
                    [&](const NixStringContextElem::DrvDeep & d) -> const StorePath & { return d.drvPath; },
                    [&](const NixStringContextElem::Built & b) -> const StorePath & {
                        return b.drvPath->getBaseStorePath();
                    },
                    [&](const NixStringContextElem::Opaque & o) -> const StorePath & { return o.path; },
                },
                elem.raw);
            if (!store.isValidPath(path)) {
                tracingCacheLog("replay miss: context path %s no longer valid", store.printStorePath(path));
                tracingCacheLog("replay fallback: getStringWithContext (invalid context)");
                return ensureInner()->getStringWithContext();
            }
        }
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return {r->first.value, std::move(ctx)};
    }
    tracingCacheLog("replay fallback: getStringWithContext");
    return ensureInner()->getStringWithContext();
}

RootedPath TracingReplayObject::getPath()
{
    tracingCacheLog("replay fallback: getPath");
    return ensureInner()->getPath();
}

bool TracingReplayObject::getBool(std::string_view errorCtx)
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetBool query{parentHash};
    if (auto r = lookupResult<trace::QueryGetBool, trace::ResultBool>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.value;
    }
    tracingCacheLog("replay fallback: getBool");
    return ensureInner()->getBool(errorCtx);
}

NixInt TracingReplayObject::getInt(std::string_view errorCtx)
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetInt query{parentHash};
    if (auto r = lookupResult<trace::QueryGetInt, trace::ResultInt>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return NixInt{r->first.value};
    }
    tracingCacheLog("replay fallback: getInt");
    return ensureInner()->getInt(errorCtx);
}

NixFloat TracingReplayObject::getFloat(std::string_view errorCtx)
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetFloat query{parentHash};
    if (auto r = lookupResult<trace::QueryGetFloat, trace::ResultFloat>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.value;
    }
    tracingCacheLog("replay fallback: getFloat");
    return ensureInner()->getFloat(errorCtx);
}

size_t TracingReplayObject::getListSize()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListSize query{parentHash};
    if (auto r = lookupResult<trace::QueryGetListSize, trace::ResultListSize>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.size;
    }
    tracingCacheLog("replay fallback: getListSize");
    return ensureInner()->getListSize();
}

std::shared_ptr<Object> TracingReplayObject::getListElem(size_t idx)
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListElem query{parentHash, idx};

    if (auto result = lookupStructuralChild<trace::QueryGetListElem, trace::ResultType>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), result->second.resultNodeHash);
        tracingCacheLog("replay hit: getListElem %d", idx);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        auto child = std::make_shared<TracingReplayObject>(
            evaluator, result->second, [self, idx]() { return ref<Object>(self->ensureInner()->getListElem(idx)); });
        child->withScope(argScope);
        if (applyContext) child->withApplyContextOnly(applyContext);
        return child;
    }

    tracingCacheLog("replay fallback: getListElem %d", idx);
    return ensureInner()->getListElem(idx);
}

std::vector<std::string> TracingReplayObject::getListOfStringsNoCtx()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetListOfStrings query{parentHash};
    if (auto r = lookupResult<trace::QueryGetListOfStrings, trace::ResultListOfStrings>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return r->first.values;
    }
    tracingCacheLog("replay fallback: getListOfStringsNoCtx");
    return ensureInner()->getListOfStringsNoCtx();
}

ObjectType TracingReplayObject::getTypeLazy()
{
    return getType();
}

ObjectType TracingReplayObject::getType()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetType query{parentHash};
    if (auto r = lookupResult<trace::QueryGetType, trace::ResultType>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        return stringToObjectType(r->first.type);
    }
    tracingCacheLog("replay fallback: getType (from=%s)", triePos.queryHashStr);
    return ensureInner()->getType();
}

RootValue TracingReplayObject::defeatCache()
{
    tracingCacheLog("replay fallback: defeatCache");
    return ensureInner()->defeatCache();
}

std::optional<FunctionInfo> TracingReplayObject::getFunctionInfo()
{
    auto parentHash = evolvedQueryFrom();
    trace::QueryGetFunctionInfo query{parentHash};
    if (auto r = lookupResult<trace::QueryGetFunctionInfo, trace::ResultFunctionInfo>(query)) {
        pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
        if (!r->first.hasInfo)
            return std::nullopt;
        return FunctionInfo{.formals = r->first.formals, .ellipsis = r->first.ellipsis};
    }
    tracingCacheLog("replay fallback: getFunctionInfo");
    return ensureInner()->getFunctionInfo();
}

std::shared_ptr<Object> TracingReplayObject::queryApply(std::shared_ptr<Object> argObj)
{
    /* Object-method counterpart of TracingReplayEvaluator::apply.
       Delegates to the evaluator so walker lookup + the lazy
       getInner callback are preserved — callers can route apply
       through queryApply uniformly without losing the caching
       layer. */
    auto self = std::const_pointer_cast<TracingReplayObject>(
        std::static_pointer_cast<const TracingReplayObject>(shared_from_this()));
    return evaluator.apply(ref<Object>(self), ref<Object>(argObj)).get_ptr();
}

} // namespace nix

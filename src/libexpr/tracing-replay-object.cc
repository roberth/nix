#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-object.hh"
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
    /* If inner has been activated to a TracingObject, share its
       applyContext for evolvedQueryFrom's computation — inner's
       applyContext gets populated by TracingObject's own maybeGetAttr
       / whnf / etc. as evaluation flows through it. TRO's own
       applyContext only grows on cache HITS (via pushObservation from
       maybeGetAttr's shallow/deep-lookup path); the two contexts
       diverge whenever TRO falls through to inner. Sharing gives TRO
       the same evolved from-hash cold's TracingObject would compute
       at analogous points — the cb-sibling discrimination path
       requires this alignment so warm's Q hashes match cold's stored
       Q hashes for the derived .whatever queries. */
    if (applyResultSubject && inner) {
        if (auto * innerT = dynamic_cast<TracingObject *>(inner->get_ptr().get())) {
            if (auto innerCtx = innerT->getApplyContext()) {
                std::vector<Edge> walk;
                walk.reserve(innerCtx->observations.size());
                for (auto & obs : innerCtx->observations) {
                    Edge edge;
                    edge.observations.push_back(obs);
                    walk.push_back(std::move(edge));
                }
                auto evolved = scopeStateIdAt(*applyResultSubject, applyArgAncestry, walk, walk.size());
                return evolved.to_string(HashFormat::Base16, false);
            }
        }
    }
    if (applyResultSubject && applyContext) {
        std::vector<Edge> walk;
        walk.reserve(applyContext->observations.size());
        for (auto & obs : applyContext->observations) {
            Edge edge;
            edge.observations.push_back(obs);
            walk.push_back(std::move(edge));
        }
        auto evolved = scopeStateIdAt(*applyResultSubject, applyArgAncestry, walk, walk.size());
        auto hex = evolved.to_string(HashFormat::Base16, false);
        return hex;
    }
    return triePos.queryHashStr;
}

std::vector<std::string> TracingReplayObject::parentHashCandidates() const
{
    std::vector<std::string> out;
    if (applyResultSubject && applyContext) {
        out.push_back(evolvedQueryFrom());
        /* If we've completed our WHNF probe, query at the post-WHNF
           prefix too. IDs are consistent with facts: a smaller fact
           set produces different IDs that align with recordings made
           at that state. Useful for composition of independent
           warmups where each warmup probed one attr after WHNF —
           sibling attrs in those warmups recorded at this prefix
           regardless of which warmup wrote them. */
        if (postWHNFObservationCount && *postWHNFObservationCount < applyContext->observations.size()) {
            std::vector<Edge> walk;
            walk.reserve(*postWHNFObservationCount);
            for (size_t i = 0; i < *postWHNFObservationCount; ++i) {
                Edge edge;
                edge.observations.push_back(applyContext->observations[i]);
                walk.push_back(std::move(edge));
            }
            auto snap = scopeStateIdAt(*applyResultSubject, applyArgAncestry, walk, walk.size());
            auto snapHex = snap.to_string(HashFormat::Base16, false);
            if (snapHex != out.front())
                out.push_back(snapHex);
        }
    } else {
        out.push_back(triePos.queryHashStr);
    }
    return out;
}

void TracingReplayObject::pushObservation(const std::string & fromHex, const Hash & queryHash, const Hash & responseHash)
{
    Hash fromHash{HashAlgorithm::SHA256};
    try {
        fromHash = Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);
    } catch (...) {
        return;
    }
    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), queryHash, responseHash);
    /* Mirror evolvedQueryFrom's inner-first preference: if inner is
       an activated TracingObject with an applyContext, push into IT
       so both TRO and cold's TracingObject share the same evolution
       trajectory. Falls back to TRO's own applyContext when inner
       isn't a TracingObject or hasn't been activated. */
    if (inner) {
        if (auto * innerT = dynamic_cast<TracingObject *>(inner->get_ptr().get())) {
            if (auto innerCtx = innerT->getApplyContext()) {
                innerCtx->observations.push_back({fromHash, elementHash});
                return;
            }
        }
    }
    if (applyContext)
        applyContext->observations.push_back({fromHash, elementHash});
}

template<typename Q, typename R>
std::optional<std::pair<R, Hash>> TracingReplayObject::lookupResult(const Q & query) const
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    nlohmann::json qj = query;
    tracingCacheLog("walker lookup: %s Q=%s queryJSON=%s",
                    Q::tag,
                    queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    qj.dump());
    auto walkResult = evaluator.walk(queryHash, const_cast<TracingReplayObject *>(this)->shared_from_this());
    if (!walkResult) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(walkResult->payload);
        tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
        return std::make_pair(j.template get<R>(), walkResult->resultNodeHash);
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: v13 payload parse failed: %s", e.what());
        return std::nullopt;
    }
}

template<typename Q, typename R>
std::optional<std::pair<R, TriePosition>> TracingReplayObject::lookupStructuralChild(const Q & query) const
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    nlohmann::json qj = query;
    tracingCacheLog("walker lookup: %s Q=%s queryJSON=%s",
                    Q::tag,
                    queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    qj.dump());
    auto walkResult = evaluator.walk(queryHash, const_cast<TracingReplayObject *>(this)->shared_from_this());
    if (!walkResult) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        queryHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(walkResult->payload);
        tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
        return std::make_pair(
            j.template get<R>(),
            TriePosition{
                .resultNodeHash = walkResult->resultNodeHash,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
                .factSetHash = walkResult->terminalCur,
            });
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: v13 payload parse failed: %s", e.what());
        return std::nullopt;
    }
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    for (auto & parentHash : parentHashCandidates()) {
        trace::QueryGetAttr query{name, parentHash};
        auto result = lookupStructuralChild<trace::QueryGetAttr, trace::ResultMaybeType>(query);
        if (!result)
            continue;
        auto shallowQueryHash = TracingDecisionGraph::computeQueryHash(query);
        auto shallowResp = result->second.resultNodeHash;
        auto shallowResult = std::move(*result);

        /* Speculative deeper lookup: if we pushed this getAttr's
           observation, evolvedQueryFrom would advance one step. Try
           looking up at that deeper Q too — sibling recordings made
           after a shallower recording share the same Q chain but
           evolve one obs deeper (see cb-sibling-discrimination-via-
           observation). If the deeper lookup hits AND its chain
           validates against live env, prefer it. */
        if (applyResultSubject && applyContext) {
            std::vector<Edge> specWalk;
            specWalk.reserve(applyContext->observations.size() + 1);
            for (auto & obs : applyContext->observations) {
                Edge edge;
                edge.observations.push_back(obs);
                specWalk.push_back(std::move(edge));
            }
            Observation specObs{
                Hash::parseNonSRIUnprefixed(parentHash, HashAlgorithm::SHA256),
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), shallowQueryHash, shallowResp)};
            Edge specEdge;
            specEdge.observations.push_back(specObs);
            specWalk.push_back(std::move(specEdge));
            auto deepFrom = scopeStateIdAt(
                *applyResultSubject, applyArgAncestry, specWalk, specWalk.size());
            auto deepFromHex = deepFrom.to_string(HashFormat::Base16, false);
            if (deepFromHex != parentHash) {
                trace::QueryGetAttr deepQuery{name, deepFromHex};
                if (auto deep = lookupStructuralChild<trace::QueryGetAttr, trace::ResultMaybeType>(deepQuery)) {
                    /* Deeper hit validated (walk's chain traversal
                       returned success). Use it — this is a
                       sibling-specific recording. */
                    auto deepQueryHash = TracingDecisionGraph::computeQueryHash(deepQuery);
                    pushObservation(deepFromHex, deepQueryHash, deep->second.resultNodeHash);
                    if (!deep->first.type) {
                        tracingCacheLog("replay hit: getAttr '%s' -> missing (deeper)", name);
                        return nullptr;
                    }
                    tracingCacheLog("replay hit: getAttr '%s' -> found (deeper)", name);
                    auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
                    auto child = std::make_shared<TracingReplayObject>(
                        evaluator, deep->second, [self, name]() { return ref<Object>(self->ensureInner()->maybeGetAttr(name)); });
                    child->withScope(argCell);
                    if (applyContext) child->withApplyContextOnly(applyContext);
                    return child;
                }
            }
        }

        /* Shallower hit — either speculation wasn't applicable or
           the deeper Q missed / diverged. */
        pushObservation(parentHash, shallowQueryHash, shallowResp);
        if (!shallowResult.first.type) {
            tracingCacheLog("replay hit: getAttr '%s' -> missing", name);
            return nullptr;
        }
        tracingCacheLog("replay hit: getAttr '%s' -> found", name);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        auto child = std::make_shared<TracingReplayObject>(
            evaluator, shallowResult.second, [self, name]() { return ref<Object>(self->ensureInner()->maybeGetAttr(name)); });
        child->withScope(argCell);
        if (applyContext) child->withApplyContextOnly(applyContext);
        return child;
    }
    tracingCacheLog("replay fallback: maybeGetAttr '%s'", name);
    return ensureInner()->maybeGetAttr(name);
}

std::optional<const trace::ResultWHNF *> TracingReplayObject::whnf()
{
    if (cachedWHNF)
        return &*cachedWHNF;
    for (auto & parentHash : parentHashCandidates()) {
        trace::QueryGetWHNF query{parentHash};
        auto r = lookupResult<trace::QueryGetWHNF, trace::ResultWHNF>(query);
        if (!r)
            continue;
        auto shallowQueryHash = TracingDecisionGraph::computeQueryHash(query);
        auto shallowResp = r->second;

        /* Speculative deeper lookup. Same mechanism as maybeGetAttr.
           Only fires when the current apply-result's evolvedQueryFrom
           depends on observations (i.e. applyContext with obs); if
           there are no obs, deeper == shallow, no lookup needed. */
        if (applyResultSubject && applyContext && !applyContext->observations.empty()) {
            std::vector<Edge> specWalk;
            specWalk.reserve(applyContext->observations.size() + 1);
            for (auto & obs : applyContext->observations) {
                Edge edge;
                edge.observations.push_back(obs);
                specWalk.push_back(std::move(edge));
            }
            Observation specObs{
                Hash::parseNonSRIUnprefixed(parentHash, HashAlgorithm::SHA256),
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), shallowQueryHash, shallowResp)};
            Edge specEdge;
            specEdge.observations.push_back(specObs);
            specWalk.push_back(std::move(specEdge));
            auto deepFrom = scopeStateIdAt(
                *applyResultSubject, applyArgAncestry, specWalk, specWalk.size());
            auto deepFromHex = deepFrom.to_string(HashFormat::Base16, false);
            if (deepFromHex != parentHash) {
                trace::QueryGetWHNF deepQuery{deepFromHex};
                if (auto deep = lookupResult<trace::QueryGetWHNF, trace::ResultWHNF>(deepQuery)) {
                    auto deepQueryHash = TracingDecisionGraph::computeQueryHash(deepQuery);
                    pushObservation(deepFromHex, deepQueryHash, deep->second);
                    if (applyContext)
                        postWHNFObservationCount = applyContext->observations.size();
                    cachedWHNF = std::move(deep->first);
                    return &*cachedWHNF;
                }
            }
        }

        pushObservation(parentHash, shallowQueryHash, shallowResp);
        if (applyContext)
            postWHNFObservationCount = applyContext->observations.size();
        cachedWHNF = std::move(r->first);
        return &*cachedWHNF;
    }
    return std::nullopt;
}

std::vector<std::string> TracingReplayObject::getAttrNames()
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getAttrNames");
        return ensureInner()->getAttrNames();
    }
    auto * p = std::get_if<trace::WHNFAttrs>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getAttrNames();
    return p->names;
}

std::string TracingReplayObject::getStringIgnoreContext()
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getStringIgnoreContext");
        return ensureInner()->getStringIgnoreContext();
    }
    auto * p = std::get_if<trace::WHNFString>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getStringIgnoreContext();
    return p->value;
}

std::string TracingReplayObject::getStringWithoutContext()
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getStringWithoutContext");
        return ensureInner()->getStringWithoutContext();
    }
    auto * p = std::get_if<trace::WHNFString>(&(*wp)->payload);
    if (!p || !p->context.empty())
        return ensureInner()->getStringWithoutContext();
    return p->value;
}

std::pair<std::string, NixStringContext> TracingReplayObject::getStringWithContext()
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getStringWithContext");
        return ensureInner()->getStringWithContext();
    }
    auto * p = std::get_if<trace::WHNFString>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getStringWithContext();
    NixStringContext ctx;
    for (const auto & s : p->context)
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
            return ensureInner()->getStringWithContext();
        }
    }
    return {p->value, std::move(ctx)};
}

RootedPath TracingReplayObject::getPath()
{
    whnf();
    tracingCacheLog("replay fallback: getPath");
    return ensureInner()->getPath();
}

bool TracingReplayObject::getBool(std::string_view errorCtx)
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getBool");
        return ensureInner()->getBool(errorCtx);
    }
    auto * p = std::get_if<trace::WHNFBool>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getBool(errorCtx);
    return p->value;
}

NixInt TracingReplayObject::getInt(std::string_view errorCtx)
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getInt");
        return ensureInner()->getInt(errorCtx);
    }
    auto * p = std::get_if<trace::WHNFInt>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getInt(errorCtx);
    return NixInt{p->value};
}

NixFloat TracingReplayObject::getFloat(std::string_view errorCtx)
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getFloat");
        return ensureInner()->getFloat(errorCtx);
    }
    auto * p = std::get_if<trace::WHNFFloat>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getFloat(errorCtx);
    return p->value;
}

size_t TracingReplayObject::getListSize()
{
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getListSize");
        return ensureInner()->getListSize();
    }
    auto * p = std::get_if<trace::WHNFList>(&(*wp)->payload);
    if (!p)
        return ensureInner()->getListSize();
    return p->size;
}

std::shared_ptr<Object> TracingReplayObject::getListElem(size_t idx)
{
    for (auto & parentHash : parentHashCandidates()) {
        trace::QueryGetListElem query{parentHash, idx};
        if (auto result = lookupStructuralChild<trace::QueryGetListElem, trace::ResultType>(query)) {
            pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), result->second.resultNodeHash);
            tracingCacheLog("replay hit: getListElem %d", idx);
            auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
            auto child = std::make_shared<TracingReplayObject>(
                evaluator, result->second, [self, idx]() { return ref<Object>(self->ensureInner()->getListElem(idx)); });
            child->withScope(argCell);
            if (applyContext) child->withApplyContextOnly(applyContext);
            return child;
        }
    }
    tracingCacheLog("replay fallback: getListElem %d", idx);
    return ensureInner()->getListElem(idx);
}

std::vector<std::string> TracingReplayObject::getListOfStringsNoCtx()
{
    for (auto & parentHash : parentHashCandidates()) {
        trace::QueryGetListOfStrings query{parentHash};
        if (auto r = lookupResult<trace::QueryGetListOfStrings, trace::ResultListOfStrings>(query)) {
            pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
            return r->first.values;
        }
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
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getType (from=%s)", triePos.queryHashStr);
        return ensureInner()->getType();
    }
    return stringToObjectType((*wp)->type);
}

RootValue TracingReplayObject::defeatCache()
{
    tracingCacheLog("replay fallback: defeatCache");
    return ensureInner()->defeatCache();
}

std::optional<FunctionInfo> TracingReplayObject::getFunctionInfo()
{
    for (auto & parentHash : parentHashCandidates()) {
        trace::QueryGetFunctionInfo query{parentHash};
        if (auto r = lookupResult<trace::QueryGetFunctionInfo, trace::ResultFunctionInfo>(query)) {
            pushObservation(parentHash, TracingDecisionGraph::computeQueryHash(query), r->second);
            if (!r->first.hasInfo)
                return std::nullopt;
            return FunctionInfo{.formals = r->first.formals, .ellipsis = r->first.ellipsis};
        }
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

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

template<typename Q, typename R>
std::optional<std::pair<R, Hash>> TracingReplayObject::lookupResult(const Q & query) const
{
    auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
    /* Phase D2: getters have no factSet chain. Terminal at
       (getterSelectorHash, parentTerminalCur). */
    auto anchorCur = triePos.factSetHash;
    tracingCacheLog("walker lookup: %s Q=%s anchor=%s (direct)",
                    Q::tag,
                    selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    anchorCur.to_string(HashFormat::Base16, false).substr(0, 12));
    auto resultNodeHash = evaluator.getDecisionGraph().getTerminal(selectorHash, anchorCur);
    if (!resultNodeHash) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        selectorHash.to_string(HashFormat::Base16, false).substr(0, 12));
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    auto payload = evaluator.getDecisionGraph().getResultPayload(*resultNodeHash);
    if (!payload) {
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(*payload);
        tracingCacheLog("replay hit: %s", Q::tag);
        tracingCacheStats().hits++;
        return std::make_pair(j.template get<R>(), *resultNodeHash);
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: payload parse failed: %s", e.what());
        tracingCacheStats().misses++;
        return std::nullopt;
    }
}

template<typename Q, typename R>
std::optional<std::pair<R, TriePosition>> TracingReplayObject::lookupStructuralChild(const Q & query) const
{
    auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
    auto anchorCur = triePos.factSetHash;
    tracingCacheLog("walker lookup: %s Q=%s anchor=%s (direct)",
                    Q::tag,
                    selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    anchorCur.to_string(HashFormat::Base16, false).substr(0, 12));
    auto resultNodeHash = evaluator.getDecisionGraph().getTerminal(selectorHash, anchorCur);
    if (!resultNodeHash) {
        tracingCacheLog("walker lookup: %s MISS Q=%s",
                        Q::tag,
                        selectorHash.to_string(HashFormat::Base16, false).substr(0, 12));
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    auto payload = evaluator.getDecisionGraph().getResultPayload(*resultNodeHash);
    if (!payload) {
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    try {
        auto j = cborStringToJson(*payload);
        tracingCacheLog("replay hit: %s", Q::tag);
        tracingCacheStats().hits++;
        return std::make_pair(
            j.template get<R>(),
            TriePosition{
                .resultNodeHash = *resultNodeHash,
                .queryHashStr = selectorHash.to_string(HashFormat::Base16, false),
                .factSetHash = anchorCur,
            });
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: payload parse failed: %s", e.what());
        tracingCacheStats().misses++;
        return std::nullopt;
    }
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    /* Symmetric with TracingObject::maybeGetAttr: existence is
       projected from parent's WHNFAttrs.names (via the walker's
       whnf() lookup); only when the attr is known to exist do we
       issue the pure-retrieval SelectorGetAttr. */
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: maybeGetAttr '%s' (no parent whnf)", name);
        return ensureInner()->maybeGetAttr(name);
    }
    auto * ap = std::get_if<trace::WHNFAttrs>(&(*wp)->payload);
    if (!ap) {
        tracingCacheLog("replay fallback: maybeGetAttr '%s' (parent whnf not attrs)", name);
        return ensureInner()->maybeGetAttr(name);
    }
    if (std::find(ap->names.begin(), ap->names.end(), name) == ap->names.end()) {
        tracingCacheLog("replay hit: getAttr '%s' -> missing (via whnf.names)", name);
        return nullptr;
    }
    trace::SelectorGetAttr query{name, triePos.queryHashStr};
    auto result = lookupStructuralChild<trace::SelectorGetAttr, trace::ResultWHNF>(query);
    if (!result) {
        tracingCacheLog("replay fallback: maybeGetAttr '%s' (no getAttr recording)", name);
        return ensureInner()->maybeGetAttr(name);
    }
    tracingCacheLog("replay hit: getAttr '%s' -> found", name);
    auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
    auto child = std::make_shared<TracingReplayObject>(
        evaluator, result->second, [self, name]() { return ref<Object>(self->ensureInner()->maybeGetAttr(name)); });
    child->cachedWHNF = std::move(result->first);
    child->withArgCell(argCell);
    /* Symmetric to TracingObject::maybeGetAttr's B3/B7-remaining
       propagation: cb-apply-origin walker children inherit both marks
       so their queryHashes match cold's. */
    if (cbApplyOrigin) {
        child->withCbApplyOrigin();
        if (producer)
            child->withProducer(*producer);
    }
    return child;
}

std::optional<const trace::ResultWHNF *> TracingReplayObject::whnf()
{
    if (cachedWHNF)
        return &*cachedWHNF;
    /* #185: decode WHNF from triePos.resultNodeHash — the parent
       Selector's Terminal Result IS a ResultWHNF (per
       DECLARE_SELECTOR_RESULT). No separate GetWHNF lookup. */
    auto payload = evaluator.getDecisionGraph().getResultPayload(triePos.resultNodeHash);
    if (!payload)
        return std::nullopt;
    try {
        auto j = cborStringToJson(*payload);
        cachedWHNF = j.get<trace::ResultWHNF>();
        tracingCacheStats().hits++;
        return &*cachedWHNF;
    } catch (const nlohmann::json::exception & e) {
        tracingCacheLog("replay: triePos payload parse failed: %s", e.what());
        return std::nullopt;
    }
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
    /* Symmetric with TracingObject::getListElem: bounds are projected
       from parent's WHNFList.size; retrieval is a distinct
       SelectorGetListElem observation returning the child's WHNF. */
    auto wp = whnf();
    if (!wp) {
        tracingCacheLog("replay fallback: getListElem %d (no parent whnf)", idx);
        return ensureInner()->getListElem(idx);
    }
    auto * lp = std::get_if<trace::WHNFList>(&(*wp)->payload);
    if (!lp) {
        tracingCacheLog("replay fallback: getListElem %d (parent whnf not list)", idx);
        return ensureInner()->getListElem(idx);
    }
    if (idx >= lp->size) {
        tracingCacheLog("replay fallback: getListElem %d out of bounds (size %zu)", idx, lp->size);
        return ensureInner()->getListElem(idx);
    }
    trace::SelectorGetListElem query{triePos.queryHashStr, idx};
    if (auto result = lookupStructuralChild<trace::SelectorGetListElem, trace::ResultWHNF>(query)) {
        tracingCacheLog("replay hit: getListElem %d", idx);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        auto child = std::make_shared<TracingReplayObject>(
            evaluator, result->second, [self, idx]() { return ref<Object>(self->ensureInner()->getListElem(idx)); });
        child->cachedWHNF = std::move(result->first);
        child->withArgCell(argCell);
        /* B3/B7-remaining: cb-apply-origin propagation, symmetric to maybeGetAttr. */
        if (cbApplyOrigin) {
            child->withCbApplyOrigin();
            if (producer)
                child->withProducer(*producer);
        }
        return child;
    }
    tracingCacheLog("replay fallback: getListElem %d", idx);
    return ensureInner()->getListElem(idx);
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
    trace::SelectorGetFunctionInfo query{triePos.queryHashStr};
    if (auto r = lookupResult<trace::SelectorGetFunctionInfo, trace::ResultFunctionInfo>(query)) {
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

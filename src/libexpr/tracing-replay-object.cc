#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>
#include <set>

namespace nix {

static ObjectType stringToObjectType(const std::string & type)
{
    if (type == "set")
        return nAttrs;
    if (type == "list")
        return nList;
    if (type == "string")
        return nString;
    if (type == "path")
        return nPath;
    if (type == "int")
        return nInt;
    if (type == "float")
        return nFloat;
    if (type == "bool")
        return nBool;
    if (type == "null")
        return nNull;
    if (type == "lambda")
        return nFunction;
    if (type == "thunk")
        return nThunk;
    if (type == "external")
        return nExternal;
    throw Error("unknown type in trace: %s", type);
}

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
        inner = getInner();
    }
    return *inner;
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
std::optional<R> TracingReplayObject::lookupResult(const Q & query) const
{
    auto & tracingIndex = evaluator.getTracingIndex();
    auto queryHash = TracingIndex::computeQueryHash(query);

    std::vector<NodeHash> pendingValidated;
    auto findResult = [&](const QueryNode & child) -> std::optional<ResultNode> {
        pendingValidated.clear();
        return tracingIndex.findResult(child.nodeHash,
            [&](const std::string & queryPayload, const NodeHash & resultNodeHash, const std::string & resultPayload) {
                auto currentResponse = evaluator.getCurrentResponse(queryPayload);
                if (!currentResponse || resultPayload != *currentResponse)
                    return false;
                pendingValidated.push_back(resultNodeHash);
                return true;
            });
    };
    auto commitValidated = [&]() {
        for (const auto & h : pendingValidated)
            evaluator.markValidated(h);
        pendingValidated.clear();
    };

    auto parseResult = [&](const ResultNode & resultNode) -> std::optional<R> {
        try {
            auto j = cborStringToJson(resultNode.payload);
            return j.template get<R>();
        } catch (const nlohmann::json::exception & e) {
            tracingCacheLog("replay: failed to parse result: %s", e.what());
            return std::nullopt;
        }
    };

    std::set<NodeHash> triedNodes;

    // Strategy 1: Trie following — direct lookup from the evaluator's temporal cursor
    if (auto cursor = evaluator.getTemporalCursor()) {
        auto nodeHash = TracingIndex::computeQueryNodeHash(*cursor, queryHash);
        if (auto child = tracingIndex.getQuery(nodeHash)) {
            triedNodes.insert(child->nodeHash);

            if (evaluator.validateToValidatedNode(child->nodeHash)) {
                if (auto resultNode = findResult(*child)) {
                    if (auto result = parseResult(*resultNode)) {
                        commitValidated();
                        commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                        evaluator.setTemporalCursor(resultNode->nodeHash);
                        tracingCacheLog("replay hit (trie): %s", Q::tag);
                        return result;
                    }
                }
            }
        }
    }

    // Strategy 2: Structural lookup — structural children
    auto structuralChildren = tracingIndex.selectStructuralChildren(triePos.resultNodeHash, queryHash);
    for (const auto & child : structuralChildren) {
        if (triedNodes.count(child.nodeHash))
            continue;
        triedNodes.insert(child.nodeHash);

        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            tracingCacheLog("replay: structural validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findResult(child)) {
            if (auto result = parseResult(*resultNode)) {
                commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                evaluator.setTemporalCursor(resultNode->nodeHash);
                tracingCacheLog("replay hit (structural): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 3: Shortcut lookup — global table
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);
    for (const auto & shortcut : shortcuts) {
        if (triedNodes.count(shortcut.nodeHash))
            continue;
        triedNodes.insert(shortcut.nodeHash);

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        if (!evaluator.validateDependencies(shortcut.nodeHash)) {
            tracingCacheLog("replay: shortcut validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findResult(*queryNode)) {
            if (auto result = parseResult(*resultNode)) {
                commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                evaluator.setTemporalCursor(resultNode->nodeHash);
                tracingCacheLog("replay hit (shortcut): %s", Q::tag);
                return result;
            }
        }
    }

    tracingCacheLog("replay miss: %s", Q::tag);
    return std::nullopt;
}

/**
 * Cascading lookup for structural children (getAttr, getListElem).
 * Same three strategies as lookupResult, but returns a TriePosition
 * for the child so further traversal can continue from that point.
 */
template<typename Q, typename R>
std::optional<std::pair<R, TriePosition>> TracingReplayObject::lookupStructuralChild(const Q & query) const
{
    auto & tracingIndex = evaluator.getTracingIndex();
    auto queryHash = TracingIndex::computeQueryHash(query);

    std::vector<NodeHash> pendingValidated;
    auto findResult = [&](const QueryNode & child) -> std::optional<ResultNode> {
        pendingValidated.clear();
        return tracingIndex.findResult(child.nodeHash,
            [&](const std::string & queryPayload, const NodeHash & resultNodeHash, const std::string & resultPayload) {
                auto currentResponse = evaluator.getCurrentResponse(queryPayload);
                if (!currentResponse || resultPayload != *currentResponse)
                    return false;
                pendingValidated.push_back(resultNodeHash);
                return true;
            });
    };
    auto commitValidated = [&]() {
        for (const auto & h : pendingValidated)
            evaluator.markValidated(h);
        pendingValidated.clear();
    };

    auto parseResultWithPos = [&](const ResultNode & resultNode) -> std::optional<std::pair<R, TriePosition>> {
        try {
            auto j = cborStringToJson(resultNode.payload);
            R result = j.template get<R>();
            auto childPos = TriePosition{
                .resultNodeHash = resultNode.nodeHash,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            };
            return std::make_pair(result, childPos);
        } catch (const nlohmann::json::exception & e) {
            tracingCacheLog("replay: failed to parse result: %s", e.what());
            return std::nullopt;
        }
    };

    std::set<NodeHash> triedNodes;

    // Strategy 1: Trie following — direct lookup from the evaluator's temporal cursor
    if (auto cursor = evaluator.getTemporalCursor()) {
        auto nodeHash = TracingIndex::computeQueryNodeHash(*cursor, queryHash);
        if (auto child = tracingIndex.getQuery(nodeHash)) {
            triedNodes.insert(child->nodeHash);

            if (evaluator.validateToValidatedNode(child->nodeHash)) {
                if (auto resultNode = findResult(*child)) {
                    if (auto result = parseResultWithPos(*resultNode)) {
                        commitValidated();
                        commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                        evaluator.setTemporalCursor(resultNode->nodeHash);
                        tracingCacheLog("replay hit (trie): %s", Q::tag);
                        return result;
                    }
                }
            }
        }
    }

    // Strategy 2: Structural lookup — structural children
    auto structuralChildren = tracingIndex.selectStructuralChildren(triePos.resultNodeHash, queryHash);
    for (const auto & child : structuralChildren) {
        if (triedNodes.count(child.nodeHash))
            continue;
        triedNodes.insert(child.nodeHash);

        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            tracingCacheLog("replay: structural validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findResult(child)) {
            if (auto result = parseResultWithPos(*resultNode)) {
                commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                evaluator.setTemporalCursor(resultNode->nodeHash);
                tracingCacheLog("replay hit (structural): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 3: Shortcut lookup — global table
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);
    for (const auto & shortcut : shortcuts) {
        if (triedNodes.count(shortcut.nodeHash))
            continue;
        triedNodes.insert(shortcut.nodeHash);

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        if (!evaluator.validateDependencies(shortcut.nodeHash)) {
            tracingCacheLog("replay: shortcut validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findResult(*queryNode)) {
            if (auto result = parseResultWithPos(*resultNode)) {
                commitValidated();
                evaluator.markValidated(resultNode->nodeHash);
                evaluator.setTemporalCursor(resultNode->nodeHash);
                tracingCacheLog("replay hit (shortcut): %s", Q::tag);
                return result;
            }
        }
    }

    tracingCacheLog("replay miss: %s", Q::tag);
    return std::nullopt;
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    auto parentHash = triePos.queryHashStr;
    trace::QueryGetAttr query{name, parentHash};

    if (auto result = lookupStructuralChild<trace::QueryGetAttr, trace::ResultMaybeType>(query)) {
        if (!result->first.type) {
            tracingCacheLog("replay hit: getAttr '%s' -> missing", name);
            return nullptr;
        }

        tracingCacheLog("replay hit: getAttr '%s' -> found", name);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        return std::make_shared<TracingReplayObject>(
            evaluator, result->second, [self, name]() { return ref<Object>(self->ensureInner()->maybeGetAttr(name)); });
    }

    tracingCacheLog("replay fallback: maybeGetAttr '%s'", name);
    return ensureInner()->maybeGetAttr(name);
}

std::vector<std::string> TracingReplayObject::getAttrNames()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r =
            lookupResult<trace::QueryGetAttrNames, trace::ResultListOfStrings>(trace::QueryGetAttrNames{parentHash}))
        return r->values;
    tracingCacheLog("replay fallback: getAttrNames"); return ensureInner()->getAttrNames();
}

std::string TracingReplayObject::getStringIgnoreContext()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetString, trace::ResultString>(trace::QueryGetString{parentHash}))
        return r->value;
    tracingCacheLog("replay fallback: getStringIgnoreContext"); return ensureInner()->getStringIgnoreContext();
}

std::string TracingReplayObject::getStringWithoutContext()
{
    // getStringWithoutContext checks for empty context which the cache doesn't track
    tracingCacheLog("replay fallback: getStringWithoutContext"); return ensureInner()->getStringWithoutContext();
}

std::pair<std::string, NixStringContext> TracingReplayObject::getStringWithContext()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetStringWithContext, trace::ResultStringWithContext>(
            trace::QueryGetStringWithContext{parentHash})) {
        NixStringContext ctx;
        for (const auto & s : r->context)
            ctx.insert(NixStringContextElem::parse(s));

        // Validate that all context paths still exist in the store
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
        return {r->value, std::move(ctx)};
    }
    tracingCacheLog("replay fallback: getStringWithContext");
    return ensureInner()->getStringWithContext();
}

SourcePath TracingReplayObject::getPath()
{
    tracingCacheLog("replay fallback: getPath"); return ensureInner()->getPath();
}

bool TracingReplayObject::getBool(std::string_view errorCtx)
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetBool, trace::ResultBool>(trace::QueryGetBool{parentHash}))
        return r->value;
    tracingCacheLog("replay fallback: getBool"); return ensureInner()->getBool(errorCtx);
}

NixInt TracingReplayObject::getInt(std::string_view errorCtx)
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetInt, trace::ResultInt>(trace::QueryGetInt{parentHash}))
        return NixInt{r->value};
    tracingCacheLog("replay fallback: getInt"); return ensureInner()->getInt(errorCtx);
}

NixFloat TracingReplayObject::getFloat(std::string_view errorCtx)
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetFloat, trace::ResultFloat>(trace::QueryGetFloat{parentHash}))
        return r->value;
    tracingCacheLog("replay fallback: getFloat"); return ensureInner()->getFloat(errorCtx);
}

size_t TracingReplayObject::getListSize()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetListSize, trace::ResultListSize>(trace::QueryGetListSize{parentHash}))
        return r->size;
    tracingCacheLog("replay fallback: getListSize"); return ensureInner()->getListSize();
}

std::shared_ptr<Object> TracingReplayObject::getListElem(size_t idx)
{
    auto parentHash = triePos.queryHashStr;
    trace::QueryGetListElem query{parentHash, idx};

    if (auto result = lookupStructuralChild<trace::QueryGetListElem, trace::ResultType>(query)) {
        tracingCacheLog("replay hit: getListElem %d", idx);
        auto self = std::static_pointer_cast<TracingReplayObject>(shared_from_this());
        return std::make_shared<TracingReplayObject>(
            evaluator, result->second, [self, idx]() { return ref<Object>(self->ensureInner()->getListElem(idx)); });
    }

    tracingCacheLog("replay fallback: getListElem %d", idx); return ensureInner()->getListElem(idx);
}

std::vector<std::string> TracingReplayObject::getListOfStringsNoCtx()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetListOfStrings, trace::ResultListOfStrings>(
            trace::QueryGetListOfStrings{parentHash}))
        return r->values;
    tracingCacheLog("replay fallback: getListOfStringsNoCtx"); return ensureInner()->getListOfStringsNoCtx();
}

ObjectType TracingReplayObject::getTypeLazy()
{
    return getType();
}

ObjectType TracingReplayObject::getType()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetType, trace::ResultType>(trace::QueryGetType{parentHash}))
        return stringToObjectType(r->type);
    tracingCacheLog("replay fallback: getType (from=%s)", triePos.queryHashStr); return ensureInner()->getType();
}

RootValue TracingReplayObject::defeatCache()
{
    tracingCacheLog("replay fallback: defeatCache"); return ensureInner()->defeatCache();
}

std::optional<FunctionInfo> TracingReplayObject::getFunctionInfo()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r =
            lookupResult<trace::QueryGetFunctionInfo, trace::ResultFunctionInfo>(trace::QueryGetFunctionInfo{parentHash}))
    {
        if (!r->hasInfo)
            return std::nullopt;
        return FunctionInfo{.formals = r->formals, .ellipsis = r->ellipsis};
    }
    tracingCacheLog("replay fallback: getFunctionInfo");
    return ensureInner()->getFunctionInfo();
}

} // namespace nix

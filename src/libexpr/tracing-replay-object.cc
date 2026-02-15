#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/value/context.hh"
#include "nix/store/store-api.hh"
#include "nix/util/error.hh"
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
        debug("replay fallback: activating inner");
        inner = getInner();
    }
    return *inner;
}

/**
 * Cascading Lookup Strategy (see doc/tracing-index-data-model.md)
 *
 * For each lookup, we try three strategies in order of expected performance:
 *
 * 1. **Trie following** - Look for temporal children (queries whose afterHash equals
 *    our result). Fastest when the user's access pattern matches the recorded order.
 *    Validates incrementally from our known-valid position.
 *
 * 2. **Structural lookup** - Look for structural children (queries whose structuralParent
 *    equals our result). Handles same operations in different order - "jumps ahead".
 *    Validates incrementally from our known-valid position.
 *
 * 3. **Shortcut lookup** - Fall back to the global shortcut table. Can switch to
 *    entirely different traces. The queryHash encodes semantic identity (operation +
 *    input object identity), so shortcuts find semantically equivalent queries.
 *    Requires full validation from root (not incremental).
 */
template<typename Q, typename R>
std::optional<R> TracingReplayObject::lookupResult(const Q & query) const
{
    auto & tracingIndex = evaluator.getTracingIndex();
    auto queryHash = TracingIndex::computeQueryHash(query);

    // Helper to find result following a query and validate responses on the path
    auto findAndValidateResult = [&](const QueryNode & child) -> std::optional<ResultNode> {
        // Walk forward: Query → Response* → Result
        std::vector<ResponseNode> responsesOnPath;
        NodeHash current = child.nodeHash;

        while (true) {
            auto results = tracingIndex.selectChildResults(current);
            if (!results.empty()) {
                // Validate responses AFTER the query (on path to result)
                if (!evaluator.validateResponses(responsesOnPath)) {
                    debug("replay: post-query validation failed for %s", Q::tag);
                    return std::nullopt;
                }
                return results[0];
            }

            auto responses = tracingIndex.selectChildResponses(current);
            if (responses.empty())
                break;

            responsesOnPath.push_back(responses[0]);
            current = responses[0].nodeHash;
        }

        debug("replay: no result found for %s", Q::tag);
        return std::nullopt;
    };

    // Helper to parse result payload
    auto parseResult = [&](const ResultNode & resultNode) -> std::optional<R> {
        try {
            auto j = nlohmann::json::parse(resultNode.payload);
            return j.template get<R>();
        } catch (const nlohmann::json::exception & e) {
            debug("replay: failed to parse result: %s", e.what());
            return std::nullopt;
        }
    };

    // Track which nodes we've already tried to avoid duplicates
    std::set<NodeHash> triedNodes;

    // Strategy 1: Trie following - temporal children (fastest when access pattern matches)
    auto temporalChildren = tracingIndex.selectChildQueries(triePos.resultNodeHash);
    for (const auto & child : temporalChildren) {
        if (child.queryHash != queryHash)
            continue;
        triedNodes.insert(child.nodeHash);

        // Validate all dependencies back to a known-valid node
        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            debug("replay: trie validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(child)) {
            if (auto result = parseResult(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (trie): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 2: Structural lookup - structural children (can jump anywhere in trie)
    auto structuralChildren = tracingIndex.selectStructuralChildren(triePos.resultNodeHash, queryHash);
    for (const auto & child : structuralChildren) {
        if (triedNodes.count(child.nodeHash))
            continue;
        triedNodes.insert(child.nodeHash);

        // Validate all dependencies back to a known-valid node
        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            debug("replay: structural validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(child)) {
            if (auto result = parseResult(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (structural): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 3: Shortcut lookup - global table (can switch traces entirely)
    // The queryHash encodes semantic identity, so this finds equivalent queries
    // in ANY trace. Requires full validation from root.
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);
    for (const auto & shortcut : shortcuts) {
        if (triedNodes.count(shortcut.nodeHash))
            continue;
        triedNodes.insert(shortcut.nodeHash);

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        // Full validation from root (not incremental - different trace context)
        if (!evaluator.validateDependencies(shortcut.nodeHash)) {
            debug("replay: shortcut validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(*queryNode)) {
            if (auto result = parseResult(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (shortcut): %s", Q::tag);
                return result;
            }
        }
    }

    debug("replay miss: %s", Q::tag);
    return std::nullopt;
}

/**
 * Cascading Lookup Strategy for structural children.
 * Same as lookupResult but returns the child's TriePosition for further traversal.
 * See doc/tracing-index-data-model.md "Cascading Lookup Strategy".
 */
template<typename Q, typename R>
std::optional<std::pair<R, TriePosition>> TracingReplayObject::lookupStructuralChild(const Q & query) const
{
    auto & tracingIndex = evaluator.getTracingIndex();
    auto queryHash = TracingIndex::computeQueryHash(query);

    // Helper to find result following a query and validate responses on the path
    auto findAndValidateResult = [&](const QueryNode & child) -> std::optional<ResultNode> {
        std::vector<ResponseNode> responsesOnPath;
        NodeHash current = child.nodeHash;

        while (true) {
            auto results = tracingIndex.selectChildResults(current);
            if (!results.empty()) {
                if (!evaluator.validateResponses(responsesOnPath)) {
                    debug("replay: post-query validation failed for %s", Q::tag);
                    return std::nullopt;
                }
                return results[0];
            }

            auto responses = tracingIndex.selectChildResponses(current);
            if (responses.empty())
                break;

            responsesOnPath.push_back(responses[0]);
            current = responses[0].nodeHash;
        }

        debug("replay: no result found for %s", Q::tag);
        return std::nullopt;
    };

    // Helper to parse result and build return value with position
    auto parseResultWithPos = [&](const ResultNode & resultNode) -> std::optional<std::pair<R, TriePosition>> {
        try {
            auto j = nlohmann::json::parse(resultNode.payload);
            R result = j.template get<R>();
            auto childPos = TriePosition{
                .resultNodeHash = resultNode.nodeHash,
                .afterHash = resultNode.nodeHash,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            };
            return std::make_pair(result, childPos);
        } catch (const nlohmann::json::exception & e) {
            debug("replay: failed to parse result: %s", e.what());
            return std::nullopt;
        }
    };

    std::set<NodeHash> triedNodes;

    // Strategy 1: Trie following - temporal children
    auto temporalChildren = tracingIndex.selectChildQueries(triePos.resultNodeHash);
    for (const auto & child : temporalChildren) {
        if (child.queryHash != queryHash)
            continue;
        triedNodes.insert(child.nodeHash);

        // Validate all dependencies back to a known-valid node
        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            debug("replay: trie validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(child)) {
            if (auto result = parseResultWithPos(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (trie): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 2: Structural lookup - structural children (can jump anywhere in trie)
    auto structuralChildren = tracingIndex.selectStructuralChildren(triePos.resultNodeHash, queryHash);
    for (const auto & child : structuralChildren) {
        if (triedNodes.count(child.nodeHash))
            continue;
        triedNodes.insert(child.nodeHash);

        // Validate all dependencies back to a known-valid node
        if (!evaluator.validateToValidatedNode(child.nodeHash)) {
            debug("replay: structural validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(child)) {
            if (auto result = parseResultWithPos(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (structural): %s", Q::tag);
                return result;
            }
        }
    }

    // Strategy 3: Shortcut lookup - global table
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);
    for (const auto & shortcut : shortcuts) {
        if (triedNodes.count(shortcut.nodeHash))
            continue;
        triedNodes.insert(shortcut.nodeHash);

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        if (!evaluator.validateDependencies(shortcut.nodeHash)) {
            debug("replay: shortcut validation failed for %s", Q::tag);
            continue;
        }

        if (auto resultNode = findAndValidateResult(*queryNode)) {
            if (auto result = parseResultWithPos(*resultNode)) {
                evaluator.markValidated(resultNode->nodeHash);
                debug("replay hit (shortcut): %s", Q::tag);
                return result;
            }
        }
    }

    debug("replay miss: %s", Q::tag);
    return std::nullopt;
}

std::shared_ptr<Object> TracingReplayObject::maybeGetAttr(const std::string & name)
{
    // For getAttr, we use structural children lookup
    // The query's `from` contains the parent's queryHash (Merkle identity)
    auto parentHash = triePos.queryHashStr;
    trace::QueryGetAttr query{name, parentHash};

    if (auto result = lookupStructuralChild<trace::QueryGetAttr, trace::ResultMaybeType>(query)) {
        if (!result->first.type) {
            debug("replay hit: getAttr '%s' -> missing", name);
            return nullptr;
        }

        debug("replay hit: getAttr '%s' -> found", name);
        return std::make_shared<TracingReplayObject>(
            evaluator, result->second, [this, name]() { return ref<Object>(ensureInner()->maybeGetAttr(name)); });
    }

    return ensureInner()->maybeGetAttr(name);
}

std::vector<std::string> TracingReplayObject::getAttrNames()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r =
            lookupResult<trace::QueryGetAttrNames, trace::ResultListOfStrings>(trace::QueryGetAttrNames{parentHash}))
        return r->values;
    return ensureInner()->getAttrNames();
}

std::string TracingReplayObject::getStringIgnoreContext()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetString, trace::ResultString>(trace::QueryGetString{parentHash}))
        return r->value;
    return ensureInner()->getStringIgnoreContext();
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
                debug("replay miss: context path %s no longer valid", store.printStorePath(path));
                return ensureInner()->getStringWithContext();
            }
        }
        return {r->value, std::move(ctx)};
    }
    return ensureInner()->getStringWithContext();
}

SourcePath TracingReplayObject::getPath()
{
    debug("replay miss: getPath not cached");
    return ensureInner()->getPath();
}

bool TracingReplayObject::getBool(std::string_view errorCtx)
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetBool, trace::ResultBool>(trace::QueryGetBool{parentHash}))
        return r->value;
    return ensureInner()->getBool(errorCtx);
}

NixInt TracingReplayObject::getInt(std::string_view errorCtx)
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetInt, trace::ResultInt>(trace::QueryGetInt{parentHash}))
        return NixInt{r->value};
    return ensureInner()->getInt(errorCtx);
}

std::vector<std::string> TracingReplayObject::getListOfStringsNoCtx()
{
    auto parentHash = triePos.queryHashStr;
    if (auto r = lookupResult<trace::QueryGetListOfStrings, trace::ResultListOfStrings>(
            trace::QueryGetListOfStrings{parentHash}))
        return r->values;
    return ensureInner()->getListOfStringsNoCtx();
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
    return ensureInner()->getType();
}

RootValue TracingReplayObject::defeatCache()
{
    debug("replay miss: defeatCache");
    return ensureInner()->defeatCache();
}

} // namespace nix

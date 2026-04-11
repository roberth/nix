#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

namespace nix {

TracingReplayEvaluator::TracingReplayEvaluator(
    ref<Evaluator> inner,
    TracingIndex & tracingIndex,
    Environment & validationEnv,
    TracingWriter & writer)
    : inner(inner)
    , tracingIndex(tracingIndex)
    , writer(writer)
    , validationEnv(validationEnv)
{
}

bool TracingReplayEvaluator::validateDependencies(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    auto deps = tracingIndex.selectDependencies(queryNodeHash);
    // Multiple recordings may share the same query node prefix,
    // producing deps from different sessions. Group by request
    // and validate: at least one result per unique request must match.
    if (!validateDepsAnyMatch(deps))
        return false;

    validatedNodes.insert(queryNodeHash);
    return true;
}

bool TracingReplayEvaluator::validateToValidatedNode(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    bool reachedValidated = false;
    auto deps = tracingIndex.selectDependenciesUntilValidated(queryNodeHash, validatedNodes, reachedValidated);

    if (!validateDeps(deps))
        return false;

    validatedNodes.insert(queryNodeHash);
    return true;
}

void TracingReplayEvaluator::markValidated(const NodeHash & nodeHash)
{
    validatedNodes.insert(nodeHash);
}

bool TracingReplayEvaluator::isValidated(const NodeHash & nodeHash) const
{
    return validatedNodes.count(nodeHash) > 0;
}

bool TracingReplayEvaluator::validateDepsAnyMatch(const std::vector<std::pair<QueryNode, ResultNode>> & deps)
{
    // Group deps by query payload. Multiple recordings from the same
    // trie prefix produce duplicate entries for the same file/env.
    // For each unique request, at least one result must validate.
    std::map<QueryHash, bool> requestValidated;

    for (const auto & [qNode, rNode] : deps) {
        if (validatedNodes.count(rNode.nodeHash))
            continue;

        auto it = requestValidated.find(qNode.queryHash);
        if (it != requestValidated.end() && it->second)
            continue; // already have a valid result for this request

        if (validateDeps({{qNode, rNode}})) {
            requestValidated[qNode.queryHash] = true;
        } else {
            if (requestValidated.find(qNode.queryHash) == requestValidated.end())
                requestValidated[qNode.queryHash] = false;
        }
    }

    for (auto & [req, valid] : requestValidated) {
        if (!valid)
            return false;
    }
    return true;
}

std::optional<std::string> TracingReplayEvaluator::getCurrentResponse(const std::string & requestCbor)
{
    try {
        auto reqJson = cborStringToJson(requestCbor);

        if (reqJson.contains("absPath")) {
            std::string path = reqJson["absPath"];
            auto currentHash = validationEnv.getFileHash(path);
            nlohmann::json respJson = trace::FileReadResponse{currentHash};
            return jsonToCborString(respJson);
        } else if (reqJson.contains("name")) {
            std::string name = reqJson["name"];
            auto currentVal = validationEnv.getEnv(name);
            nlohmann::json respJson = trace::GetEnvResponse{currentVal};
            return jsonToCborString(respJson);
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to get current response: %s", e.what());
    }
    return std::nullopt;
}

bool TracingReplayEvaluator::validateDeps(const std::vector<std::pair<QueryNode, ResultNode>> & deps)
{
    for (const auto & [qNode, rNode] : deps) {
        if (validatedNodes.count(rNode.nodeHash))
            continue;

        try {
            // Get the query payload (request) to know what to re-execute
            auto payloadOpt = tracingIndex.getQueryPayload(qNode.queryHash);
            if (!payloadOpt) {
                tracingCacheLog("replay: missing query payload for dependency");
                return false;
            }
            auto reqJson = cborStringToJson(*payloadOpt);

            // The result payload is the recorded response — compare as raw bytes
            if (reqJson.contains("absPath")) {
                std::string path = reqJson["absPath"];
                auto currentHash = validationEnv.getFileHash(path);

                nlohmann::json currentRespJson = trace::FileReadResponse{currentHash};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (rNode.payload != currentCbor) {
                    tracingCacheLog("replay invalidated: file %s changed", path);
                    return false;
                }
            } else if (reqJson.contains("name")) {
                std::string name = reqJson["name"];
                auto currentVal = validationEnv.getEnv(name);

                nlohmann::json currentRespJson = trace::GetEnvResponse{currentVal};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (rNode.payload != currentCbor) {
                    tracingCacheLog("replay invalidated: env %s changed", name);
                    return false;
                }
            } else {
                // Unknown query type (e.g. ambient interaction) — can't
                // re-execute to validate. Conservative: invalidate.
                tracingCacheLog("replay invalidated: unvalidatable dependency");
                return false;
            }
        } catch (const std::exception & e) {
            tracingCacheLog("replay: failed to parse dependency: %s", e.what());
            return false;
        }
        validatedNodes.insert(rNode.nodeHash);
    }
    return true;
}

template<typename Q>
std::optional<std::pair<std::string, TriePosition>> TracingReplayEvaluator::lookup(const Q & query)
{
    auto queryHash = TracingIndex::computeQueryHash(query);
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);

    for (const auto & shortcut : shortcuts) {
        if (!validateDependencies(shortcut.nodeHash))
            continue;

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        // Walk forward: Query → (depth>0 Query/Result)* → Result(depth=0)
        // At each step, validate depth>0 queries by computing the current
        // response and comparing with the recorded result.
        std::optional<ResultNode> resultNode;
        NodeHash current = shortcut.nodeHash;
        bool validPath = true;

        while (true) {
            // Check for a depth=0 result (the final answer)
            if (auto result = tracingIndex.getChildResult(current)) {
                resultNode = result;
                break;
            }

            // Look for depth>0 child queries (environment events)
            auto childQueries = tracingIndex.selectChildQueries(current);
            bool foundValid = false;
            for (const auto & childQ : childQueries) {
                if (childQ.depth == 0)
                    continue;

                auto payloadOpt = tracingIndex.getQueryPayload(childQ.queryHash);
                if (!payloadOpt)
                    continue;

                auto currentResponse = getCurrentResponse(*payloadOpt);
                if (!currentResponse) {
                    // Unknown query type (e.g. ambient interaction) — can't
                    // validate, so this path is not usable for replay.
                    break;
                }

                // Check if there's a result matching the current response
                auto childResult = tracingIndex.getChildResult(childQ.nodeHash);
                if (childResult && childResult->payload == *currentResponse) {
                    markValidated(childResult->nodeHash);
                    current = childResult->nodeHash;
                    foundValid = true;
                    break;
                }
            }
            if (!foundValid) {
                validPath = false;
                break;
            }
        }

        if (!resultNode || !validPath)
            continue;

        validatedNodes.insert(resultNode->nodeHash);
        temporalCursor = resultNode->nodeHash;
        tracingCacheLog("replay hit: %s", Q::tag);
        return std::make_pair(
            resultNode->payload,
            TriePosition{
                .resultNodeHash = resultNode->nodeHash,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            });
    }

    return std::nullopt;
}

bool TracingReplayEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & TracingReplayEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & TracingReplayEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

EvalState & TracingReplayEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingReplayEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    if (auto result = lookup(trace::QueryImport{displayPath})) {
        tracingCacheLog("replay hit: evalFile %s", displayPath);
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
    }

    tracingCacheLog("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    if (auto result = lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
        tracingCacheLog("replay hit: evalExpr");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
    }

    tracingCacheLog("replay miss: evalExpr");
    return inner->evalExpr(expr, basePath);
}

ref<Object> TracingReplayEvaluator::evalExprLazy(const std::string & expr, const SourcePath & basePath)
{
    return inner->evalExprLazy(expr, basePath);
}

ref<Object> TracingReplayEvaluator::mkString(const std::string & s)
{
    return inner->mkString(s);
}

ref<Object> TracingReplayEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return inner->mkAttrs(attrs);
}

ref<Object> TracingReplayEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    // Try to get identity from TracingObject/TracingReplayObject
    auto getId = [](Object & obj) -> std::optional<std::string> {
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            return to->getQueryHashStr();
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return std::optional{ro->getTriePos().queryHashStr};
        return std::nullopt;
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    // Get or allocate virtual root ids. The registry on TracingWriter
    // ensures that on a miss, when the recording evaluator sees the
    // same Object, it gets the same id — no counter drift.
    if (!fnId)
        fnId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(fn).value());
    if (!argId)
        argId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(arg).value());

    if (auto result = lookup(trace::QueryApply{*fnId, *argId})) {
        tracingCacheLog("replay hit: apply");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, fn, arg]() { return inner->apply(fn, arg); });
    }
    tracingCacheLog("replay miss: apply");

    return inner->apply(fn, arg);
}

} // namespace nix

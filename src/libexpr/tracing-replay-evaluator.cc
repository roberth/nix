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
    std::filesystem::path hashCacheDbPath)
    : inner(inner)
    , tracingIndex(tracingIndex)
    , hashCache(std::move(hashCacheDbPath))
    , validationEnv(validationEnv)
{
}

bool TracingReplayEvaluator::validateDependencies(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    auto responses = tracingIndex.selectDependencies(queryNodeHash);
    // Multiple recordings may share the same query node prefix,
    // producing responses from different sessions. Group by request
    // and validate: at least one response per unique request must match.
    if (!validateResponsesAnyMatch(responses))
        return false;

    validatedNodes.insert(queryNodeHash);
    return true;
}

bool TracingReplayEvaluator::validateToValidatedNode(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    bool reachedValidated = false;
    auto responses = tracingIndex.selectDependenciesUntilValidated(queryNodeHash, validatedNodes, reachedValidated);

    if (!validateResponses(responses))
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

bool TracingReplayEvaluator::validateResponsesAnyMatch(const std::vector<ResponseNode> & responses)
{
    // Group responses by request. Multiple recordings from the same
    // trie prefix produce duplicate responses for the same file/env.
    // For each unique request, at least one response must validate.
    std::map<std::string, bool> requestValidated; // request blob → has any match

    for (const auto & resp : responses) {
        if (validatedNodes.count(resp.nodeHash))
            continue;

        auto it = requestValidated.find(resp.request);
        if (it != requestValidated.end() && it->second)
            continue; // already have a valid response for this request

        if (validateResponses({resp})) {
            requestValidated[resp.request] = true;
        } else {
            if (requestValidated.find(resp.request) == requestValidated.end())
                requestValidated[resp.request] = false;
        }
    }

    for (auto & [req, valid] : requestValidated) {
        if (!valid)
            return false;
    }
    return true;
}

bool TracingReplayEvaluator::validateResponses(const std::vector<ResponseNode> & responses)
{
    for (const auto & resp : responses) {
        if (validatedNodes.count(resp.nodeHash))
            continue;

        try {
            // Parse only the request (to know what to re-execute).
            // The response is compared as raw bytes — no parsing needed.
            auto reqJson = cborStringToJson(resp.request);

            if (reqJson.contains("absPath")) {
                std::string path = reqJson["absPath"];
                auto currentHash = hashCache.getHash(path);

                // Re-serialize the current response to CBOR and compare bytes
                nlohmann::json currentRespJson = trace::FileReadResponse{currentHash};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (resp.response != currentCbor) {
                    tracingCacheLog("replay invalidated: file %s changed", path);
                    return false;
                }
            } else if (reqJson.contains("name")) {
                std::string name = reqJson["name"];
                auto currentVal = validationEnv.getEnv(name);

                nlohmann::json currentRespJson = trace::GetEnvResponse{currentVal};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (resp.response != currentCbor) {
                    tracingCacheLog("replay invalidated: env %s changed", name);
                    return false;
                }
            }
        } catch (const std::exception & e) {
            tracingCacheLog("replay: failed to parse dependency: %s", e.what());
            return false;
        }
        validatedNodes.insert(resp.nodeHash);
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

        // Walk forward: Query → Response* → Result
        // At each step, try all child responses — different recordings
        // may have branched from the same parent node.
        std::optional<ResultNode> resultNode;
        NodeHash current = shortcut.nodeHash;
        bool validPath = true;

        while (true) {
            auto results = tracingIndex.selectChildResults(current);
            if (!results.empty()) {
                resultNode = results[0];
                break;
            }

            auto responses = tracingIndex.selectChildResponses(current);
            if (responses.empty())
                break;

            // Try each sibling response until one validates
            bool foundValid = false;
            for (auto & resp : responses) {
                if (validatedNodes.count(resp.nodeHash) || validateResponses({resp})) {
                    current = resp.nodeHash;
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
        tracingCacheLog("replay hit: %s", Q::tag);
        return std::make_pair(
            resultNode->payload,
            TriePosition{
                .resultNodeHash = resultNode->nodeHash,
                .afterHash = resultNode->nodeHash,
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

    if (fnId && argId) {
        if (auto result = lookup(trace::QueryApply{*fnId, *argId})) {
            tracingCacheLog("replay hit: apply");
            return make_ref<TracingReplayObject>(
                *this, result->second, [this, fn, arg]() { return inner->apply(fn, arg); });
        }
        tracingCacheLog("replay miss: apply");
    }

    return inner->apply(fn, arg);
}

} // namespace nix

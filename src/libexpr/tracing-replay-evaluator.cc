#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace nix {

TracingReplayEvaluator::TracingReplayEvaluator(ref<Evaluator> inner, TracingIndex & tracingIndex)
    : inner(inner)
    , tracingIndex(tracingIndex)
{
}

bool TracingReplayEvaluator::validateDependencies(const NodeHash & queryNodeHash)
{
    // If already validated, skip
    if (validatedNodes.count(queryNodeHash))
        return true;

    auto responses = tracingIndex.selectDependencies(queryNodeHash);
    if (!validateResponses(responses))
        return false;

    // Mark as validated on success
    validatedNodes.insert(queryNodeHash);
    return true;
}

bool TracingReplayEvaluator::validateToValidatedNode(const NodeHash & queryNodeHash)
{
    // If already validated, skip
    if (validatedNodes.count(queryNodeHash))
        return true;

    bool reachedValidated = false;
    auto responses = tracingIndex.selectDependenciesUntilValidated(queryNodeHash, validatedNodes, reachedValidated);

    // If we didn't reach a validated node (went all the way to root), that's also valid
    // as long as the responses validate
    if (!validateResponses(responses))
        return false;

    // Mark as validated on success
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

bool TracingReplayEvaluator::validateResponses(const std::vector<ResponseNode> & responses)
{
    for (const auto & resp : responses) {
        try {
            auto reqJson = nlohmann::json::parse(resp.request);
            auto respJson = nlohmann::json::parse(resp.response);

            // Check if this is a file read response
            if (reqJson.contains("absPath") && respJson.contains("contentHash")) {
                std::string path = reqJson["absPath"];
                std::string expectedHash = respJson["contentHash"];

                auto currentHash = hashCache.getHash(path);
                if (currentHash.to_string(HashFormat::SRI, true) != expectedHash) {
                    debug("replay invalidated: file %s changed", path);
                    return false;
                }
            }
            // Check if this is an env lookup response
            else if (reqJson.contains("name") && respJson.contains("value")) {
                std::string name = reqJson["name"];
                const char * current = std::getenv(name.c_str());
                std::optional<std::string> currentVal;
                if (current)
                    currentVal = current;

                std::optional<std::string> expectedVal;
                if (!respJson["value"].is_null())
                    expectedVal = respJson["value"];

                if (currentVal != expectedVal) {
                    debug("replay invalidated: env %s changed", name);
                    return false;
                }
            }
        } catch (const nlohmann::json::exception & e) {
            debug("replay: failed to parse dependency: %s", e.what());
            return false;
        }
    }
    return true;
}

template<typename Q>
std::optional<std::pair<std::string, TriePosition>> TracingReplayEvaluator::lookup(const Q & query)
{
    auto queryHash = TracingIndex::computeQueryHash(query);
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);

    for (const auto & shortcut : shortcuts) {
        // Step 1: Validate responses BEFORE the query (from root to query)
        if (!validateDependencies(shortcut.nodeHash))
            continue;

        // Find the query node
        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        // Step 2: Find result following this query, collecting responses on the path
        // Walk forward from Query → Response* → Result
        std::vector<ResponseNode> responsesOnPath;
        std::optional<ResultNode> resultNode;
        NodeHash current = shortcut.nodeHash;

        while (true) {
            // Look for Result children
            auto results = tracingIndex.selectChildResults(current);
            if (!results.empty()) {
                resultNode = results[0];
                break;
            }

            // Look for Response children
            auto responses = tracingIndex.selectChildResponses(current);
            if (responses.empty()) {
                break;
            }

            responsesOnPath.push_back(responses[0]);
            current = responses[0].nodeHash;
        }

        if (!resultNode)
            continue;

        // Step 3: Validate responses AFTER the query (on the path to result)
        if (!validateResponses(responsesOnPath))
            continue;

        // Step 4: Mark result as validated and return
        validatedNodes.insert(resultNode->nodeHash);
        debug("replay hit: %s", Q::tag);
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

EvalState * TracingReplayEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingReplayEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    if (auto result = lookup(trace::QueryImport{displayPath})) {
        debug("replay hit: evalFile %s", displayPath);
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
    }

    debug("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    if (auto result = lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
        debug("replay hit: evalExpr");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
    }

    debug("replay miss: evalExpr");
    return inner->evalExpr(expr, basePath);
}

} // namespace nix

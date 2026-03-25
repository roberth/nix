#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <cstdlib>
#include <nlohmann/json.hpp>

namespace nix {

TracingReplayEvaluator::TracingReplayEvaluator(
    ref<Evaluator> inner, TracingIndex & tracingIndex, std::filesystem::path hashCacheDbPath)
    : inner(inner)
    , tracingIndex(tracingIndex)
    , hashCache(std::move(hashCacheDbPath))
{
}

bool TracingReplayEvaluator::validateDependencies(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    auto responses = tracingIndex.selectDependencies(queryNodeHash);
    if (!validateResponses(responses))
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

bool TracingReplayEvaluator::validateResponses(const std::vector<ResponseNode> & responses)
{
    for (const auto & resp : responses) {
        try {
            auto reqJson = nlohmann::json::parse(resp.request);
            auto respJson = nlohmann::json::parse(resp.response);

            if (reqJson.contains("absPath") && respJson.contains("contentHash")) {
                std::string path = reqJson["absPath"];
                std::string expectedHash = respJson["contentHash"];

                auto currentHash = hashCache.getHash(path);
                if (currentHash.to_string(HashFormat::SRI, true) != expectedHash) {
                    debug("replay invalidated: file %s changed", path);
                    return false;
                }
            } else if (reqJson.contains("name") && respJson.contains("value")) {
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
        if (!validateDependencies(shortcut.nodeHash))
            continue;

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        // Walk forward: Query → Response* → Result
        std::vector<ResponseNode> responsesOnPath;
        std::optional<ResultNode> resultNode;
        NodeHash current = shortcut.nodeHash;

        while (true) {
            auto results = tracingIndex.selectChildResults(current);
            if (!results.empty()) {
                resultNode = results[0];
                break;
            }

            auto responses = tracingIndex.selectChildResponses(current);
            if (responses.empty())
                break;

            responsesOnPath.push_back(responses[0]);
            current = responses[0].nodeHash;
        }

        if (!resultNode)
            continue;

        if (!validateResponses(responsesOnPath))
            continue;

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

EvalState & TracingReplayEvaluator::getEvalState()
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
    return inner->apply(fn, arg);
}

} // namespace nix

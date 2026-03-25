#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/file-hash-cache.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <set>
#include <vector>

namespace nix {

class TracingIndex;
struct ResponseNode;

/**
 * Evaluator that replays cached results from the tracing index.
 *
 * Uses the TracingIndex trie to look up cached query results via cascading
 * lookup (temporal → structural → shortcut). On cache miss, defers to the
 * inner evaluator.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingIndex & tracingIndex;
    FileHashCache hashCache;

    /**
     * Set of nodes whose dependencies have been validated.
     * Enables O(n) incremental validation instead of O(n²) per-query.
     */
    std::set<NodeHash> validatedNodes;

    /**
     * Try to find a cached result using the tracing index.
     * Returns nullopt on miss, or (resultPayload, triePosition) on hit.
     */
    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query);

public:
    TracingReplayEvaluator(
        ref<Evaluator> inner, TracingIndex & tracingIndex, std::filesystem::path hashCacheDbPath = {});

    /**
     * Validate a vector of response nodes against current environment.
     */
    bool validateResponses(const std::vector<ResponseNode> & responses);

    /**
     * Validate dependencies from root to queryNodeHash (full validation).
     * Used for shortcut lookups (strategy 3).
     */
    bool validateDependencies(const NodeHash & queryNodeHash);

    /**
     * Validate dependencies incrementally from nearest validated node.
     * Used for trie/structural lookups (strategies 1 & 2).
     */
    bool validateToValidatedNode(const NodeHash & queryNodeHash);

    void markValidated(const NodeHash & nodeHash);
    bool isValidated(const NodeHash & nodeHash) const;

    TracingIndex & getTracingIndex() { return tracingIndex; }

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    EvalState & getEvalState() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
};

} // namespace nix

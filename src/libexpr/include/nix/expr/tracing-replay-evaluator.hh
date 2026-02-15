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
 * Uses the TracingIndex to look up cached query results. On cache miss,
 * defers to the inner evaluator.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingIndex & tracingIndex;
    FileHashCache hashCache;

    /**
     * Set of nodes whose dependencies have been validated.
     * Used to avoid re-validating the same responses during incremental validation.
     */
    std::set<NodeHash> validatedNodes;

    /**
     * Try to find a cached result using the tracing index.
     * Returns nullopt on miss, or a pair of (resultPayload, triePosition) on hit.
     */
    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query);

public:
    /**
     * Validate a vector of response nodes.
     * Checks that each Response still produces the same result from the environment.
     */
    bool validateResponses(const std::vector<ResponseNode> & responses);

    /**
     * Validate dependencies for a lookup (from root to queryNodeHash).
     * Used by TracingReplayObject for shortcut lookups (strategy 3 of cascading lookup).
     * On success, marks queryNodeHash as validated.
     */
    bool validateDependencies(const NodeHash & queryNodeHash);

    /**
     * Validate dependencies incrementally (from any validated node to queryNodeHash).
     * Walks back from queryNodeHash until hitting a node in the validated set.
     * Used by TracingReplayObject for trie/structural lookups (strategies 1 & 2).
     * On success, marks queryNodeHash as validated.
     */
    bool validateToValidatedNode(const NodeHash & queryNodeHash);

    /**
     * Mark a node as validated (its dependencies have been checked).
     */
    void markValidated(const NodeHash & nodeHash);

    /**
     * Check if a node has already been validated.
     */
    bool isValidated(const NodeHash & nodeHash) const;

    /**
     * Get the tracing index.
     */
    TracingIndex & getTracingIndex()
    {
        return tracingIndex;
    }

    /**
     * Create a replay evaluator.
     * @param inner The wrapped evaluator for cache misses
     * @param tracingIndex The tracing index for cached lookups
     */
    TracingReplayEvaluator(ref<Evaluator> inner, TracingIndex & tracingIndex);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;

    EvalState * getEvalState() override;
};

} // namespace nix

#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <set>
#include <vector>

namespace nix {

class Environment;
class TracingIndex;
struct QueryNode;
struct ResultNode;

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
    TracingWriter & writer; // shared with TracingEvaluator for virtual root counter

    /**
     * Environment for validating cached responses during replay.
     *
     * File hash queries and env var lookups flow through this environment,
     * so that outer tracing layers can observe inner replay validation
     * reads (input-traced nesting model).
     */
    Environment & validationEnv;

    /**
     * Set of nodes whose dependencies have been validated.
     * Enables O(n) incremental validation instead of O(n²) per-query.
     */
    std::set<NodeHash> validatedNodes;

    /**
     * Temporal cursor — mirrors TracingWriter::afterHash on the recording side.
     * Advances as replay lookups succeed, enabling temporal (trie-following)
     * strategy for subsequent queries on the same or different objects.
     */
    std::optional<NodeHash> temporalCursor;

    /**
     * Ambient replay state for validating ambient interaction events.
     * Maps recorded ambient ids (the "from" field in query payloads)
     * to current Objects. Populated during apply() before lookup(),
     * extended as child-producing queries are validated.
     */
    struct AmbientReplayState
    {
        std::map<std::string, std::shared_ptr<Object>> idToObject;
        /// Root Objects without trie identity, assigned lazily to the
        /// first unseen "from" id during the walk.
        std::vector<std::shared_ptr<Object>> unresolvedRoots;
        /// Child Objects from queries that produce children (getAttr,
        /// getListElem), awaiting assignment to the next unseen id.
        std::vector<std::shared_ptr<Object>> pendingChildren;
    };

    std::optional<AmbientReplayState> ambientState;

    /**
     * Sets-based replay state: the accumulated (queryHash, responseHash)
     * pairs observed so far in this replay session. Kept sorted by
     * queryHash so isSubset can compare in linear merge.
     *
     * Used by `lookup<Q>` to consult the sets-based index in addition
     * to the temporal-trie walker. Each successful lookup (sets-based
     * or trie) appends its (queryHash, responseHash) so downstream
     * lookups see this query as part of their precondition context.
     *
     * Lifetime: tied to this `TracingReplayEvaluator` instance and
     * grows monotonically. Single-shot CLI invocations create a fresh
     * instance per session so this is naturally bounded. Long-lived
     * hosts (a daemon embedding TracingReplayEvaluator) should
     * destroy and recreate the evaluator between user requests to
     * avoid cross-request context leakage (which would be merely
     * inefficient — not incorrect, since each (queryHash,
     * responseHash) is content-addressed and false hits would
     * require both colliding hashes).
     */
    TracingIndex::SetMembers currentSetMembers;

    /**
     * Insert (queryHash, responseHash) into currentSetMembers,
     * maintaining the sorted invariant. If a duplicate queryHash with
     * a different responseHash is inserted, the new value overrides
     * (the more recent observation wins — should be a no-op in
     * deterministic evaluation but defended for safety).
     */
    void addToCurrentSetMembers(const QueryHash & queryHash, const Hash & responseHash);

    /**
     * Dispatch an ambient query against the current Objects.
     * Returns serialized CBOR result, or nullopt if the query can't
     * be dispatched (unknown id, unsupported query type).
     */
    std::optional<std::string> dispatchAmbientQuery(const nlohmann::json & reqJson);

    /**
     * Try to find a cached result using the tracing index.
     * Returns nullopt on miss, or (resultPayload, triePosition) on hit.
     */
    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query);

public:
    /**
     * @param validationEnv Environment for validation during replay.
     *        File hash and env var queries flow through this environment
     *        so outer tracing layers observe inner reads.
     */
    TracingReplayEvaluator(
        ref<Evaluator> inner, TracingIndex & tracingIndex, Environment & validationEnv, TracingWriter & writer);

    /**
     * Validate a vector of (query, result) pairs against current environment.
     * The query payload encodes the request, the result payload encodes the response.
     */
    bool validateDeps(const std::vector<std::pair<QueryNode, ResultNode>> & deps);

    /**
     * Compute the current response for a recorded request.
     * Reads current file hash or env var through validationEnv,
     * serializes to CBOR. Returns nullopt on parse failure.
     */
    std::optional<std::string> getCurrentResponse(const std::string & requestCbor);

    /**
     * Like validateDeps, but tolerates duplicate entries for the
     * same request (from multiple recordings sharing a trie prefix).
     * For each unique request, at least one result must validate.
     */
    bool validateDepsAnyMatch(const std::vector<std::pair<QueryNode, ResultNode>> & deps);

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

    std::optional<NodeHash> getTemporalCursor() const
    {
        return temporalCursor;
    }

    void setTemporalCursor(const NodeHash & nodeHash)
    {
        temporalCursor = nodeHash;
    }

    TracingIndex & getTracingIndex()
    {
        return tracingIndex;
    }

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    EvalState & getEvalState() override;

    ref<Object> evalFile(const RootedPath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkInt(NixInt i) override;
    ref<Object> mkBool(bool b) override;
    ref<Object> mkPath(const RootedPath & path) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> getInternalPrimOp(const std::string & name) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
};

} // namespace nix

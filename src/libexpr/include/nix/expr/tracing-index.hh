#pragma once
/**
 * @file
 * SQLite-based trie index for tracing evaluation cache.
 *
 * Implements the data model from doc/tracing-index-data-model.md.
 * The index stores evaluation traces in a trie structure where:
 * - Query nodes represent user queries (evalFile, getAttr, etc.)
 *   depth=0 for top-level evaluation queries, depth>0 for environment events
 * - Result nodes represent the results of queries
 *
 * Nodes are identified by content hashes, making inserts idempotent.
 * A shortcut table provides O(1) lookup for queries by their semantic hash.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct sqlite3;

namespace nix {

struct SQLite;
struct SQLiteStmt;

/**
 * A node hash identifying a node in the trie.
 * Computed as hash(afterHash, content) where content depends on node type.
 */
using NodeHash = Hash;

/**
 * A query hash identifying the semantic content of a query.
 * Computed as hash(operation, params, inputHashes).
 */
using QueryHash = Hash;

/**
 * Result of looking up a query in the shortcut table.
 */
struct ShortcutEntry
{
    NodeHash nodeHash;
    int64_t createdAt;
};

/**
 * A query node from the Queries table.
 */
struct QueryNode
{
    NodeHash nodeHash;
    QueryHash queryHash;
    std::optional<NodeHash> afterHash;
    std::optional<NodeHash> structuralParent;
    int depth = 0;
};

/**
 * A result node from the Results table.
 */
struct ResultNode
{
    NodeHash nodeHash;
    NodeHash afterHash;
    std::string payload; // Serialized result
    std::optional<NodeHash> queryNodeHash; // Which Query this Result answers
};

/**
 * SQLite-based trie index for tracing evaluation cache.
 *
 * Thread-safe via internal synchronization.
 */
class TracingIndex
{
public:
    /**
     * Open or create a tracing index at the default location.
     * Default: ~/.cache/nix/eval-tracing-index-v1/index.sqlite
     */
    TracingIndex();

    /**
     * Open or create a tracing index at the specified path.
     */
    explicit TracingIndex(const std::filesystem::path & dbPath);

    ~TracingIndex();

    TracingIndex(const TracingIndex &) = delete;
    TracingIndex & operator=(const TracingIndex &) = delete;

    // -------------------------------------------------------------------------
    // Hash computation utilities
    // -------------------------------------------------------------------------

    /**
     * Compute a query hash from a query payload.
     * The query's `from` field contains the parent's queryHash (Merkle identity),
     * so the computed hash encodes the complete provenance chain.
     */
    template<typename Q>
    static QueryHash computeQueryHash(const Q & query);

    /**
     * Compute a node hash for a query node.
     * nodeHash = hash(afterHash, queryHash)
     */
    static NodeHash computeQueryNodeHash(const std::optional<NodeHash> & afterHash, const QueryHash & queryHash);

    /**
     * Compute a node hash for a result node.
     * nodeHash = hash(afterHash, payload)
     */
    static NodeHash computeResultNodeHash(const NodeHash & afterHash, const std::string & payload);

    // -------------------------------------------------------------------------
    // Recording operations (insert into trie)
    // -------------------------------------------------------------------------

    /**
     * Insert a query node into the trie.
     * Also inserts a shortcut entry for fast lookup.
     * Idempotent: duplicate inserts are ignored.
     *
     * @param afterHash The predecessor node (Result nodeHash, or nullopt for root)
     * @param queryHash The semantic hash of the query
     * @param payload Serialized query payload (for QueryPayloads table)
     * @param structuralParent The Result nodeHash for structural lookups (attr/index)
     * @return The nodeHash of the inserted/existing query node
     */
    NodeHash insertQuery(
        const std::optional<NodeHash> & afterHash,
        const QueryHash & queryHash,
        const std::string & payload,
        const std::optional<NodeHash> & structuralParent = std::nullopt,
        int depth = 0);

    /**
     * Insert a result node into the trie.
     * Idempotent: duplicate inserts are ignored.
     *
     * @param afterHash The predecessor node (Query nodeHash or another Result)
     * @param payload Serialized result payload
     * @return The nodeHash of the inserted/existing result node
     */
    NodeHash insertResult(
        const NodeHash & afterHash,
        const std::string & payload,
        const std::optional<NodeHash> & queryNodeHash = std::nullopt);

    // -------------------------------------------------------------------------
    // Query operations
    //
    // Operations that select nodes return candidates recorded in past traces.
    // The replay logic must validate that the current environment matches
    // before using cached results.
    // -------------------------------------------------------------------------

    /**
     * Select shortcuts for a query hash.
     * Returns all matching shortcut entries, ordered by createdAt descending.
     */
    std::vector<ShortcutEntry> selectShortcuts(const QueryHash & queryHash);

    /**
     * Get a query node by its nodeHash.
     */
    std::optional<QueryNode> getQuery(const NodeHash & nodeHash);

    /**
     * Get a result node by its nodeHash.
     */
    std::optional<ResultNode> getResult(const NodeHash & nodeHash);

    /**
     * Get the query payload for a queryHash.
     */
    std::optional<std::string> getQueryPayload(const QueryHash & queryHash);

    /**
     * Select candidate child queries of a result node.
     * Returns queries that followed this result in past traces
     * (multiple possible worlds).
     */
    std::vector<QueryNode> selectChildQueries(const NodeHash & resultNodeHash);

    /**
     * Get the child result of a node (0..1).
     * Temporally singular; across possible worlds, identical response
     * chains produce identical results (deterministic evaluation).
     */
    std::optional<ResultNode> getChildResult(const NodeHash & afterHash);

    /**
     * Select candidate child results of a response or query node.
     * Returns results that followed this node in past traces.
     */
    std::vector<ResultNode> selectChildResults(const NodeHash & afterHash);

    /**
     * Select queries by structural parent and queryHash.
     *
     * Unlike temporal queries (selectChild*), structural lookup follows the
     * semantic relationship between values. For example, getAttr queries have
     * a structural parent pointing to the attrset Result, regardless of what
     * other queries or responses occurred in between.
     *
     * Returns multiple results because there may be multiple possible futures
     * recorded in past traces for the same structural query.
     */
    std::vector<QueryNode> selectStructuralChildren(const NodeHash & structuralParent, const QueryHash & queryHash);

    /**
     * Select all environment queries (depth > 0) on the path from a query
     * node back to root. Used for validating dependencies when replaying
     * from a shortcut.
     *
     * @param queryNodeHash Starting point (a Query node)
     * @return Vector of (query, result) pairs in order from root to queryNodeHash
     */
    std::vector<std::pair<QueryNode, ResultNode>> selectDependencies(const NodeHash & queryNodeHash);

    /**
     * Select environment queries on the path from a query node back to any
     * validated node. Used for incremental validation.
     *
     * @param queryNodeHash Starting point (a Query node)
     * @param validatedNodes Set of nodes whose dependencies have been validated
     * @param reachedValidated Output: set to true if we reached a validated node
     * @return Vector of (query, result) pairs from validated node (or root) to queryNodeHash
     */
    std::vector<std::pair<QueryNode, ResultNode>> selectDependenciesUntilValidated(
        const NodeHash & queryNodeHash, const std::set<NodeHash> & validatedNodes, bool & reachedValidated);

    /**
     * Walk forward from a query node through depth>0 events to find
     * the depth=0 Result. Holds the lock for the entire walk.
     *
     * @param validator Called for each depth>0 event with
     *        (queryPayload, resultNodeHash, resultPayload).
     *        Returns true if the event validates against current state.
     * @return The final Result node, or nullopt if validation fails.
     */
    std::optional<ResultNode> findResult(
        const NodeHash & queryNodeHash,
        std::function<bool(const std::string & queryPayload, const NodeHash & resultNodeHash, const std::string & resultPayload)> validator);

private:
    struct State;
    std::unique_ptr<Sync<State>> _state;

    /** Write queue + background writer thread. */
    struct WriteQueue;
    std::unique_ptr<WriteQueue> _writeQueue;
};

// -------------------------------------------------------------------------
// Template implementations
// -------------------------------------------------------------------------

template<typename Q>
QueryHash TracingIndex::computeQueryHash(const Q & query)
{
    // Serialize the query to JSON and hash it.
    // The "from" field contains the parent's queryHash (Merkle identity),
    // so the full query hash encodes the complete provenance chain.
    nlohmann::json j = query;
    std::string serialized = j.dump();
    return hashString(HashAlgorithm::SHA256, serialized);
}

} // namespace nix

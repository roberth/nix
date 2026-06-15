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
#include <atomic>
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
    std::string payload;                   // Serialized result
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
     * Default: ~/.cache/nix/eval-tracing-index-v2/index.sqlite
     */
    TracingIndex();

    /**
     * Open or create a tracing index at the specified path.
     */
    explicit TracingIndex(const std::filesystem::path & dbPath);

    ~TracingIndex();

    /**
     * Flush all active WriteQueues process-wide. Call before exec()
     * or other operations that bypass normal C++ destruction.
     *
     * Destructive: joins the writer threads. Subsequent writes to
     * any TracingIndex in this process will not actually be
     * persisted. Use `waitForWrites` for a non-destructive flush
     * confined to a single index.
     */
    static void flushAllWriteQueues();

    /**
     * Block until every write enqueued to this index before the
     * call has been committed and checkpointed. Non-destructive:
     * the writer thread keeps running and subsequent writes work.
     */
    void waitForWrites();

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
     * nodeHash = hash(afterHash, payload, queryNodeHash)
     */
    static NodeHash computeResultNodeHash(
        const NodeHash & afterHash,
        const std::string & payload,
        const std::optional<NodeHash> & queryNodeHash = std::nullopt);

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
        std::function<bool(
            const QueryHash & d0EventQueryHash,
            const std::string & queryPayload,
            const NodeHash & resultNodeHash,
            const std::string & resultPayload)> validator);

    // -------------------------------------------------------------------------
    // Sets-based index (see doc/tracing-sets-index-data-model.md)
    //
    // Each Binding maps (queryHash, precondition ResponseSet) to a Response.
    // A precondition ResponseSet is an unordered set of (d>0 queryHash,
    // d>0 responseHash) pairs that the box observed while computing the
    // Response. Lookup finds a Binding whose precondition is a subset of
    // the current ResponseSet.
    // -------------------------------------------------------------------------

    /**
     * One member of a ResponseSet: a (d>0 queryHash, d>0 responseHash) pair.
     */
    struct SetMember
    {
        QueryHash queryHash;
        Hash responseHash;

        bool operator==(const SetMember & other) const
        {
            return queryHash == other.queryHash && responseHash == other.responseHash;
        }

        bool operator<(const SetMember & other) const
        {
            return queryHash < other.queryHash
                   || (queryHash == other.queryHash && responseHash < other.responseHash);
        }
    };

    /**
     * A ResponseSet's contents as a vector sorted by queryHash. Two
     * SetMembers with the same queryHash but different responseHash are
     * a contradiction at recording time and should not coexist.
     */
    using SetMembers = std::vector<SetMember>;

    /**
     * Canonical content hash of a SetMembers vector. Members must be
     * sorted ascending by queryHash.
     */
    static Hash computePreconditionSetHash(const SetMembers & members);

    /**
     * Canonical content hash of a response payload.
     */
    static Hash computeResponseHash(const std::string & payload);

    /**
     * Insert a PreconditionSet. Idempotent: returns the same setHash
     * for identical inputs.
     *
     * @param members must be sorted ascending by queryHash.
     * @return The setHash.
     */
    Hash insertPreconditionSet(const SetMembers & members);

    /**
     * Fetch the members of a stored PreconditionSet.
     */
    std::optional<SetMembers> getPreconditionSet(const Hash & setHash);

    /**
     * Insert a response payload. Idempotent.
     */
    Hash insertSetResponse(const std::string & payload);

    /**
     * Fetch a stored response payload.
     */
    std::optional<std::string> getSetResponse(const Hash & responseHash);

    /**
     * Insert a Binding. Idempotent on (queryHash, preconditionHash).
     */
    void insertBinding(const QueryHash & queryHash, const Hash & preconditionHash, const Hash & responseHash);

    /**
     * Look up a cached Response for a Query in the current context.
     *
     * Iterates the Bindings for `queryHash` and returns the response of
     * the first one whose precondition is a subset of `current`.
     *
     * @param current must be sorted ascending by queryHash.
     */
    std::optional<std::string> lookupSetsReplay(const QueryHash & queryHash, const SetMembers & current);

    /**
     * Test whether `precondition` is a subset of `current`. Both must
     * be sorted ascending by queryHash. Exposed for tests and for
     * callers that already have the materialised members.
     */
    static bool isSubset(const SetMembers & precondition, const SetMembers & current);

    /**
     * Intersection of two SetMembers under equality (queryHash AND
     * responseHash). Members appearing in both inputs are kept;
     * members appearing in only one are dropped. Members with the
     * same queryHash but differing responseHash are dropped from
     * both — that's a contradictory observation.
     *
     * Both inputs must be sorted ascending by queryHash. Output is
     * sorted by construction. Building block for the future
     * intersection-learning step where two bindings for the same
     * queryHash producing the same Response with differing
     * preconditions can be combined into a tighter, learned
     * precondition.
     */
    static SetMembers intersectSets(const SetMembers & a, const SetMembers & b);

    /**
     * 256-bit Bloom filter summary of a SetMembers vector. The
     * filter encodes membership of each (queryHash, responseHash)
     * pair: 8 bit positions per member, derived by chunking
     * SHA256(queryHash || responseHash) into 8 × 32-bit indices
     * mod 256.
     *
     * For subset prescreen, `(P_bloom & C_bloom) == P_bloom`
     * implies "P may be a subset of C" (with false positives at
     * the rate of standard Bloom analysis). `==` failing implies
     * definitely-not-subset, so the slow HAMT-walking subset test
     * can be skipped on those candidates. Building block for the
     * lookupSetsReplay prescreen wiring; not yet stored alongside
     * PreconditionSets.
     */
    static constexpr size_t kBloomBytes = 32;
    using Bloom = std::array<uint8_t, kBloomBytes>;
    static Bloom computeBloom(const SetMembers & members);

    /**
     * `(P_bloom & C_bloom) == P_bloom`. Returns true when P_bloom
     * may be a subset of C_bloom (false positives possible);
     * false when it is definitely not.
     */
    static bool bloomMayBeSubset(const Bloom & p, const Bloom & c);

    /**
     * Run an intersection-learning pass for a single queryHash.
     *
     * For each pair of existing Bindings with the same queryHash and
     * the same Response but different preconditions, compute the
     * intersection of those preconditions and insert it as a new
     * Binding if it's strictly smaller than both inputs.
     *
     * The new tighter Binding doesn't replace the old ones — they
     * coexist; lookup will hit on the smallest precondition that
     * subset-matches the current context. Idempotent on repeated
     * runs: a recorded intersection that's already in the table is
     * a no-op insert.
     *
     * Cost: O(k²) materialise+intersect+insert work where k is the
     * number of Bindings for `queryHash`. Designed to run from a
     * compaction / maintenance entrypoint rather than on every
     * write, so a noisy queryHash (large k) doesn't slow recording.
     *
     * @return number of new Bindings inserted.
     */
    size_t runLearningPass(const QueryHash & queryHash);

    /**
     * Iterate every distinct queryHash that has at least one
     * Binding in the sets-based index. Pulls them all in memory; the
     * count is bounded by the number of distinct granular Queries
     * that recordings have ever produced (typically thousands at
     * most, easily held in memory).
     */
    std::vector<QueryHash> listBindingQueryHashes();

    /**
     * Count Bindings under a given queryHash. Primarily for tests
     * and for the eval-cache stats CLI.
     */
    size_t countBindings(const QueryHash & queryHash);

    /**
     * Delete PreconditionSets and SetResponses rows that no Binding
     * references. Intended to run after a compaction pass that
     * evicted Bindings (which may have left their PreconditionSet
     * and Response rows orphaned).
     *
     * @return (preconditionSets_deleted, setResponses_deleted).
     */
    std::pair<size_t, size_t> runGC();

    /**
     * One-shot maintenance pass: enumerate every queryHash that has
     * any Binding, run intersection-learning + eviction on each,
     * wait for the writer to drain, then run runGC (which also
     * VACUUMs). Returned struct summarises the cleanup so a CLI or
     * scheduled task can report what changed.
     */
    struct CompactResult
    {
        size_t scannedQueryHashes;
        size_t bindingsInserted;
        size_t preconditionSetsDropped;
        size_t setResponsesDropped;
    };
    CompactResult compactAll();

    /**
     * Sets-based cache statistics for the eval-cache stats CLI and
     * dev tooling. Cheap COUNT queries — no traversal.
     */
    struct CacheStats
    {
        size_t queryHashesWithBindings;
        size_t totalBindings;
        size_t preconditionSets;
        size_t setResponses;
        /* Process-local counters: incremented during lookupSetsReplay.
           Don't persist across process restarts — they describe the
           cache layer's effectiveness in this session. */
        uint64_t lookupHits;
        uint64_t lookupMisses;
        uint64_t lookupBloomPrescreenSkips;
    };
    CacheStats getStats();

private:
    struct State;

    /** Write queue + background writer thread. */
    struct WriteQueue;
    std::unique_ptr<Sync<State>> _state;
    std::unique_ptr<WriteQueue> _writeQueue;

    /* Process-local lookup counters. Atomic so the lookup path can
       bump them without taking the state lock; getStats() reads
       them with relaxed ordering since they're advisory. */
    std::atomic<uint64_t> _lookupHits{0};
    std::atomic<uint64_t> _lookupMisses{0};
    std::atomic<uint64_t> _lookupBloomPrescreenSkips{0};
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

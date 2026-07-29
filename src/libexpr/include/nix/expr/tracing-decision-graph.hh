#pragma once
/**
 * @file
 * SQLite-based decision-graph index for the tracing evaluation cache.
 *
 * Implements the schema described in doc/design/tracing-eval-cache.md.
 * Two layers:
 *
 *   1. Storage layer — content-addressed pools of atoms (Request,
 *      Response, Query, Result) and sets (RequestSet, FactSet). Each
 *      pool is keyed by content hash; INSERT OR IGNORE dedupes
 *      identical content automatically.
 *
 *   2. Decision graph layer — two edge tables on top of the storage
 *      pool hashes: `Asks(Q, factSet) -> requestSet` and
 *      `Terminals(Q, factSet) -> result`. The entry point is
 *      pinned to (Q, empty FactSet); no entry-point index needed.
 *
 * See the "Open work" section of `doc/design/tracing-eval-cache.md`
 * for candidate follow-ups.
 */

#include "nix/util/hash.hh"
#include "nix/util/sync.hh"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nix {

struct SQLite;
struct SQLiteStmt;

class TracingDecisionGraph
{
public:
    /* Role-keyed hash aliases for readability. All are SHA-256 in
       the canonical Hash type — distinguishing them is a typing
       discipline, not a runtime check. */
    using RequestHash = Hash;  // d>0 query atom
    using ResponseHash = Hash; // d>0 response atom
    using QueryHash = Hash;    // d=0 query atom (operation + params + inputHashes)
    using ResultHash = Hash;   // d=0 result atom
    using SetHash = Hash;      // RequestSet or FactSet content hash

    /* A (Request, Response) pair. The atomic unit of observed
       evaluation context. Facts aren't separately content-addressed
       — they live inline in FactSet members. */
    struct Fact
    {
        RequestHash request;
        ResponseHash response;

        bool operator==(const Fact & o) const
        {
            return request == o.request && response == o.response;
        }

        bool operator<(const Fact & o) const
        {
            if (request != o.request)
                return request < o.request;
            return response < o.response;
        }
    };

    /* Compute the canonical Query hash for a Query type. JSON-serialise
       and SHA-256. */
    template<typename Q>
    static QueryHash computeSelectorHash(const Q & query);

    /* Compute the canonical Response hash for a serialised payload. */
    static Hash computeResponseHash(const std::string & payload);

    /* Open or create a decision-graph index at the default location.
       Default: ~/.cache/nix/eval-tracing-decision-graph/index.sqlite
       NIX_TRACING_CACHE_DIR overrides the default for hermetic tests. */
    TracingDecisionGraph();

    /* Open or create an index at the specified path. */
    explicit TracingDecisionGraph(const std::filesystem::path & dbPath);

    ~TracingDecisionGraph();

    TracingDecisionGraph(const TracingDecisionGraph &) = delete;
    TracingDecisionGraph & operator=(const TracingDecisionGraph &) = delete;

    /* ─────────────────────────────────────────────────────────────────
       Storage layer: atomic content-addressed pools
       ───────────────────────────────────────────────────────────────── */

    /* Insert an atom into its content-addressed pool. The hash is
       computed by the caller (SHA-256 of the payload). Idempotent:
       INSERT OR IGNORE on the hash. */
    void insertRequest(const RequestHash & h, std::string_view payload);
    void insertSelector(const QueryHash & h, std::string_view payload);
    void insertResult(const ResultHash & h, std::string_view payload);

    /* Atom payload lookup by hash. Returns nullopt if not present. */
    std::optional<std::string> getRequestPayload(const RequestHash & h);

    /* True if the request payload names a cb-apply
       (`"query":"apply"`). Consumed only by walk()'s apply-bypass
       fallback pass, which is a candidate for removal once the
       walker no longer needs to guess sibling cb-apply positions.
       If that path goes, this method goes with it. */
    bool isApplyRequest(const RequestHash & h);
    std::optional<std::string> getSelectorPayload(const QueryHash & h);
    std::optional<std::string> getResultPayload(const ResultHash & h);

    /* ObservationSet CAS pool. An observation set is the set of
       (selectorHash, responsePayload) tuples the outer's callback
       probed on an inner-supplied contra-arg during one callback
       firing. Payload is inline so the walker reconstructs arg
       responses directly from this CAS.

       Referenced by `SelectorCallbackApply.argObsSet` — different
       observation sets give different queryHashes → distinct DB
       rows.

       Members hash: SHA-256 of the sorted-by-selectorHash CBOR of the
       member list. Idempotent via INSERT OR IGNORE. */
    struct Observation
    {
        Hash selectorHash{HashAlgorithm::SHA256};
        /* CBOR bytes of the observed response (a
           `trace::ResultVariant`). Used by the walker at replay to
           serve callback probes via an obsSet-answering proxy. */
        std::string responsePayload;

        bool operator==(const Observation & o) const
        {
            return selectorHash == o.selectorHash && responsePayload == o.responsePayload;
        }

        bool operator<(const Observation & o) const
        {
            if (selectorHash != o.selectorHash)
                return selectorHash < o.selectorHash;
            return responsePayload < o.responsePayload;
        }
    };
    static Hash computeObservationSetHash(std::vector<Observation> members);
    Hash insertObservationSet(std::vector<Observation> members);
    std::optional<std::vector<Observation>> getObservationSet(const Hash & h);


    /* ─────────────────────────────────────────────────────────────────
       Storage layer: set pools (content-addressed by canonical hash)
       ───────────────────────────────────────────────────────────────── */

    /* Canonical hash computation: SHA-256 of the CBOR encoding of
       the sorted, deduplicated member list. Same set → same hash
       regardless of insertion order. */
    static SetHash computeRequestSetHash(const std::vector<RequestHash> & members);
    static SetHash computeFactSetHash(const std::vector<Fact> & members);

    /* The hash of the empty set, common enough to be a constant. */
    static SetHash emptySetHash();

    /* Insert a set into its pool by its full member list. Computes
       the canonical hash, deduplicates, and stores. Returns the
       canonical hash. Idempotent on hash. */
    SetHash insertRequestSet(std::vector<RequestHash> members);
    SetHash insertFactSet(std::vector<Fact> members);

    /* Make the given factSet members available to record() under the
       caller-provided hash, *without* re-sorting or re-folding.
       Caller is responsible for ensuring the hash matches an XOR-fold
       over the per-element hashes of these (deduplicated) members. */
    void installFactSet(const SetHash & hash, const std::vector<Fact> & members);

    /* XOR-fold one Fact into a running set hash. Used by callers that
       maintain their factSet hash incrementally to avoid the O(N)
       cost of insertFactSet. */
    static Hash xorFactIntoHash(const Hash & h, const Hash & request, const Hash & response);

    /* XOR two set-hashes. Used by stateHashAt and similar
       XOR-fold operations over factSet hashes. */
    static Hash xorHashes(const Hash & a, const Hash & b);

    /* Extend an existing set by adding new members. Computes the
       resulting set's canonical hash, inserts it into the pool if
       not already present, and returns the new set hash.

       Cost: loads the existing set's members (O(|set|)), unions
       with new members (O(|set| + |new|)), and hashes the result.
       Acceptable at Phase 1 scale; a future optimisation could
       cache materialised member lists or switch to an additive
       hash scheme. */
    SetHash extendRequestSet(const SetHash & parent, const std::vector<RequestHash> & extras);
    SetHash extendFactSet(const SetHash & parent, const std::vector<Fact> & extras);

    /* Read a set's full member list. Returns nullopt if the set
       hash isn't in the pool. */
    std::optional<std::vector<RequestHash>> getRequestSet(const SetHash & h);
    std::optional<std::vector<Fact>> getFactSet(const SetHash & h);

    /* Useful dispatch: the subset of an edge's requestSet whose
       Responses aren't already in cur's facts. This is what record()
       inspects when deciding whether to follow or split an existing
       edge, and what walk() actually dispatches at each step.
       Doc reference: §"Invariant: pairwise-disjoint useful
       dispatches per (Q, FactSet)". */
    static std::vector<RequestHash> usefulDispatch(
        const std::vector<RequestHash> & edgeRequestSet,
        const std::unordered_set<RequestHash> & dispatchedSoFar);

    /* ─────────────────────────────────────────────────────────────────
       Decision graph layer: edges keyed by (Q, factSet)
       ───────────────────────────────────────────────────────────────── */

    /* Insert an Asks edge: at (Q, factSet), the box's next set of
       Requests is `requestSet`. Optional `altRequestSet` records a
       fallback the walker tries if primary's fold reaches a dead end
       (see "state/observation-creep canonicalisation" in the main
       doc). Idempotent on
       (selectorHash, factSetHash, requestSetHash) — alt column is
       stamped on first insert; subsequent inserts at the same key
       are dropped (INSERT OR IGNORE), so alt on a later insert with
       a different alt value has no effect. */
    void insertAsk(
        const QueryHash & q,
        const SetHash & factSet,
        const SetHash & requestSet,
        const std::optional<SetHash> & altRequestSet = std::nullopt);

    struct AskEdge
    {
        SetHash requestSet;
        std::optional<SetHash> altRequestSet;
    };

    /* Look up the outgoing Ask edges at (Q, factSet). */
    std::vector<AskEdge> getAsks(const QueryHash & q, const SetHash & factSet);


    /* Remove a specific Asks edge. Used by Patricia split to
       re-point an existing edge. */
    void removeAsk(const QueryHash & q, const SetHash & factSet, const SetHash & requestSet);

    /* Insert a Terminal: at (Q, factSet), the recorded Result for
       Q is `result`. Idempotent on
       (selectorHash, factSetHash, resultHash). */
    void insertTerminal(const QueryHash & q, const SetHash & factSet, const ResultHash & result);

    /* Look up the Terminal at (Q, factSet). Returns the Result hash
       if a Terminal exists. (Phase 1: only one Terminal per
       position is expected; nondeterminism handling is deferred.) */
    std::optional<ResultHash> getTerminal(const QueryHash & q, const SetHash & factSet);

    /* Copy every outgoing Ask and Terminal at (q, srcCur) into
       (q, dstCur). Used by the walk-side alt fallback: on primary miss
       and alt success, the alt's fold lands at srcCur; copying its
       outgoing state onto the primary's fold target (dstCur) lets
       future walks reach the recorded continuation directly from
       primary without the fallback. Idempotent via INSERT OR IGNORE. */
    void copyOutgoing(const QueryHash & q, const SetHash & srcCur, const SetHash & dstCur);

    /* Insert an Ask row at (q, cur) whose Request set is derived from
       `facts`, applying Patricia split against existing Asks at the
       same (q, cur) so that after the call all outgoing Asks at that
       cur are pairwise-disjoint (or identical). Cascades: after
       splitting the shared prefix off, the remaining new-side content
       is inserted at (q, cur + fold(shared)); the same discipline
       applies there.

       `dispatchedSoFar` filters existing Asks' Request members before
       computing the overlap — a request already folded into cur must
       not appear in `shared`, since XOR-folding it again would cancel
       it out of cur and place the intermediate at the wrong position.
       Callers with a running per-recording `dispatchedSoFar` pass it
       explicitly; callers with none (or with `cur == emptySetHash()`)
       pass empty.

       `alt` is preserved on the new Ask when it lands without split,
       and dropped when split fires — the split changes the row's
       identity and the alt was designed for the original whole rs.
       Existing Asks being split preserve their own alts on the
       tail. */
    void insertAskSplitting(
        const QueryHash & q,
        const SetHash & cur,
        const std::vector<Fact> & facts,
        const std::unordered_set<RequestHash> & dispatchedSoFar = {},
        const std::optional<SetHash> & alt = std::nullopt);

    /* Cheap existence check: does any Asks or Terminal row exist at
       (Q, factSet)? Used by walk() to validate that a candidate
       FactSet hash lies on some recording for this query, without
       persisting FactSet members. */
    bool hasAnyEdge(const QueryHash & q, const SetHash & factSet);

    /* ─────────────────────────────────────────────────────────────────
       Recording and replay
       ───────────────────────────────────────────────────────────────── */

    /* Integrate (q, factSet, result) into the decision graph as a
       recording. Walks the factSet's Facts in canonical order,
       writing one singleton Asks edge per Fact and a Terminal at
       the end. Idempotent on repeat: duplicate edges and terminals
       are absorbed by INSERT OR IGNORE.

       Set canonicity dedupes shared prefixes across recordings of
       the same Q automatically; different Responses to the same
       Request land at different (Q, FactSet) positions and so
       coexist without conflict. Patricia split for the residual
       multi-Request-overlap case is a deferred optimisation. */
    void record(const QueryHash & q, const SetHash & factSet, const ResultHash & result);

    /* Fast-path overload: the caller maintains responseFor (request →
       response) and allRequests (= factSet.requests) incrementally,
       handing them in by reference so record() skips the O(N)
       rebuild of these structures from getFactSet() members on every
       call. TracingWriter uses this; tests use the simpler overload
       above.

       `startFactSetHash` is the explicit start point of this Q's Ask
       chain — the "structural parent factSet" (parent Query's
       terminalCur). Defaults to emptySetHash(), giving from-∅
       behavior. Non-empty values anchor the chain at parent's
       terminal so the walker's parentAnchor path finds Q-labeled
       Asks there. */
    void record(
        const QueryHash & q,
        const SetHash & factSet,
        const ResultHash & result,
        const std::unordered_map<Hash, Hash> & responseFor,
        const std::unordered_set<Hash> & allRequests,
        SetHash startFactSetHash);

    /* Fastest overload: caller also supplies the canonical RequestSet
       hash for the *current* allRequests (e.g. from an incremental
       TrieBuilder). When record() falls through to the whole-remaining
       insert at cur=startFactSetHash, it uses this precomputed hash
       directly and avoids re-running insertRequestSet over
       allRequests. See other overload for `startFactSetHash`. */
    void record(
        const QueryHash & q,
        const SetHash & factSet,
        const ResultHash & result,
        const std::unordered_map<Hash, Hash> & responseFor,
        const std::unordered_set<Hash> & allRequests,
        const SetHash & sessionRequestsRsHash,
        SetHash startFactSetHash);

    /* Navigate from (Q, ∅) using `dispatch` to evaluate Requests
       the recorded path needs. Returns the Result hash on hit,
       nullopt on miss. The dispatch callback is invoked for each
       Request whose Response isn't already in the accumulated
       FactSet (in this Phase 1 cut, that's every Request along
       the path). `onEdgeAttempt` (if set) is called once per
       Asks-edge attempt, AFTER the edge's useful requests have
       been dispatched, with `committed=true` if the resulting
       factset reaches a continuation and `false` if history is
       rejecting this branch. Used by the subject-id-aware caller to
       promote per-edge dispatched facts into a `ObservationSet`
       on commit (= principle 5/7) or discard them on reject. */
    /* Hit: returns (resultHash, terminalCur). The terminalCur is the
       factSet the history landed on when committing the terminal — child
       Q lookups use it as their candidate startCur, so a child's history
       starts from its parent's structural anchor. */
    struct WalkHit { ResultHash resultHash; SetHash terminalCur; };
    /* ObservationSet context passed to dispatch: identifies the specific Asks
       edge whose requests are being dispatched. */
    struct EdgeContext
    {
        QueryHash selectorHash;
        SetHash fromFactSetHash;
        SetHash requestSetHash;
    };

    std::optional<WalkHit> walk(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &, const EdgeContext &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt = {},
        /* Starting cur for the history. Defaults to ∅. Callers that
           have a structural anchor (= parent TracingReplayObject's terminalCur) can
           hand it in so the history starts at that lookup position. */
        const SetHash & startCur = SetHash(HashAlgorithm::SHA256),
        /* Task #110 Q-evolution: after each Ask edge commits (folding
           its request/response into cur), the walker calls this hook
           with the pre-fold Q, and it returns the post-fold Q. If Q
           evolved (the caller's fromSubject state hash changed under
           the walker's local envWalk), subsequent getTerminal/getAsks
           lookups use the new Q. Default is identity (no evolution). */
        const std::function<QueryHash(const QueryHash & preFoldQ)> & recomputeQ = {});

    /* Backward-compat overload: dispatch takes only the request. Used
       by unit tests that don't need edge context. */
    std::optional<WalkHit> walk(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt = {},
        const SetHash & startCur = SetHash(HashAlgorithm::SHA256))
    {
        return walk(q,
            [&](const RequestHash & req, const EdgeContext &) { return dispatch(req); },
            onEdgeAttempt, startCur);
    }

    /* Persist one trie node by hash. Idempotent (INSERT OR IGNORE +
       in-process cache short-circuit). Used by TrieBuilder to push
       its dirty nodes to storage. */
    void persistRequestSetNode(const Hash & nodeHash, std::string_view payload);

    /* ─────────────────────────────────────────────────────────────────
       Incremental RequestSet builder
       ─────────────────────────────────────────────────────────────────

       Maintains an in-memory hash-prefix trie that grows by one
       Request hash at a time. Used by TracingWriter to keep a
       running canonical RequestSet hash for the global envFactSet's
       requests without paying O(N) per logResult.

       Insertion is O(log N) amortised — one path-copy from leaf to
       root, with a split at the leaf if it would exceed
       TRIE_SPLIT_THRESHOLD. The root hash is computed lazily and
       cached; calling rootHash() after a mutation re-walks only the
       dirty path.

       persist(decisionGraph) flushes any unpersisted nodes into the
       RequestSetNodes pool. Subsequent reads of the same root hash
       via getRequestSet() will see the trie via the existing
       per-node payload cache. */
    class TrieBuilder
    {
    public:
        TrieBuilder();
        ~TrieBuilder();

        TrieBuilder(const TrieBuilder &) = delete;
        TrieBuilder & operator=(const TrieBuilder &) = delete;

        /* Insert one Request hash into the trie. Duplicate inserts
           are no-ops. */
        void insert(const Hash & request);

        /* Current root hash. Recomputes the dirty subtree if anything
           changed since the last call. */
        Hash rootHash();

        /* Push any unpersisted nodes into the RequestSetNodes pool.
           Idempotent. */
        void persist(TracingDecisionGraph & g);

        /* Forward-declared so static helpers in the .cc can name the
           type; defined in the .cc. */
        struct Node;

    private:
        std::unique_ptr<Node> root;
    };

    /* ─────────────────────────────────────────────────────────────────
       Maintenance
       ───────────────────────────────────────────────────────────────── */

    /* Block until all enqueued writes have been committed. */
    void waitForWrites();

private:
    struct State;
    std::unique_ptr<Sync<State>> _state;

    /* RequestSet trie internals — see schema comment and definitions
       in tracing-decision-graph.cc. */
    Hash insertTrieRecursive(std::vector<Hash> sortedMembers, int depth);
    bool collectTrieMembers(const Hash & nodeHash, std::vector<RequestHash> & out);
public:
    /* Public so `collectTrieMembers` and other in-file helpers can
       fetch stored node payloads. Not intended for general use. */
    std::optional<std::string> getRequestSetNodePayload(const Hash & nodeHash);
};

template<typename Q>
TracingDecisionGraph::QueryHash TracingDecisionGraph::computeSelectorHash(const Q & query)
{
    /* Serialise the query to JSON and SHA-256 it. The Query's
       "from" field carries the parent's selectorHash (Merkle identity),
       so the resulting hash encodes the full provenance chain. */
    nlohmann::json j = query;
    auto serialised = j.dump();
    return hashString(HashAlgorithm::SHA256, serialised);
}

} // namespace nix

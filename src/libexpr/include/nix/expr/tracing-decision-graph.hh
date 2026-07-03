#pragma once
/**
 * @file
 * SQLite-based decision-graph index for the tracing evaluation cache.
 *
 * Implements doc/design/tracing-eval-cache.md (v13 schema; see also
 * doc/outdated/tracing-decision-graph-data-model.md for the
 * superseded Phase-1 sketch). Two layers:
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
 * Phase 1 only: intersectionless recording and replay with Patricia
 * split. Phase 2 (passive-replay-before-insert, distance-to-any-R
 * heuristic) is a separate follow-up.
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
    static QueryHash computeQueryHash(const Q & query);

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
    void insertQuery(const QueryHash & h, std::string_view payload);
    void insertResult(const ResultHash & h, std::string_view payload);

    /* Atom payload lookup by hash. Returns nullopt if not present. */
    std::optional<std::string> getRequestPayload(const RequestHash & h);
    std::optional<std::string> getQueryPayload(const QueryHash & h);
    std::optional<std::string> getResultPayload(const ResultHash & h);

    /* LocalResponseMap: response payload pool, keyed by the *request*
       hash. d=2 REPLAY ONLY: `ReplayLocalObject` reads these back to
       serve probes into a reconstructed LocalObject value tree — this
       is the design's "d=2 walker is the only consumer" contract from
       `content-identity-via-asks.md` ("Atom storage"). The soundness
       argument requires the reqhash to be a pure function of
       (subject, scope, prior facts in the chain); across writer
       sessions with different outer args, that assumption can fail
       (same seed(N) pre-observation CDI → same reqhash → different
       responses from arg-dependent env), so d=1 dispatch MUST NOT
       read this map. See `cross-session-seed-collision` memory. */
    void insertLocalResponse(const RequestHash & requestHash, std::string_view payload);
    std::optional<std::string> getLocalResponsePayload(const RequestHash & requestHash);

    /* Edge-context-keyed response storage. Cold records
       (queryHash, fromFactSetHash, requestHash) → payload for each
       fact in each Q's per-edge requestSet.

       NOT a d=1 dispatch source. Same rule as LocalResponseMap: d=1
       walker live-dispatches; serving from storage bypasses the
       structural-validation contract even when the per-edge triple
       is more specific than LRM's reqhash-only key. See
       `cross-session-seed-collision` memory for why per-edge-context
       lookup isn't a safe workaround — if walker's live navigation
       fails, that is the walker's miss signal, not a licence to
       consult storage.

       Kept as an offline-inspection / diagnostic surface. If a
       future use lands, name it explicitly and document why the
       lookup is safe at that layer. */
    void insertEdgeResponse(
        const QueryHash & queryHash,
        const SetHash & fromFactSetHash,
        const RequestHash & requestHash,
        std::string_view payload);
    std::optional<std::string> getEdgeResponsePayload(
        const QueryHash & queryHash,
        const SetHash & fromFactSetHash,
        const RequestHash & requestHash);

    /* Subject-CDI stamp index: cold writes (cidHash, Q, edgeIndex, scope);
       walker looks up (Q, K) directly for a target idStr + scope. */
    void insertSubjectStampSite(
        const Hash & cidHash, const QueryHash & queryHash, size_t edgeIndex,
        const Hash & scope, const Hash & subjectHash);
    std::optional<std::pair<QueryHash, size_t>> getSubjectStampSite(
        const Hash & cidHash, const Hash & scope, const Hash & subjectHash);

    /* True if cold stamped any (scope, subjectHash) row for this cidHash.
       Used by walker's LRM substitution as a "recorded CDI" gate. */
    bool hasSubjectStampSite(const Hash & cidHash);

    /* Apply-result producer index: cold records `applyResultCid → (fnId, argId)`
       so warm walker can route apply-result CDIs through resolveApplyId
       (proper per-apply live reconstruction) instead of cell iteration
       (which conflates distinct apply-result CDIs sharing cell[0]). */
    void insertApplyResultProducer(
        const Hash & cidHash, const Hash & fnIdHash, const Hash & argIdHash);
    std::optional<std::pair<Hash, Hash>> getApplyResultProducer(const Hash & cidHash);

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
    void primeFactSetCache(const SetHash & hash, const std::vector<Fact> & members);

    /* XOR-fold one Fact into a running set hash. Used by callers that
       maintain their factSet hash incrementally to avoid the O(N)
       cost of insertFactSet. */
    static Hash xorFactIntoHash(const Hash & h, const Hash & request, const Hash & response);

    /* XOR two set-hashes. Used by cidasks::scopeStateIdAt and similar
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
        const std::unordered_set<RequestHash> & curRequests);

    /* ─────────────────────────────────────────────────────────────────
       Decision graph layer: edges keyed by (Q, factSet)
       ───────────────────────────────────────────────────────────────── */

    /* Insert an Asks edge: at (Q, factSet), the box's next set of
       Requests is `requestSet`. Idempotent on
       (queryHash, factSetHash, requestSetHash). */
    void insertAsks(const QueryHash & q, const SetHash & factSet, const SetHash & requestSet);

    /* Look up the outgoing RequestSet edges at (Q, factSet). */
    std::vector<SetHash> getAsks(const QueryHash & q, const SetHash & factSet);

    /** Enumerate ALL Asks rows for a given queryHash across all
        fromFactSetHashes. Used by `TracingWriter::logResult` to
        populate `EdgeResponses` for edges that `record()` inserted
        via Patricia split beyond the writer's own `perQAsksEdges`.
        Returns pairs of (fromFactSetHash, requestSetHash). */
    std::vector<std::pair<SetHash, SetHash>> getAllAsksForQ(const QueryHash & q);

    /* Remove a specific Asks edge. Used by Patricia split to
       re-point an existing edge. */
    void removeAsks(const QueryHash & q, const SetHash & factSet, const SetHash & requestSet);

    /* Insert a Terminal: at (Q, factSet), the recorded Result for
       Q is `result`. Idempotent on
       (queryHash, factSetHash, resultHash). */
    void insertTerminal(const QueryHash & q, const SetHash & factSet, const ResultHash & result);

    /* Look up the Terminal at (Q, factSet). Returns the Result hash
       if a Terminal exists. (Phase 1: only one Terminal per
       position is expected; nondeterminism handling is deferred.) */
    std::optional<ResultHash> getTerminal(const QueryHash & q, const SetHash & factSet);

    /* Cheap existence check: does any Asks or Terminal row exist at
       (Q, factSet)? Used by walk() to validate that a candidate
       FactSet hash lies on some recording for this query, without
       persisting FactSet members. */
    bool hasAnyEdge(const QueryHash & q, const SetHash & factSet);

    /* ─────────────────────────────────────────────────────────────────
       Decision graph layer: depth-2 (cb-apply boundary)

       The depth-2 trie is keyed on factSet alone (no Q), per
       doc/design/tracing-eval-cache-content-identity-via-asks.md.
       It records the observations the outer makes against an
       inner-supplied LocalObject during a covariant callback.
       Same-call sibling collapse within is intentional;
       cross-call disambiguation is via scope state id inheritance.

       Unlike depth-1, edges store toFactSetHash explicitly: at
       depth-2 there is no live producer for incoming-ambient
       observations at replay, so the walker can't reproduce the
       transition by live dispatch.
       ───────────────────────────────────────────────────────────────── */

    /* Insert a depth-2 Asks edge: at `fromFactSet`, dispatching
       `requestSet` and observing the recorded responses lands at
       `toFactSet`. Idempotent on (fromFactSet, requestSet). */
    void insertAmbientAsks(const SetHash & fromFactSet, const SetHash & requestSet, const SetHash & toFactSet);

    /* Look up depth-2 outgoing edges at `fromFactSet`. Returns a
       list of (requestSetHash, toFactSetHash) pairs. */
    std::vector<std::pair<SetHash, SetHash>> getAmbientAsks(const SetHash & fromFactSet);

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
       above. */
    void record(
        const QueryHash & q,
        const SetHash & factSet,
        const ResultHash & result,
        const std::unordered_map<Hash, Hash> & responseFor,
        const std::unordered_set<Hash> & allRequests);

    /* Fastest overload: caller also supplies the canonical RequestSet
       hash for the *current* allRequests (e.g. from an incremental
       TrieBuilder). When record() falls through to the whole-remaining
       insert at cur=∅, it uses this precomputed hash directly and
       avoids re-running insertRequestSet over allRequests. */
    void record(
        const QueryHash & q,
        const SetHash & factSet,
        const ResultHash & result,
        const std::unordered_map<Hash, Hash> & responseFor,
        const std::unordered_set<Hash> & allRequests,
        const SetHash & allRequestsRsHash);

    /* Navigate from (Q, ∅) using `dispatch` to evaluate Requests
       the recorded path needs. Returns the Result hash on hit,
       nullopt on miss. The dispatch callback is invoked for each
       Request whose Response isn't already in the accumulated
       FactSet (in this Phase 1 cut, that's every Request along
       the path). `onEdgeAttempt` (if set) is called once per
       Asks-edge attempt, AFTER the edge's useful requests have
       been dispatched, with `committed=true` if the resulting
       factset reaches a continuation and `false` if walk is
       rejecting this branch. Used by the cidasks-aware caller to
       promote per-edge dispatched facts into a `cidasks::Edge`
       on commit (= principle 5/7) or discard them on reject. */
    /* Hit: returns (resultHash, terminalCur). The terminalCur is the
       factSet the walk landed on when committing the terminal — child
       Q lookups use it as their candidate startCur, so a child's walk
       starts from its parent's structural anchor. */
    struct WalkHit { ResultHash resultHash; SetHash terminalCur; };
    /* Edge context passed to dispatch: identifies the specific Asks
       edge whose requests are being dispatched. Walker uses this to
       key EdgeResponses lookups so it can distinguish cross-sibling
       contamination (walker's context corresponds to a Q whose
       cumulative factSet inherited a fact from a prior sibling)
       from outer-value change (live truly differs from what cold
       recorded at this edge). */
    struct EdgeContext
    {
        QueryHash queryHash;
        SetHash fromFactSetHash;
        SetHash requestSetHash;
    };

    std::optional<WalkHit> walk(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &, const EdgeContext &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt = {},
        /* Starting cur for the walk. Defaults to ∅. Callers that
           have a structural anchor (= parent TracingReplayObject's terminalCur) can
           hand it in so the walk starts at that lookup position. */
        const SetHash & startCur = SetHash(HashAlgorithm::SHA256),
        const std::unordered_set<RequestHash> & startCurRequests = {});

    /* Full-featured walk with `reverseOutgoing` control — reversing
       tries alternate rs orderings at each cur, useful as a
       backtracking probe when the default order leads to a dead end. */
    std::optional<WalkHit> walkImpl(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &, const EdgeContext &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt,
        const SetHash & startCur,
        const std::unordered_set<RequestHash> & startCurRequests,
        bool reverseOutgoing);

    /* Backward-compat overload: dispatch takes only the request. Used
       by unit tests that don't need edge context. */
    std::optional<WalkHit> walk(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt = {},
        const SetHash & startCur = SetHash(HashAlgorithm::SHA256),
        const std::unordered_set<RequestHash> & startCurRequests = {})
    {
        return walk(q,
            [&](const RequestHash & req, const EdgeContext &) { return dispatch(req); },
            onEdgeAttempt, startCur, startCurRequests);
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
       running canonical RequestSet hash for the global v13FactSet's
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

        /* Compute the symmetric difference between this in-memory
           trie and a stored trie rooted at `otherRoot`. Appends
           elements only in this to `onlyInThis`, elements only in
           the stored trie to `onlyInOther`. Skips subtrees whose
           root hashes already match — for `dispatched ⊆ recordedRS`
           this collapses to O(|delta| · branching_factor) work
           regardless of how big either side is. */
        void diff(
            TracingDecisionGraph & g,
            const Hash & otherRoot,
            std::vector<Hash> & onlyInThis,
            std::vector<Hash> & onlyInOther);

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
    /* Public so TrieBuilder::diff (defined in the .cc) can fetch
       stored node payloads. Not intended for general use. */
    std::optional<std::string> getRequestSetNodePayload(const Hash & nodeHash);
};

template<typename Q>
TracingDecisionGraph::QueryHash TracingDecisionGraph::computeQueryHash(const Q & query)
{
    /* Serialise the query to JSON and SHA-256 it. The Query's
       "from" field carries the parent's queryHash (Merkle identity),
       so the resulting hash encodes the full provenance chain. */
    nlohmann::json j = query;
    auto serialised = j.dump();
    return hashString(HashAlgorithm::SHA256, serialised);
}

} // namespace nix

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

#include "nix/expr/trace-types.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"
#include "nix/util/sync.hh"

namespace nix::trace::rst { class FrozenNode; using FrozenNodePtr = nix::ref<const FrozenNode>; }

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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
    using RequestHash = TracingHash;  // d>0 query atom
    using ResponseHash = TracingHash; // d>0 response atom
    using QueryHash = TracingHash;    // d=0 query atom (operation + params + inputHashes)
    using ResultHash = TracingHash;   // d=0 result atom
    using SetHash = TracingHash;      // RequestSet or FactSet content hash

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
    static TracingHash computeResponseHash(const std::string & payload);

    /* Open or create a decision-graph index at the default location.
       Default: ~/.cache/nix/eval-tracing-decision-graph/index.sqlite
       NIX_TRACING_CACHE_DIR overrides the default for hermetic tests. */
    TracingDecisionGraph();

    /* Open or create an index at the specified path. */
    explicit TracingDecisionGraph(const std::filesystem::path & dbPath);

    ~TracingDecisionGraph();

    TracingDecisionGraph(const TracingDecisionGraph &) = delete;
    TracingDecisionGraph & operator=(const TracingDecisionGraph &) = delete;

    /** Session-scoped SelectorPool. Interns recursive Selector heap
        nodes so identical Selectors share a single ref<const Selector>
        across the process. Populated as writers construct producer
        Selectors and as walkers reconstruct them from DB rows. */
    trace::SelectorPool selectorPool;

    /* ─────────────────────────────────────────────────────────────────
       Storage layer: atomic content-addressed pools
       ───────────────────────────────────────────────────────────────── */

    /* Insert an atom into its content-addressed pool. The hash is
       computed by the caller (SHA-256 of the payload). Idempotent:
       INSERT OR IGNORE on the hash. */
    void insertRequest(const RequestHash & h, std::string_view payload);
    void insertSelector(const QueryHash & h, std::string_view payload);
    void insertResult(const ResultHash & h, std::string_view payload);

    /* Typed atom lookup by hash. Returns the parsed variant; the raw
       CBOR bytes stay behind an internal cache and never surface to
       callers. The typed form is cached per-hash so repeat lookups
       skip both the SQLite round-trip and the CBOR parse. Selectors
       have their own typed accessor via `selectorPool.find`. */
    std::optional<trace::Request> getRequest(const RequestHash & h);
    std::optional<trace::ResultVariant> getResult(const ResultHash & h);

    /* True if the request is a cb-apply Selector. Consumed only by
       walk()'s apply-bypass fallback pass, which is a candidate for
       removal once the walker no longer needs to guess sibling
       cb-apply positions. If that path goes, this method goes with it. */
    bool isApplyRequest(const RequestHash & h);

    /* Raw-payload accessors — used only by the typed accessors above
       and by SelectorPool. Callers outside those paths should use the
       typed forms. */
    std::optional<std::string> getRequestPayload(const RequestHash & h);
    std::optional<std::string> getSelectorPayload(const QueryHash & h);
    std::optional<std::string> getResultPayload(const ResultHash & h);

    /* ObservationSet CAS pool. An observation set is the set of
       InlineFacts the outer's callback probed on an inner-supplied
       contra-arg during one callback firing. The set as a whole
       carries the scope (one callback firing); individual members
       are Facts without in-object attribution. Response is stored
       inline so the walker reconstructs arg responses directly from
       this CAS.

       Referenced by `SelectorCallbackApply.argObsSet` — different
       observation sets give different queryHashes → distinct DB
       rows.

       Members hash: SHA-256 of the sorted-by-reqHash CBOR of the
       member list. Idempotent via INSERT OR IGNORE. */
    struct InlineFact
    {
        TracingHash reqHash = trace::tracingZeroHash();
        /* CBOR bytes of the observed response (a
           `trace::ResultVariant`). Used by the walker at replay to
           serve callback probes via an obsSet-answering proxy. */
        std::string responsePayload;

        bool operator==(const InlineFact & o) const
        {
            return reqHash == o.reqHash && responsePayload == o.responsePayload;
        }

        bool operator<(const InlineFact & o) const
        {
            if (reqHash != o.reqHash)
                return reqHash < o.reqHash;
            return responsePayload < o.responsePayload;
        }
    };
    TracingHash insertObservationSet(std::vector<InlineFact> members);
    std::optional<std::vector<InlineFact>> getObservationSet(const TracingHash & h);


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

    /* Persist a FrozenNodePtr (assumed to be interned in this DG's
       requestSetTrieCache — use internRequestSet or a rst set-op
       result). Idempotent on the node's identity hash. */
    SetHash insertRequestSet(trace::rst::FrozenNodePtr node);

    /* Intern a vector of member hashes into the shared trie cache and
       return the resulting FrozenNodePtr. Present for callers that
       intrinsically produce a vector (canonical substitution loop,
       Fact-list extraction, test literals). Callers that want to
       persist should chain `insertRequestSet(internRequestSet(vec))`.
       Callers with a MutableNode should freeze via
       state->requestSetTrieCache directly (see extendRequestSet). */
    trace::rst::FrozenNodePtr internRequestSet(std::vector<RequestHash> members);

    /* Look up an interned rs by its identity hash without building
       anything. Since identity = XOR of members, callers who can
       compute the XOR streaming (without materialising the member
       vector) can try this first — under matching-until-divergence,
       many rs identities are reused across chains, and a hit skips
       the vector build + internSet entirely. */
    std::optional<trace::rst::FrozenNodePtr> tryFindRequestSet(const TracingHash & identity);

    /* Insert `sortedMembers` (sorted+deduped) into `node`, producing a
       new FrozenNodePtr with `persisted=false` on newly-created nodes.
       Direct on the immutable structure — no MutableNode intermediate,
       no freeze step. Callers that want to persist should chain
       `insertRequestSet(insertSortedMembers(node, sortedMembers))`. */
    trace::rst::FrozenNodePtr insertSortedMembers(
        const trace::rst::FrozenNodePtr & node,
        std::span<const RequestHash> sortedMembers);

    SetHash insertFactSet(std::vector<Fact> members);

    /* Per-Fact element hash: BLAKE3(request || response). Callers with
       a fact in hand precompute this once (e.g. ArgCell::FactEntry) so
       downstream XOR-folds skip the re-hash. */
    static TracingHash factElementHash(const TracingHash & request, const TracingHash & response);

    /* XOR-fold one Fact into a running set hash. Used by callers that
       maintain their factSet hash incrementally to avoid the O(N)
       cost of insertFactSet. Prefer .xorInPlace(factElementHash(...))
       when the elementHash is already known (cached on FactEntry). */
    static TracingHash xorFactIntoHash(const TracingHash & h, const TracingHash & request, const TracingHash & response);

    static TracingHash xorHashes(const TracingHash & a, const TracingHash & b);

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

    /* Read a RequestSet as an interned FrozenNodePtr. Loads the trie
       top-down (children before parents) so future intersection/
       difference/isSubset calls against related sets can short-circuit
       at shared subtrees. Returns nullopt if the root hash isn't in
       the pool. */
    std::optional<trace::rst::FrozenNodePtr> getRequestSetNode(const SetHash & h);

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

    std::optional<WalkHit> walk(
        const QueryHash & q,
        const std::function<ResponseHash(const RequestHash &)> & dispatch,
        const std::function<void(bool committed, const std::vector<RequestHash> &)> & onEdgeAttempt = {},
        /* Starting cur for the history. Defaults to ∅. Callers that
           have a structural anchor (= parent TracingReplayObject's terminalCur) can
           hand it in so the history starts at that lookup position. */
        const SetHash & startCur = trace::tracingZeroHash());

    /* Persist one trie node by hash. Idempotent (INSERT OR IGNORE +
       in-process cache short-circuit). Called from the request-set
       trie's persist walk (see request-set-trie.hh). */
    void persistRequestSetNode(const TracingHash & nodeHash, std::string_view payload);

    /* ─────────────────────────────────────────────────────────────────
       Maintenance
       ───────────────────────────────────────────────────────────────── */

    /* Block until all enqueued writes have been committed. */
    void waitForWrites();

private:
    struct State;
    std::unique_ptr<Sync<State>> _state;

public:
    /* Fetch a raw stored RequestSet node payload by its hash. Callers
       are typically the rst layer's DB loader; not intended for
       general use. */
    std::optional<std::string> getRequestSetNodePayload(const TracingHash & nodeHash);
};

template<typename Q>
TracingDecisionGraph::QueryHash TracingDecisionGraph::computeSelectorHash(const Q & query)
{
    /* Must agree with the free `nix::computeSelectorHash(SelectorNode)`
       in trace-types.cc — both produce the identity used to look up
       Selectors, so any divergence in encoding OR truncation makes
       lookups miss. See trace-types.cc for rationale. */
    nlohmann::json j = query;
    auto cbor = nlohmann::json::to_cbor(j);
    return trace::tracingHash(
        std::string_view(reinterpret_cast<const char *>(cbor.data()), cbor.size()));
}

} // namespace nix

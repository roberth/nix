#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * v13 decision-graph index.
 */

#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/ref.hh"

#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

namespace nix {

class Object;

/**
 * Serialize a JSON value to CBOR as a std::string (for v13 payload storage).
 */
inline std::string jsonToCborString(const nlohmann::json & j)
{
    auto cbor = nlohmann::json::to_cbor(j);
    return std::string(reinterpret_cast<const char *>(cbor.data()), cbor.size());
}

/**
 * Parse a CBOR blob (stored as std::string) back to JSON.
 */
inline nlohmann::json cborStringToJson(const std::string & s)
{
    auto bytes = reinterpret_cast<const uint8_t *>(s.data());
    return nlohmann::json::from_cbor(bytes, bytes + s.size());
}

/**
 * A handle identifying a recorded d=0 Result, kept for back-compat with
 * a hex string of the queryHash that produced it (used by child queries
 * to compute their own queryHash with Merkle provenance).
 */
struct TriePosition
{
    Hash resultNodeHash;          // v13 ResultHash for this result
    std::string queryHashStr;     // hex of the queryHash that produced it
    /* Walker-side: the cur the v13Walk landed on when committing
       this terminal. Used by child Q lookups as a candidate startCur
       (= structurally-anchored lookup position) so a child walk
       starts from its parent's reached factSet rather than from
       session-leaky lastQFactsHash. Empty hash on TracingReplayObjects synthesized
       outside the walker (recording side). */
    Hash factSetHash{HashAlgorithm::SHA256};
};

/**
 * Trace writer: logs evaluation events to a JSON sink and records
 * them in the v13 decision graph.
 */
class TracingWriter
{
    TraceSink & sink;
    /* v13 decision-graph index. nullptr disables decision-graph
       recording (sink-only mode). */
    TracingDecisionGraph * decisionGraph;
    /* v13 global factSet, accumulating monotonically across the
       session per the design doc. Sampled at each logResult and
       fed into decisionGraph->record(). Only d>0 (Request, Response)
       Facts are added; d=0 Q→R pairs are not (the walk dispatch
       can't fetch them).

       The factSet hash is maintained incrementally via XOR-fold on
       each new Fact, and a seenRequests set dedupes per request.
       This makes the per-logResult cost O(1) instead of O(|factSet|)
       for the hash computation: insertFactSet (which would re-sort
       and re-fold all members) is bypassed via primeFactSetCache. */
    std::vector<TracingDecisionGraph::Fact> v13FactSet;
    TracingDecisionGraph::SetHash v13FactSetHash;
    std::unordered_set<Hash> seenRequests;
    /* request → response lookup, maintained as facts arrive.
       Handed to record() by reference so it doesn't rebuild
       O(N) per call. */
    std::unordered_map<Hash, Hash> responseFor;
    /* Incremental trie of allRequests; gives record() the canonical
       RequestSet hash for the whole-remaining edge in O(1). */
    TracingDecisionGraph::TrieBuilder allRequestsTrie;

    /* Ambient facts buffered during recording and flushed at
       logResult time via flushPendingAmbient. The Subject identifies
       which value the observation is about — flush uses it via
       cidasks::scopeStateIdAt to compute the fact's `from` field
       against the relevant Asks-edge precondition factset.

       Layer marker: depth-1 facts (inner asks outer about an outer
       value) feed into the depth-1 v13FactSet. Depth-2 facts (outer
       probes an inner-supplied LocalObject during a cb apply) group
       by their `applyId` (= the cb apply's resultId) into a depth-2
       Asks-edge in `AmbientAsks`, per the via-Asks design. */
    struct PendingFact
    {
        trace::QueryVariant query;
        trace::ResultVariant result;
        cidasks::Subject subject;
        Hash inheritedScope; ///< outer-scope argStateIds for scopeStateIdAt
        /* Empty hash = depth-1; otherwise = the cb apply's resultId,
           grouping this fact into the depth-2 sub-trace for that apply. */
        Hash depth2ApplyId{HashAlgorithm::SHA256};
    };
    /* Depth-1 facts (= ambient observations on outer state). Drained
       at every intermediate splitFlush and at finalize. */
    std::vector<PendingFact> pendingDepth1Facts;
    /* Depth-2 facts live on their owning PendingApplyBoundary so
       each cb-apply invocation's chain is built from exactly its
       own probe sequence. Storage is below (= PendingApplyBoundary's
       facts field). */

    /* Persistent cidasks chain for depth-1 ambient observations.
       d1CidasksWalk is kept 1:1-aligned with `perQAsksEdges`:
       every Asks edge inserted into `perQAsksEdges` is paired with
       a d1 edge inserted at the SAME index. This invariant lets the
       walker's `cidasksWalk` — which grows once per dispatched Asks
       edge via `commitEdge` — match the writer's d1 walk
       edge-for-edge, so `scopeStateIdAt(subject, scope, walk, K)`
       computes the same value on both sides. Per-arg-completion
       option 2 depends on this alignment. */
    std::vector<cidasks::Edge> d1CidasksWalk;
    /* Stages the next d1 edge between `flushPendingAmbient` (which
       drains pendingDepth1Facts into it) and `splitFlush` (which
       pushes it to d1CidasksWalk paired with a perQAsksEdge). May
       be empty (= file-read-only Asks edge) — still pushed so that
       d1CidasksWalk.size() == perQAsksEdges.size() always holds. */
    cidasks::Edge pendingD1Edge;

    /* Per-Q boundary tracking. `pendingNewRequests` accumulates every
       new query hash added to v13FactSet since the last logResult,
       whether from `logResponse` (= env/file), `noteEnvObservation`,
       or `flushPendingAmbient`. AmbientQueries are depth-1 just like
       file reads; bundling them with env/file into one Asks edge per
       logResult keeps the trie's edge structure 1:1 with d1CidasksWalk.
       `perQAsksEdges` retains each finalized boundary so every Q's
       logResult can pre-insert all of them in its namespace via
       INSERT OR IGNORE (= idempotent). */
    std::vector<Hash> pendingNewRequests;
    TracingDecisionGraph::SetHash prevQFactSetHash{TracingDecisionGraph::emptySetHash()};
    struct PerQAsksEdge
    {
        TracingDecisionGraph::SetHash fromFactSetHash;
        TracingDecisionGraph::SetHash requestSetHash;
    };
    std::vector<PerQAsksEdge> perQAsksEdges;
    /* Mirrors `seenRequests` but keyed by query hash, not fact hash.
       record()'s slow path iterates this to build the trailing
       remaining-edge — an Asks edge's requestSet is a set of query
       hashes, not fact hashes. */
    std::unordered_set<Hash> allRequestHashes;

    struct PendingRequest
    {
        nlohmann::json payload;
        std::optional<std::string> keyPlaceholder;
    };
    std::vector<PendingRequest> pendingRequests;

    /* Deferred cb-apply boundaries. markApplyBoundary pushes a new
       entry with empty facts; logDepth2Observation appends probes to
       the most recently-pushed boundary whose applyId matches.
       flushPendingAmbient processes each boundary's d=2 chain (=
       just its own facts), computes the terminal cumulative
       factSet as AmbientResult, and synthesises the d=1 apply Fact
       at `(applyReqHash, AmbientResult)`. Each cb-apply invocation
       owns exactly its own probe sequence. Recording order = vector
       order. */
    struct PendingApplyBoundary
    {
        Hash applyId;            ///< depth2ApplyId for the d=2 group
        Hash applyRequestHash;   ///< natural hash of applyQueryPayload
        std::vector<PendingFact> facts;
        /* Chronological insertion: ε perQAsksEdge for this boundary
           is inserted into perQAsksEdges at this position at finalize
           time (= position recorded at markApplyBoundary time, AFTER
           splitFlush(false) drained pre-boundary d=1 chunk). This
           makes the walker dispatch the ε edge BEFORE the body's
           d=1 facts that follow, so the lambda-standin's primop
           fires and seedCell extension happens in time for seed(N+1)
           probes to resolve. */
        size_t insertionIndex;
        /* prevQFactSetHash AT markApplyBoundary time = cur the
           walker would have at the start of ε's dispatch BEFORE
           any prior ε's contributions. After each ε insertion at
           finalize, this gets XOR-propagated by prior ε's element
           hashes. */
        Hash fromFactSetHashAtBoundary;
        /* Option (b) — late d2 obs support. Once a boundary's first
           finalize pass runs, it stays in `pendingApplyBoundaries`
           with `finalized=true` so a later `logDepth2Observation`
           with the same applyId can find it and process the probe
           incrementally instead of dropping it. State preserved
           across re-processings:
            - `cumulativeFactSet` = current d=2 chain terminal (=
              AmbientResult so far).
            - `factHash` = current SHA-256(applyReqHash || cumulativeFactSet),
              i.e. the synthetic d=1 apply Fact's element hash. On
              each re-process, recomputed; the delta between old and
              new is XOR-applied to v13FactSetHash and downstream
              perQAsksEdges' fromFactSetHash to keep the writer
              state consistent with the extended chain.
            - `pos` = the actual perQAsksEdges position where this
              boundary's ε edge ended up after insertion (=
              `insertionIndex + shift` at finalize time). Needed
              because subsequent boundaries' insertions don't shift
              this entry, but the in-memory shift counter is local
              to the finalize loop. */
        bool finalized = false;
        Hash cumulativeFactSet{HashAlgorithm::SHA256};
        Hash factHash{HashAlgorithm::SHA256};
        size_t pos = 0;
        /* Facts up to (but not including) this index have been
           processed in a previous finalize pass — their Request /
           LocalResponse / AmbientAsks entries are already in the
           DB. Re-entrant finalize passes only need to insert the
           tail `facts[lastProcessedCount..]`. */
        size_t lastProcessedCount = 0;
    };
    std::vector<PendingApplyBoundary> pendingApplyBoundaries;

    /* RAII suppress counter for `markApplyBoundary` while > 0. Used to
       elide redundant boundary firings during walker re-dispatch of a
       recorded apply (= `dispatchApplyLive`): walker's
       `fnObj->queryApply(replayLocal)` re-routes through
       `AmbientObject::queryApply` → `applyFn` → `AmbientApply::run`,
       which would normally fire `markApplyBoundary` — but that path
       represents validation of an already-recorded apply event, not a
       NEW event. Letting it fire inflates `d1CidasksWalk` with ε edges
       per re-validation, breaking the walker's 1:1 alignment with
       cold's writer at warm. */
    size_t suppressApplyBoundary = 0;

public:
    /* RAII helper: scoped suppress of markApplyBoundary. */
    class SuppressApplyBoundary
    {
        TracingWriter & writer;
    public:
        explicit SuppressApplyBoundary(TracingWriter & w) : writer(w) { ++writer.suppressApplyBoundary; }
        ~SuppressApplyBoundary() { --writer.suppressApplyBoundary; }
        SuppressApplyBoundary(const SuppressApplyBoundary &) = delete;
        SuppressApplyBoundary & operator=(const SuppressApplyBoundary &) = delete;
    };
private:

    /* Q hashes that have been logResult'd in this writer's lifetime.
       Re-inserted under at late-d2-obs re-process time so the
       updated `perQAsksEdges` (with corrected downstream
       `fromFactSetHash`) lands as additional Asks rows under each
       prior Q — letting the walker's chain walk for those Q's use
       the post-re-open propagation. */
    std::unordered_set<Hash> recordedQHashes;

public:
    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
        , v13FactSetHash(TracingDecisionGraph::emptySetHash())
    {
    }

    /** Cumulative cidasks walk over depth-1 ambient observations.
        One edge per logResult-triggered flush. Exposed so writer-side
        apply-result wrappers (TracingObject with applyResultSubject)
        can compute `scopeStateIdAt(subject, scope, walk, walk.size())`
        — the per-arg evolved scopeStateId the design's principle #3 requires
        for child queries on those wrappers. Walker's parallel handle
        is TracingReplayEvaluator::getCidasksWalk. */
    const std::vector<cidasks::Edge> & getD1CidasksWalk() const
    {
        return d1CidasksWalk;
    }

    /** Cumulative factSet hash maintained per-fact via XOR-fold.
        At cold time, advances at `noteEnvObservation` (= walker
        dispatches), `logResponse` (= env/file recordings), and
        `flushPendingAmbient` (= inner's ambient observations).
        At warm time, advances only at `noteEnvObservation` —
        which captures every dispatched fact, mirroring cold's
        cumulative. The walker reads this as the ground-truth
        cur for the cascading Terminal lookup (= when fast-path's
        per-edge math doesn't reach the recorded position because
        the Q has multiple terminals at curs that depend on prior
        sibling-style divergence). */
    const TracingDecisionGraph::SetHash & getV13FactSetHash() const
    {
        return v13FactSetHash;
    }

    /**
     * Opaque handle linking a query to its result.
     */
    struct QueryHandle
    {
        std::optional<Hash> queryHash;
    };

    /**
     * Log a root query (evalFile, evalExpr, apply).
     * Returns (valueHandle, queryHandle) so the caller can pass queryHandle to logResult.
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logRootQuery(const Q & query)
    {
        auto valueNum = sink.logQuery(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto queryHash = TracingDecisionGraph::computeQueryHash(query);
        return {valueNum, {queryHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.).
     * The query's `from` field must contain the parent's queryHash
     * (Merkle identity).
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logQuery(const Q & query, const std::optional<TriePosition> & /*parent*/)
    {
        auto valueNum = sink.logQuery(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto queryHash = TracingDecisionGraph::computeQueryHash(query);
        return {valueNum, {queryHash}};
    }

    /**
     * Log a response (file read, env lookup, etc.) — a d>0
     * Request/Response pair. Appended to v13 factSet for the next
     * Result's recording, and the Request/Response payloads land
     * in v13's atomic pools.
     */
    template<typename Req>
    void logResponse(const trace::Response<Req> & resp)
    {
        sink.log(nlohmann::json(resp));
        if (!decisionGraph)
            return;
        nlohmann::json reqJson = resp.request;
        nlohmann::json respJson = resp.response;
        auto queryHash = TracingDecisionGraph::computeQueryHash(resp.request);
        auto responsePayload = jsonToCborString(respJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        decisionGraph->insertRequest(queryHash, jsonToCborString(reqJson));
        if (storeAllResponsePayloads)
            decisionGraph->insertLocalResponse(queryHash, responsePayload);
        /* Dedupe by (request, response) pair, not request alone.
           Idempotent observations (same request, same response —
           e.g. file reads, env reads) collapse to one entry; sibling
           cb applies (same request, different responses) keep both
           contributions so v13FactSetHash reflects both elementHashes
           and the trie's per-(Q, factSet) terminals don't collide. */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({queryHash, responseHash});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, queryHash, responseHash);
            responseFor.emplace(queryHash, responseHash);
            allRequestsTrie.insert(queryHash);
            if (allRequestHashes.insert(queryHash).second)
                pendingNewRequests.push_back(queryHash);
        }
    }

    /**
     * Log an ambient interaction as a d>0 Request/Response pair.
     *
     * Under Phase 4 of content-defined identity, ambient facts are
     * buffered here rather than eagerly inserted into v13FactSet and
     * the Requests pool / LocalResponseMap — the `from` field of the query
     * may be a placeholder (counter-derived local id) whose final
     * Buffered until flushPendingAmbient() at logResult time
     * inserts into the pool at the query payload's natural reqHash. */
    void logAmbientInteraction(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        cidasks::Subject subject,
        Hash inheritedScope = Hash(HashAlgorithm::SHA256))
    {
        if (!decisionGraph)
            return;
        pendingDepth1Facts.push_back({query, result, std::move(subject), std::move(inheritedScope),
            /*depth2ApplyId=*/ Hash(HashAlgorithm::SHA256)});
    }

    /**
     * Log a depth-2 observation (= the outer probes an inner-supplied
     * LocalObject during a cb apply). Same payload shape as the
     * depth-1 path; the additional `applyId` (= the cb apply's
     * resultId) groups this fact into a depth-2 sub-trace at flush.
     */
    void logDepth2Observation(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        cidasks::Subject subject,
        Hash inheritedScope,
        Hash applyId)
    {
        if (!decisionGraph)
            return;
        /* Append to the most recently pushed boundary whose applyId
           matches — that's the cb-apply invocation currently
           building its probe sequence. Each invocation's probes
           land in its own facts vector, no cross-invocation mixing.

           Option (b) — late d2 obs: the boundary may already be
           `finalized=true` (e.g. cb-sibling's `{f,x}: f x` doesn't
           force its local during the body, so probes only fire
           when the outer subsequently accesses `.whatever` on the
           apply-result — by then the boundary's first finalize
           pass has already run). Boundaries are no longer cleared
           after finalize; this search still finds them, and the
           next `flushPendingAmbient(true)` pass picks up the new
           facts via `lastProcessedCount` and processes them
           incrementally. */
        for (auto it = pendingApplyBoundaries.rbegin();
             it != pendingApplyBoundaries.rend(); ++it) {
            if (it->applyId == applyId) {
                it->facts.push_back({query, result, std::move(subject),
                    std::move(inheritedScope), applyId});
                if (it->finalized)
                    tracingCacheLog(
                        "logDepth2Observation: late probe queued for finalized applyId=%s (now %zu facts, %zu processed)",
                        applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                        it->facts.size(), it->lastProcessedCount);
                return;
            }
        }
        /* No matching boundary at all — true invariant violation. */
        tracingCacheLog(
            "logDepth2Observation: no matching boundary for applyId=%s",
            applyId.to_string(HashFormat::Base16, false).substr(0, 12));
    }

    /**
     * Note an environment observation made by the walker during a
     * cache hit's dispatch. The walker calls dispatch live to verify
     * that recorded paths still hold against the current environment;
     * each `(request, response)` it computes is a real observation of
     * the environment, just like one made via `logResponse` or
     * `logAmbientInteraction` during interpretation. Feeding it back
     * into `v13FactSet` keeps the writer's cumulative state invariant
     * to whether facts came via interpretation or cache-hit dispatch.
     * Without this, a subsequent `logResult` for some Q that fell
     * back to inner would record at a factSetHash missing the
     * walker's prior dispatches — creating a sibling Asks chain and
     * disqualifying single-edge fast paths on future warms.
     */
    void noteEnvObservation(const Hash & request, const Hash & response)
    {
        if (!decisionGraph)
            return;
        /* Same dedupe semantics as logResponse — see commentary
           there. */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), request, response);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({request, response});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, request, response);
            responseFor.emplace(request, response);
            allRequestsTrie.insert(request);
            if (allRequestHashes.insert(request).second)
                pendingNewRequests.push_back(request);
        }
    }

    /**
     * Defer a Requests-pool insert until logResult.
     *
     * AmbientResolver::apply uses this to register the QueryApply
     * Request and the localArg sidecar. At flush: if `keyPlaceholder`
     * is set the insert key is that key (the local's scope state id);
     * otherwise the insert key is the hash of the payload (the apply
     * Q's own queryHash).
     */
    void deferRequest(nlohmann::json payload, std::optional<std::string> keyPlaceholder = std::nullopt)
    {
        if (!decisionGraph)
            return;
        pendingRequests.push_back({std::move(payload), std::move(keyPlaceholder)});
    }

    /**
     * Flush buffered ambient facts and Requests into the pool at
     * their natural reqHashes.
     *
     * Called from `splitFlush` (= every cb-apply boundary and at
     * logResult). With `finalize=false` (= intermediate flushes),
     * only depth-1 facts are drained; depth-2 facts and buffered
     * `pendingApplyBoundaries` stay buffered for later. With
     * `finalize=true` (= logResult), pendingApplyBoundaries are
     * also processed: for each, the d=2 chain group is built,
     * its terminal `cumulativeFactSet` is the AmbientResult, and
     * the d=1 synthetic apply Fact `(applyReqHash, AmbientResult)`
     * is folded into v13FactSet / d1CidasksWalk / pendingNewRequests
     * just like an ordinary depth-1 ambient observation.
     */
    void flushPendingAmbient(bool finalize = false);

    /**
     * End the current Asks edge at a cb-apply boundary inside a
     * body run. Processes pending observations (advancing
     * d1CidasksWalk by one edge if any ambient observations are
     * pending), finalises the perQAsksEdge boundary, and resets
     * pendingNewRequests so the next observation set starts a
     * fresh edge.
     *
     * Required at every cb-apply boundary the writer crosses
     * during a body run — TracingEvaluator::apply,
     * TracingObject::queryApply, AmbientResolver::apply. Without
     * this split, multiple body-level cb-applies collapse into a
     * single Asks edge in the recorded trie, but the walker
     * advances its cumulative `cidasksWalk` once per dispatched
     * Asks edge (= principle 6) — leaving writer and walker at
     * different walk indices when they each compute the
     * apply-result's argStateId, producing different queryHashes.
     *
     * Skip-on-empty per the principle 4 + 7 read: an Asks edge
     * with no ambient observations doesn't move cidasks state, so
     * walker's commitEdge is a no-op for it. Same on the writer.
     */
    void splitFlush(bool finalize = false);

    /**
     * Mark a cb-apply boundary in the recording. Closes the
     * preceding observations into their own Asks edge (= β1 via
     * splitFlush), inserts the apply Request payload into the CAS
     * pool, and buffers a `PendingApplyBoundary` recording the
     * applyId and reqHash.
     *
     * The d=1 apply Fact itself is *not* folded into v13FactSet
     * here. Its response hash is the AmbientResult (= terminal of
     * the d=2 chain captured for this applyId), which is only known
     * at flushPendingAmbient time. Deferring synthesis keeps the
     * d=1 cur consistent with via-Asks §"Recording (depth-2)":
     * "The terminal factSet hash *is* the `AmbientResult`, which
     * the depth-1 walker XOR-folds into its own `cur` as the
     * `Response` for the enclosing `AmbientQuery`."
     *
     * The `fromHash` of the synthetic d=1 apply Fact's
     * d1CidasksWalk observation is `Hash(0)` — the apply boundary
     * is a walk-advance marker, not a fact about any subject, so
     * it doesn't fold into any subject's own-loop.
     */
    void markApplyBoundary(const nlohmann::json & applyQueryPayload);

    /**
     * Log a nested cb-apply as a depth-2 fact under the enclosing
     * cb-apply's chain. Used by TracingEvaluator::apply when the
     * fn is a TracingLocalObject (= inner-supplied lambda being
     * applied by the outer). Per via-Asks Replay (depth-2): the
     * lambda primop at warm pulls this edge by (chainCursor,
     * stampedReqHash). Walker-side counterpart in
     * `<replay-local-lambda>` impl advances the standin's
     * chainCursor by this fact's elementHash.
     *
     * Subject = ApplyResultSubject{fn, arg} (caller-built) so the
     * generic flushPendingAmbient stamping puts the constituents'
     * roots into `fromCIDs[]` and an Apply step into `path`. Matches
     * walker stamping. No-op when there's no enclosing cb-apply.
     */
    /**
     * Return the `applyId` of the cb-apply boundary currently on top
     * of `pendingApplyBoundaries`. Used by `IT::apply` when fn is a
     * TracingLocalObject (= the recursive cb-apply path) to capture
     * the enclosing boundary's id before the recursive call would
     * otherwise push a new boundary; the captured id then flows to
     * the `LambdaApplyResultObject` wrapping the apply result, so
     * its observations land in the same boundary's d=2 chain as the
     * recursive apply Fact `logDepth2ApplyFact` appended.
     */
    std::optional<Hash> getCurrentApplyBoundaryId() const
    {
        if (pendingApplyBoundaries.empty())
            return std::nullopt;
        return pendingApplyBoundaries.back().applyId;
    }

    void logDepth2ApplyFact(
        const nlohmann::json & applyQueryPayload,
        const cidasks::Subject & resultSubject,
        const Hash & applyScope)
    {
        if (!decisionGraph)
            return;
        if (pendingApplyBoundaries.empty())
            return;
        auto & enclosing = pendingApplyBoundaries.back();
        trace::QueryApply applyQ{
            applyQueryPayload["params"]["fn"].get<std::string>(),
            applyQueryPayload["params"]["arg"].get<std::string>(),
        };
        enclosing.facts.push_back({
            trace::QueryVariant{applyQ},
            trace::ResultVariant{trace::ResultType{"apply"}},
            resultSubject,
            applyScope,
            enclosing.applyId,
        });
    }

    /**
     * When true, every file-read / env-var response payload gets
     * persisted into the decisionGraph's LocalResponseMap too —
     * useful for offline debugging when JSON traces aren't
     * available. Default false: walker never reads depth-1 payloads
     * from there (= live-dispatches against the env instead), so
     * the storage is pure overhead unless someone's grepping the
     * DB by hand.
     */
    bool storeAllResponsePayloads = false;

    /**
     * Log a d=0 Result. Records (Q, current factSet) -> Result in
     * the v13 decision graph and returns a TriePosition for use by
     * child queries.
     */
    template<typename R>
    std::optional<TriePosition> logResult(ValueHandle valueNum, const R & result, const QueryHandle & qh)
    {
        sink.logResult(valueNum, result);

        if (!decisionGraph || !qh.queryHash)
            return std::nullopt;

        /* Process any pending ambient observations, finalise
           buffered cb-apply boundaries (computing each one's
           AmbientResult from its d=2 chain and folding the
           synthetic d=1 apply Fact in), and close the trailing
           Asks edge boundary. splitFlush is also called at every
           cb-apply boundary inside a body run, but with
           finalize=false; the d=2-driven AmbientResult computation
           happens only here at logResult, since intermediate
           splitFlushes can be interleaved with the apply's body
           and the d=2 chain may not be complete yet. */
        splitFlush(/*finalize=*/ true);

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        /* v13FactSetHash is maintained incrementally per fact; skip
           insertFactSet's O(N log N) sort + fold. primeFactSetCache
           makes the members available to record() via getFactSet
           without rebuilding the hash. responseFor + seenRequests
           are passed by reference so record() doesn't re-build its
           per-call lookup map and remaining set.

           allRequestsTrie is maintained incrementally per fact and
           gives us the canonical RequestSet root hash for the
           current allRequests in O(1). Persist any unwritten nodes
           and hand the root hash to record() as the precomputed RS
           hash for the whole-remaining edge — record() can then
           skip its insertRequestSet(remainingVec) call. */
        decisionGraph->primeFactSetCache(v13FactSetHash, v13FactSet);
        allRequestsTrie.persist(*decisionGraph);

        tracingCacheLog("logResult: Q=%s factSet=%s -> result (inserting %zu Asks edges)",
                        qh.queryHash->to_string(HashFormat::Base16, false).substr(0, 12),
                        v13FactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        perQAsksEdges.size());
        for (const auto & edge : perQAsksEdges)
            decisionGraph->insertAsks(*qh.queryHash, edge.fromFactSetHash, edge.requestSetHash);

        /* If we have per-Q edges, skip the whole-remaining shortcut
           so the walker walks them one by one (= each commit advances
           ctx.edgeIndex). Pass `allRequestHashes` (= query hashes),
           not `seenRequests` (= fact hashes for XOR dedup); record()'s
           slow path iterates this for its trailing remaining-edge. */
        if (perQAsksEdges.empty())
            decisionGraph->record(*qh.queryHash, v13FactSetHash, resultNodeHash,
                responseFor, seenRequests, allRequestsTrie.rootHash());
        else
            decisionGraph->record(*qh.queryHash, v13FactSetHash, resultNodeHash,
                responseFor, allRequestHashes);

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.queryHash->to_string(HashFormat::Base16, false),
        };
    }

    /**
     * Get underlying TraceSink for compatibility.
     */
    TraceSink & getSink()
    {
        return sink;
    }

    /**
     * used to advance the temporal cursor after a hit. v13 has no
     * temporal cursor; this is a no-op.
     */
    void syncAfterHash(const Hash & /*resultNodeHash*/)
    {
        // No-op under v13.
    }

    /**
     * Whether v13 decision-graph recording is enabled.
     */
    bool hasIndex() const
    {
        return decisionGraph != nullptr;
    }

    TracingDecisionGraph * getDecisionGraph() const
    {
        return decisionGraph;
    }
};

} // namespace nix

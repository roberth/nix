#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * decision-graph index.
 */

#include "nix/expr/subject-id.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/util/ref.hh"

#include <algorithm>
#include <map>
#include <optional>
#include <unordered_set>
#include <vector>

namespace nix {

class Object;

/**
 * Serialize a JSON value to CBOR as a std::string (for payload storage).
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
    Hash resultNodeHash;          // ResultHash for this result
    std::string queryHashStr;     // hex of the queryHash that produced it
    /* Walker-side: the cur the history landed on when committing
       this terminal. Used by child Q lookups as a candidate startCur
       (= structurally-anchored lookup position) so a child history
       starts from its parent's reached factSet rather than from
       session-leaky envCur. Empty hash on TracingReplayObjects synthesized
       outside the walker (recording side). */
    Hash factSetHash{HashAlgorithm::SHA256};
};

/**
 * Trace writer: logs evaluation events to a JSON sink and records
 * them in the decision graph.
 */
class TracingWriter
{
    TraceSink & sink;
    /* decision-graph index. nullptr disables decision-graph
       recording (sink-only mode). */
    TracingDecisionGraph * decisionGraph;
    /* global factSet, accumulating monotonically across the
       session per the design doc. Sampled at each logResult and
       fed into decisionGraph->record(). Only d>0 (Request, Response)
       Facts are added; d=0 Q→R pairs are not (the history dispatch
       can't fetch them).

       The factSet hash is maintained incrementally via XOR-fold on
       each new Fact, and a seenRequests set dedupes per request.
       This makes the per-logResult cost O(1) instead of O(|factSet|)
       for the hash computation: insertFactSet (which would re-sort
       and re-fold all members) is bypassed via installFactSet. */
    std::vector<TracingDecisionGraph::Fact> envFactSet;
    TracingDecisionGraph::SetHash envFactSetHash;
    std::unordered_set<Hash> seenRequests;
    /* request → response lookup, maintained as facts arrive.
       Handed to record() by reference so it doesn't rebuild
       O(N) per call. */
    std::unordered_map<Hash, Hash> responseFor;
    /* Incremental trie of allRequests; gives record() the canonical
       RequestSet hash for the whole-remaining edge in O(1). */
    TracingDecisionGraph::TrieBuilder sessionRequestsTrie;

    /* Ambient facts buffered during recording and flushed at
       logResult time via flushAmbient. The Subject identifies
       which value the observation is about — flush uses it via
       stateHashAt to compute the fact's `from` field
       against the relevant Asks-edge precondition factset.

       Layer marker: env layer facts (inner asks outer about an outer
       value) feed into the env layer envFactSet. Depth-2 facts (outer
       probes an inner-supplied LocalObject during a cb apply) group
       by their `applyId` (= the cb apply's resultId) into a ambient layer
       Asks-edge in `AmbientAsks`, per the via-Asks design. */
    struct PendingFact
    {
        trace::QueryVariant query;
        trace::ResultVariant result;
        Subject subject;
        Hash argAncestry; ///< outer-argAncestry state hashes for stateHashAt
        /* Empty hash = env layer; otherwise = the cb apply's resultId,
           grouping this fact into the ambient layer sub-trace for that apply. */
        Hash ambientApplyId{HashAlgorithm::SHA256};
    };
    /* Depth-2 (Ambient) facts live on their owning PendingCbApply so
       each cb-apply invocation's chain is built from exactly its own
       probe sequence. Storage is below (= PendingCbApply's facts
       field). Depth-1 (env-layer outer-value probes) are stamped and
       pushed per-probe in `logOuterObservation`, not buffered here. */

    /* Persistent history chain for env-layer observations.
       envWalk is kept 1:1-aligned with `envAsksEdges`: every Asks
       edge inserted into `envAsksEdges` is paired with an
       ObservationSet at the SAME index. This invariant lets the
       walker's `envWalk` — which grows once per dispatched Asks
       edge via `commitEdge` — match the writer's history
       edge-for-edge, so `stateHashAt(subject, argAncestry, history, K)`
       computes the same value on both sides. */
    std::vector<ObservationSet> envWalk;

    /* Per-Q boundary tracking. `pendingNewRequests` accumulates every
       new query hash added to envFactSet since the last logResult,
       whether from `logResponse` (= env/file), `noteEnvObservation`,
       or `flushAmbient`. OuterQueries are env layer just like
       file reads; bundling them with env/file into one Asks edge per
       logResult keeps the trie's edge structure 1:1 with envWalk.
       `envAsksEdges` retains each finalized boundary so every Q's
       logResult can pre-insert all of them in its namespace via
       INSERT OR IGNORE (= idempotent). */
    std::vector<Hash> pendingNewRequests;

    TracingDecisionGraph::SetHash prevQFactSetHash{TracingDecisionGraph::emptySetHash()};

    /* Per-Q factSet baseline: writer's cumulative envFactSetHash at the
       start of the currently-active Q's evaluation. Q-local factSet at
       any moment = envFactSetHash XOR this. Stacked for nested Qs
       (logQuery pushes current envFactSetHash, logResult pops). This
       makes writer stamps (Ask fromFactSetHash, Terminal factSetHash,
       InnerValueResponse contextHash) align with walker's per-walk
       factSet: walker starts fresh at ∅ each walker.walk(); writer's
       per-Q baseline plays the analogous role on the record side. */
    std::vector<TracingDecisionGraph::SetHash> perQFactSetBaselineStack{
        TracingDecisionGraph::emptySetHash()};

    /** Per-Q factSet: writer's cumulative XOR'd against current Q's
        baseline. This is the value the walker would have as its
        per-walk factSet at the corresponding point under matching-
        until-divergence. */
    TracingDecisionGraph::SetHash perQFactSetHash() const
    {
        return TracingDecisionGraph::xorHashes(
            envFactSetHash, perQFactSetBaselineStack.back());
    }

    struct AsksEdgeRecord
    {
        TracingDecisionGraph::SetHash fromFactSetHash;
        TracingDecisionGraph::SetHash requestSetHash;
    };
    std::vector<AsksEdgeRecord> envAsksEdges;
    /* Q-local edge slice: envAsksEdges size at the start of the
       currently-active Q. Only edges appended after this index are
       Q-own; logResult inserts only those, with fromFactSetHash
       rebased against the Q's baseline. */
    std::vector<size_t> perQAsksEdgesStartStack{0};
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

    /* Deferred cb-apply boundaries. openCbApply pushes a new
       entry with empty facts; logAmbientObservation appends probes to
       the most recently-pushed boundary whose applyId matches.
       flushAmbient processes each boundary's ambient chain (=
       just its own facts), computes the terminal cumulative
       factSet as AmbientResult, and synthesises the env apply Fact
       at `(applyReqHash, AmbientResult)`. Each cb-apply invocation
       owns exactly its own probe sequence. Recording order = vector
       order. */
    struct PendingCbApply
    {
        Hash applyId;            ///< ambientApplyId for the ambient group
        Hash applyRequestHash;   ///< natural hash of applyQueryPayload
        std::vector<PendingFact> facts;
        /* Chronological insertion: ε perQAsksEdge for this boundary
           is inserted into envAsksEdges at this position at finalize
           time (= position recorded at openCbApply time, AFTER
           closeAsksEdge(false) drained pre-boundary env chunk). This
           makes the walker dispatch the ε edge BEFORE the body's
           env facts that follow, so the lambda-ReplayCallbackArg's primop
           fires and seedCell extension happens in time for arg(N+1)
           probes to resolve. */
        size_t insertionIndex;
        /* prevQFactSetHash AT openCbApply time = cur the
           walker would have at the start of ε's dispatch BEFORE
           any prior ε's contributions. After each ε insertion at
           finalize, this gets XOR-propagated by prior ε's element
           hashes. */
        Hash envCurAtOpen;
        /* The OUTER writer's env cur at the moment inner emitted
           this cb-apply Fact. Captured at openCbApply from
           `outerWriter->getV13FactSetHash()`. Under lockstep
           replay this is the value the outer walker sees as its
           own env cur when it dispatches this same cb-apply Fact,
           i.e. `walkerCur` at `dispatchApplyLive`. Used together
           with `envCurAtOpen` to compute the
           InnerValueResponse contextHash. */
        Hash outerEnvCurAtOpen;
        /* Walker's outer env cur at THIS cb-apply's dispatch
           moment (= envCurAtOpen XOR priorApplyFactAccum
           at first-finalize time). Stored for late-obs re-processing
           so re-emitted InnerValueResponse inserts use the same
           context as the first-finalize inserts. Zero (empty hash)
           until first finalize populates it. */
        Hash contextCur;
        /* Option (b) — late ambient obs support. Once a boundary's first
           finalize pass runs, it stays in `pendingCbApplies`
           with `finalized=true` so a later `logAmbientObservation`
           with the same applyId can find it and process the probe
           incrementally instead of dropping it. State preserved
           across re-processings:
            - `cumulativeFactSet` = current ambient chain terminal (=
              AmbientResult so far).
            - `factHash` = current SHA-256(applyReqHash || cumulativeFactSet),
              i.e. the synthetic env apply Fact's element hash. On
              each re-process, recomputed; the delta between old and
              new is XOR-applied to envFactSetHash and downstream
              envAsksEdges' fromFactSetHash to keep the writer
              state consistent with the extended chain.
            - `pos` = the actual envAsksEdges position where this
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
           InnerValueResponse / AmbientAsks entries are already in the
           DB. Re-entrant finalize passes only need to insert the
           tail `facts[lastProcessedCount..]`. */
        size_t lastProcessedCount = 0;
    };
    std::vector<PendingCbApply> pendingCbApplies;

    /* RAII suppress counter for `openCbApply` while > 0. Used to
       elide redundant boundary firings during walker re-dispatch of a
       recorded apply (= `dispatchApplyLive`): walker's
       `fnObj->queryApply(replayLocal)` re-routes through
       `OuterObject::queryApply` → `applyFn` → `OuterApply::run`,
       which would normally fire `openCbApply` — but that path
       represents validation of an already-recorded apply event, not a
       NEW event. Letting it fire inflates `envWalk` with ε edges
       per re-validation, breaking the walker's 1:1 alignment with
       cold's writer at warm. */
    size_t suppressCbApply = 0;

public:
    /* RAII helper: scoped suppress of openCbApply. */
    class SuppressApplyBoundary
    {
        TracingWriter & writer;
    public:
        explicit SuppressApplyBoundary(TracingWriter & w) : writer(w) { ++writer.suppressCbApply; }
        ~SuppressApplyBoundary() { --writer.suppressCbApply; }
        SuppressApplyBoundary(const SuppressApplyBoundary &) = delete;
        SuppressApplyBoundary & operator=(const SuppressApplyBoundary &) = delete;
    };
private:

    /* Q hashes that have been logResult'd in this writer's lifetime.
       Re-inserted under at late-d2-obs re-process time so the
       updated `envAsksEdges` (with corrected downstream
       `fromFactSetHash`) lands as additional Asks rows under each
       prior Q — letting the walker's chain history for those Q's use
       the post-re-open propagation. */
    std::unordered_set<Hash> recordedQHashes;

public:
    /* Subject-evolution stamp: insert one SubjectEvolutionEdges row. Called
       from cold's stateHashAtStamping hook callback at
       fact-`from` construction sites. Immediate write (not
       buffered) — Subject-evolution emissions per stateHashAt call are
       bounded by the history length × observations per edge and are
       infrequent enough that buffering isn't necessary. */
    void insertSubjectEvolutionEdge(
        const Hash & subjectHash, const Hash & curHash,
        const Hash & obsFromHash, const Hash & obsElementHash,
        const Hash & nextCurHash)
    {
        if (!decisionGraph)
            return;
        decisionGraph->insertSubjectEvolutionEdge(
            subjectHash, curHash, obsFromHash, obsElementHash, nextCurHash);
    }

    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
        , envFactSetHash(TracingDecisionGraph::emptySetHash())
    {
    }

    /** The outer evaluator's writer, if this writer is inside a
        nested `builtins.cache` call. Its `getV13FactSetHash()` is
        the value the walker sees as `walkerCur` when it dispatches
        this writer's recorded cb-apply Fact at replay under
        lockstep. Set by cache.cc from the outer's TracingReplayEvaluator.
        Null on the top-level (non-nested) writer. */
    TracingWriter * outerWriter = nullptr;

    /** Cumulative subject-id history over env layer ambient observations.
        One edge per logResult-triggered flush. Exposed so writer-side
        apply-result wrappers (TracingObject with applyResultSubject)
        can compute `stateHashAt(subject, argAncestry, history, history.size())`
        — the per-arg evolved state hash the design's principle #3 requires
        for child queries on those wrappers. Walker's parallel handle
        is TracingReplayEvaluator::getCidasksWalk. */
    const std::vector<ObservationSet> & getD1CidasksWalk() const
    {
        return envWalk;
    }

    /** Cumulative factSet hash maintained per-fact via XOR-fold.
        At cold time, advances at `noteEnvObservation` (= walker
        dispatches), `logResponse` (= env/file recordings), and
        `flushAmbient` (= inner's ambient observations).
        At warm time, advances only at `noteEnvObservation` —
        which captures every dispatched fact, mirroring cold's
        cumulative. The walker reads this as the ground-truth
        cur for the cascading Terminal lookup (= when fast-path's
        per-edge math doesn't reach the recorded position because
        the Q has multiple terminals at curs that depend on prior
        sibling-style divergence). */
    const TracingDecisionGraph::SetHash & getV13FactSetHash() const
    {
        return envFactSetHash;
    }

    /**
     * Opaque handle linking a query to its result.
     */
    struct QueryHandle
    {
        std::optional<Hash> queryHash;
        /* Parent Query's terminalCur, captured at logQuery time. Used
           at logResult as the explicit start point of this Q's Ask
           chain — the "structural parent factSet" the walker's
           parentAnchor path lands on. std::nullopt for root queries
           (no parent → chain starts at ∅). */
        std::optional<Hash> structuralParentFactSetHash;
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
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logRootQuery: Q=%s queryJSON=%s",
            queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        return {valueNum, {queryHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.).
     * The query's `from` field must contain the parent's queryHash
     * (Merkle identity).
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logQuery(const Q & query, const std::optional<TriePosition> & parent)
    {
        auto valueNum = sink.logQuery(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto queryHash = TracingDecisionGraph::computeQueryHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logQuery: Q=%s queryJSON=%s",
            queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        /* Push per-Q baselines so subsequent observations get stamped
           against Q-local factSet. Nested Qs (child logQuery inside
           parent's evaluation) get their own fresh baseline; parent's
           context is restored at child's logResult. */
        perQFactSetBaselineStack.push_back(envFactSetHash);
        perQAsksEdgesStartStack.push_back(envAsksEdges.size());
        QueryHandle qh{queryHash};
        if (parent)
            qh.structuralParentFactSetHash = parent->factSetHash;
        return {valueNum, qh};
    }

    /**
     * Log a response (file read, env lookup, etc.) — a d>0
     * Request/Response pair. Per-probe: each call pushes its own
     * single-request Ask + envWalk entry, matching the
     * logOuterObservation path and keeping prevQFactSetHash tightly
     * synchronised with envFactSetHash. Without per-probe pushing
     * here, prevQFactSetHash lags behind envFactSetHash whenever a
     * file/env read intervenes between two logOuterObservation
     * calls, and the latter's Ask row gets inserted at a stale cur
     * that the walker (with its up-to-date live cur) cannot reach.
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
            decisionGraph->insertInnerValueResponse(queryHash, Hash(HashAlgorithm::SHA256), responsePayload);
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        if (!seenRequests.insert(factHash).second)
            return;
        envFactSet.push_back({queryHash, responseHash});
        envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
            envFactSetHash, queryHash, responseHash);
        responseFor.emplace(queryHash, responseHash);
        sessionRequestsTrie.insert(queryHash);
        allRequestHashes.insert(queryHash);
        auto requestSetHash = decisionGraph->insertRequestSet({queryHash});
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        /* Push envWalk with the fact's observation (fromHash=0 since
           file/env reads don't attribute to any Subject). Walker's
           dispatch of this Ask returns the same responseHash live,
           folds elementHash into cur — walker's envWalk push would
           match on fingerprint if it happened. Since walker's
           commitEdge skips empty pushes (and file/env dispatch
           pushes no pendingEdgeObservations), walker's envWalk is
           shorter here; that's harmless because these observations
           have fromHash=0 and fold into no Subject's own-loop. */
        ObservationSet obsSet;
        obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        envWalk.push_back(std::move(obsSet));
        prevQFactSetHash = envFactSetHash;
    }

    /**
     * Log an env-layer outer-value probe (inner asks outer via
     * OuterObject). Stamped and pushed inline per-probe rather than
     * buffered: the design's chain says each response advances the
     * Subject's state hash, and the NEXT probe's `from` is that
     * evolved state hash (principle 5's post-substitution rule). If
     * we batched multiple probes into one flush, all their `from`
     * fields would share the pre-flush state and collapse to identical
     * requestHashes — collapsing sibling cb-apply invocations that
     * SHOULD end up at distinct trie positions. Per-probe stamping
     * yields the design's per-observation state evolution.
     *
     * Concurrency batching within a single Ask (P7's XOR
     * commutativity) is legitimate for Env probes that don't feed
     * each other's `from` fields (independent file reads via
     * logResponse), but not for outer-value probes whose
     * requestHashes depend on evolved Subject state — so those go
     * per-probe here. */
    void logOuterObservation(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        Subject subject,
        Hash argAncestry = Hash(HashAlgorithm::SHA256));

    /**
     * Log a ambient layer observation (= the outer probes an inner-supplied
     * LocalObject during a cb apply). Same payload shape as the
     * env layer path; the additional `applyId` (= the cb apply's
     * resultId) groups this fact into a ambient layer sub-trace at flush.
     */
    void logAmbientObservation(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        Subject subject,
        Hash argAncestry,
        Hash applyId)
    {
        if (!decisionGraph)
            return;
        /* Append to the most recently pushed boundary whose applyId
           matches — that's the cb-apply invocation currently
           building its probe sequence. Each invocation's probes
           land in its own facts vector, no cross-invocation mixing.

           Option (b) — late ambient obs: the boundary may already be
           `finalized=true` (e.g. cb-sibling's `{f,x}: f x` doesn't
           force its local during the body, so probes only fire
           when the outer subsequently accesses `.whatever` on the
           apply-result — by then the boundary's first finalize
           pass has already run). Boundaries are no longer cleared
           after finalize; this search still finds them, and the
           next `flushAmbient(true)` pass picks up the new
           facts via `lastProcessedCount` and processes them
           incrementally. */
        for (auto it = pendingCbApplies.rbegin();
             it != pendingCbApplies.rend(); ++it) {
            if (it->applyId == applyId) {
                it->facts.push_back({query, result, std::move(subject),
                    std::move(argAncestry), applyId});
                if (it->finalized)
                    tracingCacheLog(
                        "logAmbientObservation: late probe queued for finalized applyId=%s (now %zu facts, %zu processed)",
                        applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                        it->facts.size(), it->lastProcessedCount);
                return;
            }
        }
        /* No matching boundary at all — true invariant violation. */
        tracingCacheLog(
            "logAmbientObservation: no matching boundary for applyId=%s",
            applyId.to_string(HashFormat::Base16, false).substr(0, 12));
    }

    /**
     * Note an environment observation made by the walker during a
     * cache hit's dispatch. The walker calls dispatch live to verify
     * that recorded paths still hold against the current environment;
     * each `(request, response)` it computes is a real observation of
     * the environment, just like one made via `logResponse` or
     * `logOuterObservation` during interpretation. Feeding it back
     * into `envFactSet` keeps the writer's cumulative state invariant
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
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), request, response);
        if (!seenRequests.insert(factHash).second)
            return;
        responseFor.emplace(request, response);
        envFactSet.push_back({request, response});
        envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
            envFactSetHash, request, response);
        sessionRequestsTrie.insert(request);
        allRequestHashes.insert(request);
        /* Per-probe push (see logResponse for reasoning). */
        auto requestSetHash = decisionGraph->insertRequestSet({request});
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        ObservationSet obsSet;
        obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        envWalk.push_back(std::move(obsSet));
        prevQFactSetHash = envFactSetHash;
    }

    /**
     * Defer a Requests-pool insert until logResult.
     *
     * OuterResolver::apply uses this to register the QueryApply
     * Request and the localArg sidecar. At flush: if `keyPlaceholder`
     * is set the insert key is that key (the local's state hash);
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
     * Called from `closeAsksEdge` (= every cb-apply and at
     * logResult). With `finalize=false` (= intermediate flushes),
     * only env layer facts are drained; ambient layer facts and buffered
     * `pendingCbApplies` stay buffered for later. With
     * `processApplies=true` (= logResult), pendingCbApplies are
     * also processed: for each, the ambient chain group is built,
     * its terminal `cumulativeFactSet` is the AmbientResult, and
     * the env synthetic apply Fact `(applyReqHash, AmbientResult)`
     * is folded into envFactSet / envWalk / pendingNewRequests
     * just like an ordinary env layer ambient observation.
     */
    void flushAmbient(bool processApplies = false);

    /**
     * End the current Asks edge at a cb-apply inside a
     * body run. Processes pending observations (advancing
     * envWalk by one edge if any ambient observations are
     * pending), finalises the perQAsksEdge boundary, and resets
     * pendingNewRequests so the next observation set starts a
     * fresh edge.
     *
     * Required at every cb-apply the writer crosses
     * during a body run — TracingEvaluator::apply,
     * TracingObject::queryApply, OuterResolver::apply. Without
     * this split, multiple body-level cb-applies collapse into a
     * single Asks edge in the recorded trie, but the walker
     * advances its cumulative `envWalk` once per dispatched
     * Asks edge (= principle 6) — leaving writer and walker at
     * different history indices when they each compute the
     * apply-result's state hash, producing different queryHashes.
     *
     * Skip-on-empty per the principle 4 + 7 read: an Asks edge
     * with no ambient observations doesn't move subject-id state, so
     * walker's commitEdge is a no-op for it. Same on the writer.
     */
    void closeAsksEdge(bool processApplies = false);

    /**
     * Mark a cb-apply in the recording. Closes the
     * preceding observations into their own Asks edge (= β1 via
     * closeAsksEdge), inserts the apply Request payload into the CAS
     * pool, and buffers a `PendingCbApply` recording the
     * applyId and reqHash.
     *
     * The env apply Fact itself is *not* folded into envFactSet
     * here. Its response hash is the AmbientResult (= terminal of
     * the ambient chain captured for this applyId), which is only known
     * at flushAmbient time. Deferring synthesis keeps the
     * env cur consistent with via-Asks §"Recording (ambient layer)":
     * "The terminal factSet hash *is* the `AmbientResult`, which
     * the env layer walker XOR-folds into its own `cur` as the
     * `Response` for the enclosing `OuterQuery`."
     *
     * The `fromHash` of the synthetic env apply Fact's
     * envWalk observation is `Hash(0)` — the cb-apply
     * is a history-advance marker, not a fact about any subject, so
     * it doesn't fold into any subject's own-loop.
     */
    void openCbApply(const nlohmann::json & applyQueryPayload);

    /**
     * Log a nested cb-apply as a ambient layer fact under the enclosing
     * cb-apply's chain. Used by TracingEvaluator::apply when the
     * fn is a TracingCallbackArg (= inner-supplied lambda being
     * applied by the outer). Per via-Asks Replay (ambient layer): the
     * lambda primop at warm pulls this edge by (chainCursor,
     * stampedReqHash). Walker-side counterpart in
     * `<replay-local-lambda>` impl advances the ReplayCallbackArg's
     * chainCursor by this fact's elementHash.
     *
     * Subject = ApplyResultSubject{fn, arg} (caller-built) so the
     * generic flushAmbient stamping puts the constituents'
     * roots into `fromStateHashes[]` and an Apply step into `path`. Matches
     * walker stamping. No-op when there's no enclosing cb-apply.
     */
    /**
     * Return the `applyId` of the cb-apply currently on top
     * of `pendingCbApplies`. Used by `IT::apply` when fn is a
     * TracingCallbackArg (= the recursive cb-apply path) to capture
     * the enclosing boundary's id before the recursive call would
     * otherwise push a new boundary; the captured id then flows to
     * the `TracingCallbackApplyResult` wrapping the apply result, so
     * its observations land in the same boundary's ambient chain as the
     * recursive apply Fact `logAmbientApplyFact` appended.
     */
    std::optional<Hash> getCurrentCbApplyId() const
    {
        if (pendingCbApplies.empty())
            return std::nullopt;
        return pendingCbApplies.back().applyId;
    }

    void logAmbientApplyFact(
        const nlohmann::json & applyQueryPayload,
        const Subject & resultSubject,
        const Hash & applyArgAncestry)
    {
        if (!decisionGraph)
            return;
        if (pendingCbApplies.empty())
            return;
        auto & enclosing = pendingCbApplies.back();
        trace::QueryApply applyQ{
            applyQueryPayload["params"]["fn"].get<std::string>(),
            applyQueryPayload["params"]["arg"].get<std::string>(),
        };
        enclosing.facts.push_back({
            trace::QueryVariant{applyQ},
            trace::ResultVariant{trace::ResultType{"apply"}},
            resultSubject,
            applyArgAncestry,
            enclosing.applyId,
        });
    }

    /**
     * When true, every file-read / env-var response payload gets
     * persisted into the decisionGraph's InnerValueResponse too —
     * useful for offline debugging when JSON traces aren't
     * available. Default false: walker never reads env layer payloads
     * from there (= live-dispatches against the env instead), so
     * the storage is pure overhead unless someone's grepping the
     * DB by hand.
     */
    bool storeAllResponsePayloads = false;

    /**
     * Log a d=0 Result. Records (Q, current factSet) -> Result in
     * the decision graph and returns a TriePosition for use by
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
           AmbientResult from its ambient chain and folding the
           synthetic env apply Fact in), and close the trailing
           Asks edge boundary. closeAsksEdge is also called at every
           cb-apply inside a body run, but with
           finalize=false; the ambient-driven AmbientResult computation
           happens only here at logResult, since intermediate
           splitFlushes can be interleaved with the apply's body
           and the ambient chain may not be complete yet. */
        closeAsksEdge(/*processApplies=*/ true);

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        /* envFactSetHash is maintained incrementally per fact; skip
           insertFactSet's O(N log N) sort + fold. installFactSet
           makes the members available to record() via getFactSet
           without rebuilding the hash. responseFor + seenRequests
           are passed by reference so record() doesn't re-build its
           per-call lookup map and remaining set.

           sessionRequestsTrie is maintained incrementally per fact and
           gives us the canonical RequestSet root hash for the
           current allRequests in O(1). Persist any unwritten nodes
           and hand the root hash to record() as the precomputed RS
           hash for the whole-remaining edge — record() can then
           skip its insertRequestSet(remainingVec) call. */
        decisionGraph->installFactSet(envFactSetHash, envFactSet);
        sessionRequestsTrie.persist(*decisionGraph);

        /* Q-local views: baseline captured at logQuery, XOR-rebase to
           get Q-local factSet and Q-local edge fromFactSetHash. This
           is what the walker's per-walk factSet would be at the
           corresponding point on the read side. */
        auto qBaseline = perQFactSetBaselineStack.back();
        auto qEdgesStart = perQAsksEdgesStartStack.back();
        auto perQFactSet = TracingDecisionGraph::xorHashes(envFactSetHash, qBaseline);
        size_t qEdgeCount = envAsksEdges.size() - qEdgesStart;
        tracingCacheLog("logResult: Q=%s factSet=%s -> result (inserting %zu Asks edges)",
                        qh.queryHash->to_string(HashFormat::Base16, false).substr(0, 12),
                        perQFactSet.to_string(HashFormat::Base16, false).substr(0, 12),
                        qEdgeCount);
        for (size_t i = qEdgesStart; i < envAsksEdges.size(); ++i) {
            const auto & edge = envAsksEdges[i];
            auto perQFromFactSet = TracingDecisionGraph::xorHashes(edge.fromFactSetHash, qBaseline);
            decisionGraph->insertAsk(*qh.queryHash, perQFromFactSet, edge.requestSetHash);
        }

        /* Insert Terminal for Q at its per-Q terminal factSet. Ask
           edges already inserted above cover Q's own trajectory; we
           skip the record() call's Patricia-split / whole-remaining
           logic because it's session-cumulative in shape and would
           either fire spurious slow-path inserts (allRequestHashes is
           session-cumulative but Q owns only a subset) or insert an
           unhelpful whole-remaining Ask under Q. Q's Terminal is what
           the walker's terminal lookup keys on; the Ask chain path is
           already laid out via the per-Q edge inserts above. */
        decisionGraph->insertTerminal(*qh.queryHash, perQFactSet, resultNodeHash);

        /* Pop per-Q stacks: parent's context restored for any
           continued evaluation. */
        perQFactSetBaselineStack.pop_back();
        perQAsksEdgesStartStack.pop_back();

        /* Populate per-edge response table AFTER `record()` so
           Patricia-split-added Asks rows are covered too. Enumerate
           ALL Asks rows for Q (not just `envAsksEdges`), and use
           InnerValueResponse (`getInnerValueResponsePayload`) as the source of truth so
           coordinates whose reqhashes came from a prior sibling's
           dispatch (cumulative-dependency principle) also get
           covered. */
        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.queryHash->to_string(HashFormat::Base16, false),
            .factSetHash = perQFactSet,
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
     * used to advance the temporal cursor after a hit. currently has no
     * temporal cursor; this is a no-op.
     */
    void syncAfterHash(const Hash & /*resultNodeHash*/)
    {
        // No-op.
    }

    /**
     * Whether decision-graph recording is enabled.
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

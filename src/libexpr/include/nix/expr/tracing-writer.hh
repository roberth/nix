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
       or `flushPending`. OuterQueries are env layer just like
       file reads; bundling them with env/file into one Asks edge per
       logResult keeps the trie's edge structure 1:1 with envWalk.
       `envAsksEdges` retains each finalized boundary so every Q's
       logResult can pre-insert all of them in its namespace via
       INSERT OR IGNORE (= idempotent). */
    std::vector<Hash> pendingNewRequests;

    TracingDecisionGraph::SetHash prevQFactSetHash{TracingDecisionGraph::emptySetHash()};
    struct AsksEdgeRecord
    {
        TracingDecisionGraph::SetHash fromFactSetHash;
        TracingDecisionGraph::SetHash requestSetHash;
    };
    std::vector<AsksEdgeRecord> envAsksEdges;

    /** Per-active-query state. Each `logQuery` pushes; each `logResult`
        pops. LIFO nesting matches the evaluator's Q hierarchy (parent
        Q's evaluation triggers child Q's logQuery inside). Observations
        that fire while a Q is active are attributed to that Q's
        current `currentQ`. When an observation folds into `envWalk`
        and evolves the fromSubject's state hash, the writer re-derives
        `currentQ` (by updating the payload's `from` field and re-
        hashing). Subsequent Ask edges land at the new `currentQ`, and
        `logResult` inserts Terminal at the final `currentQ`. */
    struct ActiveQuery
    {
        /** Current Q hash, updated on each observation that evolves the
            fromSubject's state. */
        Hash currentQ{HashAlgorithm::SHA256};
        /** Q's serialisable payload. `from` gets rewritten as the
            fromSubject's state evolves; re-hashing gives `currentQ`. */
        nlohmann::json payloadTemplate;
        /** Subject that Q's `from` field's state hash is derived from.
            Not set for root queries or queries whose from is a fixed
            hash (state does not evolve for those). */
        std::optional<Subject> fromSubject;
        /** argAncestry for `stateHashAt(fromSubject, ..., envWalk, ...)`. */
        Hash fromSubjectArgAncestry{HashAlgorithm::SHA256};
        /** Cached fromSubject state hash at last recomputation; used to
            detect changes without re-hashing Q every observation. */
        Hash fromSubjectLastState{HashAlgorithm::SHA256};
        /** Parent Q's terminalCur, for the walker's structural anchor. */
        std::optional<TracingDecisionGraph::SetHash> structuralParentFactSetHash;
        /** Task #110: per-Q chain observation history. Each observation
            attributed to this Q (the innermost at that moment) appends
            here. Q's `from` is derived from stateHashAt(fromSubject,
            argAncestry, perQEnvWalk, perQEnvWalk.size()) — using THIS
            Q's own chain, not session-wide observations from other Qs.
            Preserves same-shape collapse: two same-shape Qs with the
            same fromSubject-initial-state evolve to the same finalQ
            because they see the same fold from their own chains. */
        std::vector<ObservationSet> perQEnvWalk;
        /** B2 composite emission: sub-Q's on-the-wire query tag,
            captured at logQuery for use at logResult when synthesising
            the composite request payload for the parent. */
        std::string queryTag;
        /** B10 landing-chain simulation: captured at push time,
            immutable through Q's evolution. Used at logResult to
            simulate walker's per-Q Q evolution through the pre-push
            session-cumulative Ask trail and insert Ask rows under each
            simulated (Q, cur) so the walker (starting from ∅) can fold
            its way to this Q's session-cumulative entry cur. */
        nlohmann::json initialPayloadTemplate;
        Hash initialFromSubjectState{HashAlgorithm::SHA256};
        size_t envAsksEdgesSizeAtPush{0};
    };
    std::vector<ActiveQuery> activeQueryStack;
    /* Mirrors `seenRequests` but keyed by query hash, not fact hash.
       record()'s slow path iterates this to build the trailing
       remaining-edge — an Asks edge's requestSet is a set of query
       hashes, not fact hashes. */
    std::unordered_set<Hash> allRequestHashes;

    std::vector<nlohmann::json> pendingRequests;

    /* Active cb-apply cells. `createCallbackCell` pushes a new cell
       at cache-boundary apply; `logCallbackObservation` appends each
       observation the outer makes on the arg to the cell's
       `runningObsSet`; at sampling moments the writer snapshots that
       set into the ObservationSet CAS and emits a QueryCallbackApply
       request referencing it. Cell lookup at sampling time is by
       `fnStateHashHex` — the fn's initial state hash captured at
       apply time. */
    struct CallbackCell
    {
        /* Identity of this callback firing; used by
           `logCallbackObservation` to route observations to the right
           cell. Equals the natural hash of the apply query payload. */
        Hash applyId{HashAlgorithm::SHA256};
        /* Fn's initial state hash (empty history). Cell lookup key
           in `logOuterObservation` — matches the ApplyResultSubject's
           fn state hash under matching-until-divergence. Captured
           at `createCallbackCell` from the applyQueryPayload's `fn`
           field. */
        std::string fnStateHashHex;
        /* Cached call's callArgAncestry, encoded into the
           CallbackApplyRef so the walker's ReplayCallbackArg
           reconstructs the arg's Subject at the same argAncestry —
           required for probe queryHashes to match cold's obsSet. */
        std::string argAncestryHex;
        /* Observations made on this cell's contra-arg so far.
           Snapshotted into the ObservationSet CAS at
           CallbackApplyRef stamping time. */
        std::vector<TracingDecisionGraph::Observation> runningObsSet;
    };
    std::vector<CallbackCell> callbackCells;

    /** Emit the single CallbackApply Fact for a pendingCbApply whose
        firing has just completed. Inserts the request into the pool,
        folds `(cbApplyQueryHash, cbApplyRespHash)` into envFactSet,
        pushes an Ask edge, and records an envWalk observation with
        fromHash = fn's subject state hash. Sets `it.emitted = true`.
        Idempotent: no-op if already emitted or if runningObsSet is
        empty. */

    /* RAII suppress counter for `createCallbackCell` while > 0. Used to
       elide redundant boundary firings during walker re-dispatch of a
       recorded apply (= `dispatchApplyLive`): walker's
       `fnObj->queryApply(replayLocal)` re-routes through
       `OuterObject::queryApply` → `applyFn` → `OuterApply::run`,
       which would normally fire `createCallbackCell` — but that path
       represents validation of an already-recorded apply event, not a
       NEW event. Letting it fire inflates `envWalk` with ε edges
       per re-validation, breaking the walker's 1:1 alignment with
       cold's writer at warm. */
    size_t suppressCbApply = 0;

public:
    /* RAII helper: scoped suppress of createCallbackCell. */
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
    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
        , envFactSetHash(TracingDecisionGraph::emptySetHash())
    {
    }

    /** Cumulative subject-id history over Env-layer observations.
        One edge per logResult-triggered flush. Exposed so writer-side
        apply-result wrappers (TracingObject with applyResultSubject)
        can compute `stateHashAt(subject, argAncestry, history, history.size())`
        — the per-arg evolved state hash Design principle #3 requires
        for child queries on those wrappers. Walker's parallel handle
        is TracingReplayEvaluator::getCidasksWalk. */
    const std::vector<ObservationSet> & getD1CidasksWalk() const
    {
        return envWalk;
    }

    /** Task #110 (C3): emit a QueryCallbackApply observation for an
        applyResult, carrying the applyResult's WHNF as the Result.
        Called from TracingObject::whnf() when it has an
        applyResultSubject. Looks up the matching CallbackCell by
        fn's initial state hash, snapshots the cell's runningObsSet
        into the ObservationSet CAS, constructs the QCA payload with
        fn's current state hash as `fn`, and emits via the standard
        logOuterObservation path. Under content addressing, same
        (fn, obsSet) pair produces the same QCA queryHash across
        callers — parent's chain sees a single QCA-per-firing. */
    void emitCallbackApplyForApplyResult(
        const Subject & applyResultSubject,
        Hash applyArgAncestry,
        const trace::ResultWHNF & whnf)
    {
        if (!decisionGraph)
            return;
        auto * ar = std::get_if<ApplyResultSubject>(&applyResultSubject.data);
        if (!ar || !ar->fn)
            return;
        auto fnInitial = stateHashAtSubject(*ar->fn, applyArgAncestry, {}, 0);
        auto fnInitialHex = fnInitial.to_string(HashFormat::Base16, false);
        for (auto it = callbackCells.rbegin(); it != callbackCells.rend(); ++it) {
            auto & cell = *it;
            if (cell.fnStateHashHex != fnInitialHex)
                continue;
            if (cell.argAncestryHex.empty())
                continue;
            auto obsSetHash = decisionGraph->insertObservationSet(cell.runningObsSet);
            auto fnCurrent = stateHashAtSubject(
                *ar->fn, applyArgAncestry, envWalk, envWalk.size());
            trace::QueryCallbackApply qca;
            qca.fn = trace::QueryLeaf{trace::StateHashLeaf{
                fnCurrent.to_string(HashFormat::Base16, false),
                cell.argAncestryHex}};
            qca.argObsSet = obsSetHash.to_string(HashFormat::Base16, false);
            logOuterObservation(
                trace::QueryVariant{std::move(qca)},
                trace::ResultVariant{whnf},
                *ar->fn,
                applyArgAncestry);
            return;
        }
    }

    /** Cumulative factSet hash maintained per-fact via XOR-fold.
        At cold time, advances at `noteEnvObservation` (= walker
        dispatches) and `logResponse` (= env/file recordings).
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
     * Log a root query (evalFile, evalExpr, apply). Root queries have
     * no evolving from-subject, so `activeQueryStack` records the Q
     * as fixed.
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
        ActiveQuery aq;
        aq.currentQ = queryHash;
        aq.payloadTemplate = qj;
        aq.queryTag = std::string(Q::tag);
        aq.initialPayloadTemplate = qj;
        aq.envAsksEdgesSizeAtPush = envAsksEdges.size();
        activeQueryStack.push_back(std::move(aq));
        return {valueNum, {queryHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.). If
     * `fromSubject` is provided, the writer will re-derive Q's `from`
     * field after each observation that evolves that subject's state
     * hash, and Ask/Terminal rows for this Q will be keyed on the
     * evolved Q at each step (Q evolution protocol).
     */
    template<typename Q>
    std::pair<ValueHandle, QueryHandle> logQuery(
        const Q & query,
        const std::optional<TriePosition> & parent,
        std::optional<Subject> fromSubject = std::nullopt,
        Hash fromSubjectArgAncestry = Hash(HashAlgorithm::SHA256))
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
        QueryHandle qh{queryHash};
        if (parent)
            qh.structuralParentFactSetHash = parent->factSetHash;
        Hash lastState(HashAlgorithm::SHA256);
        if (fromSubject) {
            lastState = stateHashAt(
                *fromSubject, fromSubjectArgAncestry, envWalk, envWalk.size());
        }
        ActiveQuery aq;
        aq.currentQ = queryHash;
        aq.payloadTemplate = qj;
        aq.fromSubject = std::move(fromSubject);
        aq.fromSubjectArgAncestry = fromSubjectArgAncestry;
        aq.fromSubjectLastState = lastState;
        aq.structuralParentFactSetHash = qh.structuralParentFactSetHash;
        aq.queryTag = std::string(Q::tag);
        aq.initialPayloadTemplate = qj;
        aq.initialFromSubjectState = lastState;
        aq.envAsksEdgesSizeAtPush = envAsksEdges.size();

        /* B11: preconditions. Under callback-model §3, Q's chain
           starts at index M > 0 carrying preconditions from prior
           state. Fold pre-push session observations into aq's per-Q
           chain, evolving aq.currentQ to Q_M and inserting Ask rows
           under each intermediate Q value so walkers starting at
           (Q_initial, ∅) can fold their way to (Q_M, Q_entry_cur).

           Under matching-until-divergence, walker's per-walk perQEnvWalk
           after committing the same N landing-chain obs contains the
           same ObservationSets → stateHashAt gives the same evolved
           state → walker's Q at end of landing == aq.currentQ (Q_M).
           Q's first own Ask (later, at logOuterObservation) is then
           recorded under aq.currentQ = Q_M, which walker finds. */
        if (aq.fromSubject) {
            for (size_t i = 0; i < envAsksEdges.size(); ++i) {
                const auto & edge = envAsksEdges[i];
                decisionGraph->insertAsk(aq.currentQ, edge.fromFactSetHash, edge.requestSetHash);
                if (i < envWalk.size()) {
                    aq.perQEnvWalk.push_back(envWalk[i]);
                    auto newState = stateHashAt(
                        *aq.fromSubject, aq.fromSubjectArgAncestry,
                        aq.perQEnvWalk, aq.perQEnvWalk.size());
                    if (newState != aq.fromSubjectLastState) {
                        aq.fromSubjectLastState = newState;
                        auto newFromHex = newState.to_string(HashFormat::Base16, false);
                        if (aq.payloadTemplate.contains("params")
                            && aq.payloadTemplate["params"].is_object()) {
                            auto & p = aq.payloadTemplate["params"];
                            if (p.contains("from"))
                                p["from"] = newFromHex;
                            if (p.contains("fromStateHashes")
                                && p["fromStateHashes"].is_array()
                                && !p["fromStateHashes"].empty())
                                p["fromStateHashes"][0] = newFromHex;
                        }
                        aq.currentQ = hashString(HashAlgorithm::SHA256, aq.payloadTemplate.dump());
                    }
                }
            }
            if (envAsksEdges.size() > 0)
                tracingCacheLog("logQuery: precondition fold %zu obs -> Q_M=%s",
                                envAsksEdges.size(),
                                aq.currentQ.to_string(HashFormat::Base16, false).substr(0, 12));
        }

        activeQueryStack.push_back(std::move(aq));
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
        /* Task #110 (correct model): attribute to the innermost active
           Q only (see logOuterObservation). */
        if (!activeQueryStack.empty()) {
            auto & innermost = activeQueryStack.back();
            decisionGraph->insertAsk(innermost.currentQ, prevQFactSetHash, requestSetHash);
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        /* File/env reads have fromHash=0 and thus don't evolve any
           subject's state hash — no Q evolution triggered here. */
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
     * B2 composite sub-Q emission. Records a single fact against the
     * innermost active Q — the parent, since this is called from
     * logResult AFTER the sub-Q's frame has been popped — using the
     * completed sub-Q's finalQ payload verbatim (no re-stamping) as
     * the request and its resultHash as the response.
     *
     * fromHash on the pushed observation is zero: stateHashAt's
     * `obs.fromHash == subject.stateHash` filter never matches any
     * real Subject, so the composite folds into parent's cur / env
     * but doesn't perturb any Subject's own-loop. Same-shape sub-Qs
     * produce identical (request, response) pairs and dedup via
     * seenRequests.
     */
    void logCompositeSubQ(
        const nlohmann::json & subQueryPayload,
        Hash subQueryHash,
        const nlohmann::json & subResultPayload,
        Hash subResultHash);

    /**
     * Record one observation the outer made on a callback firing's
     * contra-arg. Routes to the matching CallbackCell by `applyId`
     * and pushes an `{queryHash, responsePayload}` entry into that
     * cell's `runningObsSet`. `logOuterObservation` later snapshots
     * that set into the ObservationSet CAS when it stamps a
     * CallbackApplyRef into an outer probe reaching this apply's
     * result.
     *
     * The contra-arg's `Subject` has no state-hash evolution — its
     * structural id `SHA("positional-<depth>") XOR argAncestry` is
     * constant — so `from` is stamped against an empty history.
     * Sibling calls with different callback bodies discriminate
     * downstream via the obsSet content-hash on their enclosing
     * CallbackApply, not via arg-side state hash evolution.
     */
    void logCallbackObservation(
        const trace::QueryVariant & query,
        const trace::ResultVariant & result,
        Subject subject,
        Hash argAncestry,
        Hash applyId)
    {
        if (!decisionGraph)
            return;
        /* Most recent matching cell = the cb-apply invocation
           currently building its probe sequence. */
        for (auto it = callbackCells.rbegin();
             it != callbackCells.rend(); ++it) {
            if (it->applyId != applyId)
                continue;
            /* Capture argAncestry on first observation. The walker
               rebuilds the ReplayCallbackArg's Subject depth from
               fn's argCell chain at dispatch time (under the shared-
               computation invariant), so no depth capture here. */
            if (it->argAncestryHex.empty())
                it->argAncestryHex = argAncestry.to_string(HashFormat::Base16, false);
            if (it->fnStateHashHex.empty())
                return;
            trace::QueryVariant stampedQuery = query;
            auto par = pathAndRootsFromSubject(subject);
            std::vector<trace::QueryLeaf> fromStateHashes;
            fromStateHashes.reserve(par.roots.size());
            for (auto & root : par.roots) {
                auto cid = stateHashAfter(root, argAncestry, {});
                fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::visit([&](auto & q) {
                using QT = std::decay_t<decltype(q)>;
                if constexpr (requires { q.from; }) {
                    q.from = fromStateHashes.empty()
                        ? trace::QueryLeaf{std::string{}}
                        : fromStateHashes[0];
                    q.path = par.path;
                    q.fromStateHashes = fromStateHashes;
                }
            }, stampedQuery);
            auto qh = std::visit(
                [](const auto & q) {
                    return TracingDecisionGraph::computeQueryHash(q);
                }, stampedQuery);
            nlohmann::json rJson = std::visit(
                [](const auto & r) -> nlohmann::json { return r; },
                result);
            auto rPayload = jsonToCborString(rJson);
            it->runningObsSet.push_back({qh, rPayload});
            return;
        }
        tracingCacheLog(
            "logCallbackObservation: no matching cell for applyId=%s",
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
        /* Per-probe push (see logResponse for reasoning). Task #110
           (correct model): attribute to the innermost active Q only. */
        auto requestSetHash = decisionGraph->insertRequestSet({request});
        if (!activeQueryStack.empty()) {
            auto & innermost = activeQueryStack.back();
            decisionGraph->insertAsk(innermost.currentQ, prevQFactSetHash, requestSetHash);
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        ObservationSet obsSet;
        obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        envWalk.push_back(std::move(obsSet));
        prevQFactSetHash = envFactSetHash;
    }

    /**
     * Defer a Requests-pool insert until logResult. Insert key is the
     * hash of the payload (the apply Q's own queryHash).
     */
    void deferRequest(nlohmann::json payload)
    {
        if (!decisionGraph)
            return;
        pendingRequests.push_back(std::move(payload));
    }

    /**
     * Insert deferred Requests into the CAS pool at their natural
     * (payload-hash) keys. Called from `closeAsksEdge`. With
     * `processApplies=true` the trailing file/env-read chunk is
     * also closed; otherwise pending state stays buffered.
     */
    void flushPending(bool processApplies = false);

    /**
     * Close the current Asks edge. Calls flushPending, then closes
     * any trailing file/env-read batch. Called at every cb-apply
     * crossing and at logResult.
     */
    void closeAsksEdge(bool processApplies = false);

    /**
     * Push a new `CallbackCell` onto the writer's stack for a
     * cb-apply. The cell's `applyId` is the natural hash of the
     * apply query payload, used to route observations from the
     * TracingCallbackArg and TracingCallbackApplyResult back into
     * this cell's `runningObsSet` via `logCallbackObservation`.
     * The obsSet is later snapshotted into an ObservationSet
     * referenced from a QueryCallbackApply request.
     */
    void createCallbackCell(const nlohmann::json & applyQueryPayload);

    /**
     * Return the `applyId` of the cb-apply currently on top of
     * `callbackCells`, or nullopt if there is none. Used by
     * `IT::apply` when fn is a TracingCallbackArg (the recursive
     * cb-apply path) to capture the enclosing cell's id before the
     * recursive call would push a new one; the captured id flows
     * to the `TracingCallbackApplyResult` wrapping the apply
     * result, so its observations route to the enclosing cell's
     * runningObsSet.
     */
    std::optional<Hash> getCurrentCbApplyId() const
    {
        if (callbackCells.empty())
            return std::nullopt;
        return callbackCells.back().applyId;
    }

    /**
     * Log a d=0 Result. Records (Q_final, current factSet) -> Result
     * in the decision graph and returns a TriePosition for use by
     * child queries. Under the Q-evolution protocol, Q_final is the
     * activeQuery's `currentQ` after all this Q's observations have
     * folded — which may differ from the Q hash returned at
     * `logQuery` time.
     */
    template<typename R>
    std::optional<TriePosition> logResult(ValueHandle valueNum, const R & result, const QueryHandle & qh)
    {
        sink.logResult(valueNum, result);

        if (!decisionGraph || !qh.queryHash) {
            if (!activeQueryStack.empty())
                activeQueryStack.pop_back();
            return std::nullopt;
        }

        /* Process any pending Requests, finalise buffered cb-apply
           cells, and close the trailing Asks edge boundary. Any
           observations that fire during closeAsksEdge still fold
           into envWalk and evolve the innermost
           activeQuery's Q (via logOuterObservation). */
        closeAsksEdge(/*processApplies=*/ true);

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        decisionGraph->installFactSet(envFactSetHash, envFactSet);
        sessionRequestsTrie.persist(*decisionGraph);

        Hash finalQ = activeQueryStack.empty()
            ? *qh.queryHash
            : activeQueryStack.back().currentQ;
        tracingCacheLog("logResult: Q_initial=%s Q_final=%s factSet=%s -> result",
                        qh.queryHash->to_string(HashFormat::Base16, false).substr(0, 12),
                        finalQ.to_string(HashFormat::Base16, false).substr(0, 12),
                        envFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12));

        decisionGraph->insertTerminal(finalQ, envFactSetHash, resultNodeHash);

        /* B2: capture sub-Q's evolved payload so we can emit a
           composite observation against the parent Q (if one
           remains) after popping this frame. Composite is one fact
           folded into session-cumulative envFactSetHash, per
           Foundational 9. Retires the session-cumulative
           envAsksEdges bridging (composite replaces its role
           semantically, but landing-chain reachability from ∅/anchor
           is a separate follow-up). */
        std::optional<nlohmann::json> subQueryPayload;
        if (!activeQueryStack.empty()) {
            subQueryPayload = std::move(activeQueryStack.back().payloadTemplate);
            activeQueryStack.pop_back();
        }

        if (subQueryPayload && !activeQueryStack.empty())
            logCompositeSubQ(*subQueryPayload, finalQ, j, resultNodeHash);

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = finalQ.to_string(HashFormat::Base16, false),
            .factSetHash = envFactSetHash,
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

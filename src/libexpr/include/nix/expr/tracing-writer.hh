#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * decision-graph index.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/q-state.hh"
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
 * a hex string of the selectorHash that produced it (used by child queries
 * to compute their own selectorHash with Merkle provenance).
 */
struct TriePosition
{
    Hash resultNodeHash;          // ResultHash for this result
    std::string queryHashStr;     // hex of the selectorHash that produced it
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

    /** Per-active-selector state. Each `logSelector` allocates a QState
        and pushes it here; each `logResult` pops. LIFO nesting matches
        the evaluator's Q hierarchy (parent Q's evaluation triggers
        child Q's logSelector inside). Observations that fire while a Q
        is active are attributed to that Q's `currentQ`.

        Migration note (task list #158–#171): the QStates pushed here
        are the same shared objects that application cells store in
        their `ArgCell::qState` (once Phase B lands the sharing).
        Retiring `activeQueryStack` in Phase E just deletes the stack;
        callers walk cell chain to find the innermost `QState` instead.

        Field breakdown, and the concurrency rationale for putting
        this state on cells rather than a writer-global stack, live in
        `q-state.hh`. */
    using ActiveSelector = std::shared_ptr<QState>;
    std::vector<ActiveSelector> activeQueryStack;
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
       set into the ObservationSet CAS and emits a SelectorCallbackApply
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
        , sessionRootCell(ArgCell::make(nullptr, nullptr))
    {
    }

    /** Session-root cell: exists for the writer's lifetime, parents
        every arg cell created during the session. Under the multiplexer
        + per-cell factset direction (task #177): env facts fold into
        `sessionRootCell->ownFactSet`; all subsequent cells inherit via
        `factSetHash()`'s parent-chain walk.

        Currently unused (dual-write / read wiring lands in follow-up
        commits). Kept as a stable shared_ptr so cells can safely take
        it as their parent. */
    std::shared_ptr<ArgCell> sessionRootCell;

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

    /** Task #110 (C3): emit a SelectorCallbackApply observation for an
        applyResult, carrying the applyResult's WHNF as the Result.
        Called from TracingObject::whnf() when it has an
        applyResultSubject. Looks up the matching CallbackCell by
        fn's initial state hash, snapshots the cell's runningObsSet
        into the ObservationSet CAS, constructs the QCA payload with
        fn's current state hash as `fn`, and emits via the standard
        logOuterObservation path. Under content addressing, same
        (fn, obsSet) pair produces the same QCA selectorHash across
        callers — parent's chain sees a single QCA-per-firing. */
    void emitCallbackApplyForApplyResult(
        const std::shared_ptr<const ArgCell> & callbackCell,
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

        /* Cell-based reader (preferred): if a cell with populated
           callbackState is provided, and its fn matches, read from it
           directly — no writer.callbackCells iteration. */
        auto tryEmitFromCell = [&](const CallbackState & cs) -> bool {
            if (cs.argAncestryHex.empty())
                return false;
            if (cs.fnStateHashHex != fnInitialHex)
                return false;
            auto obsSetHash = decisionGraph->insertObservationSet(cs.runningObsSet);
            auto fnCurrent = stateHashAtSubject(
                *ar->fn, applyArgAncestry, envWalk, envWalk.size());
            trace::SelectorCallbackApply qca;
            qca.fn = trace::SelectorLeaf{trace::StateHashLeaf{
                fnCurrent.to_string(HashFormat::Base16, false),
                cs.argAncestryHex}};
            qca.argObsSet = obsSetHash.to_string(HashFormat::Base16, false);
            logOuterObservation(
                trace::SelectorVariant{std::move(qca)},
                trace::ResultVariant{whnf},
                *ar->fn,
                applyArgAncestry,
                callbackCell);
            return true;
        };
        if (callbackCell && callbackCell->callbackState
            && tryEmitFromCell(*callbackCell->callbackState))
            return;

        /* Fallback: iterate writer.callbackCells (pre-cell-migration
           path). Retained until all createCallbackCell callsites
           populate an ArgCell.callbackState reachable at the emit
           point. See task list for the retirement plan. */
        for (auto it = callbackCells.rbegin(); it != callbackCells.rend(); ++it) {
            auto & cell = *it;
            if (cell.fnStateHashHex != fnInitialHex)
                continue;
            if (cell.argAncestryHex.empty())
                continue;
            CallbackState adapter;
            adapter.applyId = cell.applyId;
            adapter.fnStateHashHex = cell.fnStateHashHex;
            adapter.argAncestryHex = cell.argAncestryHex;
            adapter.runningObsSet = cell.runningObsSet;
            (void) tryEmitFromCell(adapter);
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
    struct SelectorHandle
    {
        std::optional<Hash> selectorHash;
        /* Parent Query's terminalCur, captured at logSelector time. Used
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
    std::pair<ValueHandle, SelectorHandle> logRootSelector(const Q & query)
    {
        auto valueNum = sink.logSelector(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logRootSelector: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        ActiveSelector aq = std::make_shared<QState>();
        aq->currentQ = selectorHash;
        aq->payloadTemplate = trace::SelectorVariant{query};
        aq->queryTag = std::string(Q::tag);
        aq->initialPayloadTemplate = qj;
        aq->envAsksEdgesSizeAtPush = envAsksEdges.size();
        aq->prevCur = envFactSetHash;
        activeQueryStack.push_back(std::move(aq));
        return {valueNum, {selectorHash}};
    }

    /**
     * Cell-migration Phase C: root selector variant that also aliases
     * the pushed ActiveSelector shared_ptr onto the given cell's
     * qState. Mirrors `logSelectorOnCell` for root queries.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logRootSelectorOnCell(
        const std::shared_ptr<const ArgCell> & cell,
        const Q & query)
    {
        auto pair = logRootSelector(query);
        if (cell && !activeQueryStack.empty()) {
            cell->qState = activeQueryStack.back();
            cell->qState->cell = cell;
            /* #177: this Q's chain starts at the cell's ownFactSet
               (what's been folded into THIS cell so far). Walker from
               ∅ dispatches Q's Ask chain and folds new facts into
               local cur, ending at cell.ownFactSet after Q completes. */
            cell->qState->prevCur = cell->ownFactSet;
        }
        return pair;
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.). If
     * `fromSubject` is provided, the writer will re-derive Q's `from`
     * field after each observation that evolves that subject's state
     * hash, and Ask/Terminal rows for this Q will be keyed on the
     * evolved Q at each step (Q evolution protocol).
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logSelector(
        const Q & query,
        const std::optional<TriePosition> & parent,
        std::optional<Subject> fromSubject = std::nullopt,
        Hash fromSubjectArgAncestry = Hash(HashAlgorithm::SHA256))
    {
        auto valueNum = sink.logSelector(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logSelector: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        SelectorHandle qh{selectorHash};
        if (parent)
            qh.structuralParentFactSetHash = parent->factSetHash;
        Hash lastState(HashAlgorithm::SHA256);
        if (fromSubject) {
            lastState = stateHashAt(
                *fromSubject, fromSubjectArgAncestry, envWalk, envWalk.size());
        }
        ActiveSelector aq = std::make_shared<QState>();
        aq->currentQ = selectorHash;
        aq->payloadTemplate = trace::SelectorVariant{query};
        aq->fromSubject = std::move(fromSubject);
        aq->fromSubjectArgAncestry = fromSubjectArgAncestry;
        aq->fromSubjectLastState = lastState;
        aq->structuralParentFactSetHash = qh.structuralParentFactSetHash;
        aq->queryTag = std::string(Q::tag);
        aq->initialPayloadTemplate = qj;
        aq->initialFromSubjectState = lastState;
        aq->envAsksEdgesSizeAtPush = envAsksEdges.size();

        /* B11: preconditions. Under callback-model §3, Q's chain
           starts at index M > 0 carrying preconditions from prior
           state. Fold pre-push session observations into aq's per-Q
           chain, evolving aq->currentQ to Q_M and inserting Ask rows
           under each intermediate Q value so walkers starting at
           (Q_initial, ∅) can fold their way to (Q_M, Q_entry_cur).

           Under matching-until-divergence, walker's per-walk perQEnvWalk
           after committing the same N landing-chain obs contains the
           same ObservationSets → stateHashAt gives the same evolved
           state → walker's Q at end of landing == aq->currentQ (Q_M).
           Q's first own Ask (later, at logOuterObservation) is then
           recorded under aq->currentQ = Q_M, which walker finds. */
        if (aq->fromSubject) {
            for (size_t i = 0; i < envAsksEdges.size(); ++i) {
                const auto & edge = envAsksEdges[i];
                decisionGraph->insertAsk(aq->currentQ, edge.fromFactSetHash, edge.requestSetHash);
                if (i < envWalk.size()) {
                    aq->perQEnvWalk.push_back(envWalk[i]);
                    auto newState = stateHashAt(
                        *aq->fromSubject, aq->fromSubjectArgAncestry,
                        aq->perQEnvWalk, aq->perQEnvWalk.size());
                    if (newState != aq->fromSubjectLastState) {
                        aq->fromSubjectLastState = newState;
                        trace::rewriteFrom(
                            aq->payloadTemplate,
                            newState.to_string(HashFormat::Base16, false));
                        aq->currentQ = trace::computeSelectorHash(aq->payloadTemplate);
                    }
                }
            }
            if (envAsksEdges.size() > 0)
                tracingCacheLog("logSelector: precondition fold %zu obs -> Q_M=%s",
                                envAsksEdges.size(),
                                aq->currentQ.to_string(HashFormat::Base16, false).substr(0, 12));
        }

        /* Multiplexer prevCur: after precondition fold, this Q's own
           chain is at "session cur when Q became active." */
        aq->prevCur = envFactSetHash;
        activeQueryStack.push_back(std::move(aq));
        return {valueNum, qh};
    }

    /**
     * Cell-migration Phase B variant: same as `logSelector`, but also
     * aliases the pushed `ActiveSelector` shared_ptr onto the given
     * cell's `qState`. Mutations on either side (writer's stack via
     * `activeQueryStack.back()->...`, or cell via `cell->qState->...`)
     * are visible to both — they're the same `QState`.
     *
     * Callers that create an ArgCell for an application (SelectorApply,
     * SelectorCallbackApply, root SelectorImport/SelectorExpr in later
     * phases) use this to associate the Selector's chain state with
     * the cell. Cell-only lookups (walker after Phase E) will read
     * qState directly from the cell without going through the writer's
     * stack.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logSelectorOnCell(
        const std::shared_ptr<const ArgCell> & cell,
        const Q & query,
        const std::optional<TriePosition> & parent,
        std::optional<Subject> fromSubject = std::nullopt,
        Hash fromSubjectArgAncestry = Hash(HashAlgorithm::SHA256))
    {
        auto pair = logSelector(query, parent, std::move(fromSubject), std::move(fromSubjectArgAncestry));
        if (cell && !activeQueryStack.empty()) {
            cell->qState = activeQueryStack.back();
            cell->qState->cell = cell;
            /* #177: this Q's chain starts at cell's ownFactSet. */
            cell->qState->prevCur = cell->ownFactSet;
        }
        return pair;
    }

    /**
     * Cell-migration Phase D2: getter Query — trace-only, no push
     * onto activeQueryStack. Contra-observations dispatched during
     * the getter's evaluation attribute to whatever frame is on top
     * of the stack (the enclosing apply/root cell), not to a
     * getter-specific frame. Terminal is inserted via logQueryResult
     * at a caller-supplied anchorCur (typically parent's
     * terminalCur) — no factSet chain of the getter's own.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logQuery(
        const Q & query,
        const std::optional<TriePosition> & parent)
    {
        auto valueNum = sink.logSelector(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logQuery: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        SelectorHandle qh{selectorHash};
        if (parent)
            qh.structuralParentFactSetHash = parent->factSetHash;
        return {valueNum, qh};
    }

    /**
     * Cell-migration Phase D2: getter Result. Inserts a direct
     * Terminal at (getterSelectorHash, anchorCur) — no chain walk,
     * no logResult side effects (no closeAsksEdge, no pop from
     * activeQueryStack).
     */
    template<typename R>
    std::optional<TriePosition> logQueryResult(
        ValueHandle valueNum,
        const R & result,
        const SelectorHandle & qh,
        const Hash & anchorCur)
    {
        sink.logResult(valueNum, result);
        if (!decisionGraph || !qh.selectorHash)
            return std::nullopt;
        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);
        decisionGraph->insertTerminal(*qh.selectorHash, anchorCur, resultNodeHash);
        tracingCacheLog(
            "writer logQueryResult: Q=%s anchor=%s -> result=%s",
            qh.selectorHash->to_string(HashFormat::Base16, false).substr(0, 12),
            anchorCur.to_string(HashFormat::Base16, false).substr(0, 12),
            resultNodeHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.selectorHash->to_string(HashFormat::Base16, false),
            .factSetHash = anchorCur,
        };
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
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(resp.request);
        auto responsePayload = jsonToCborString(respJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        decisionGraph->insertRequest(selectorHash, jsonToCborString(reqJson));
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), selectorHash, responseHash);
        if (!seenRequests.insert(factHash).second)
            return;
        envFactSet.push_back({selectorHash, responseHash});
        envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
            envFactSetHash, selectorHash, responseHash);
        /* #177 broadcast fold: env facts are ambient — every active
           cell folds them into its own ownFactSet. Walker from ∅
           dispatches Q's own Ask chain and reaches cell.ownFactSet
           naturally without landing chains. sessionRootCell folds too
           so cross-Q session accounting still has an anchor. */
        sessionRootCell->ownFactSet = TracingDecisionGraph::xorFactIntoHash(
            sessionRootCell->ownFactSet, selectorHash, responseHash);
        for (auto & aq : activeQueryStack) {
            if (auto cell = aq->cell.lock())
                cell->ownFactSet = TracingDecisionGraph::xorFactIntoHash(
                    cell->ownFactSet, selectorHash, responseHash);
        }
        responseFor.emplace(selectorHash, responseHash);
        sessionRequestsTrie.insert(selectorHash);
        allRequestHashes.insert(selectorHash);
        auto requestSetHash = decisionGraph->insertRequestSet({selectorHash});
        /* Multiplexer broadcast (user 2026-07-25/26): a fact is ambient
           — it's caused by the interpreter's own execution, not by any
           specific Q. Every currently-active Q's chain must include it
           so a walker following that Q's chain from ∅ can reach the Q's
           Terminal cur (= session cur at logResult time). */
        ObservationSet obsSet;
        obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        for (auto & aq : activeQueryStack) {
            /* #177 reader switch: Ask keyed at aq's cell.ownFactSet
               (before-fold value cached in aq->prevCur). Env fact
               already broadcast-folded into every active cell above,
               so cell->ownFactSet is up-to-date for the advance. */
            decisionGraph->insertAsk(aq->currentQ, aq->prevCur, requestSetHash);
            aq->perQEnvWalk.push_back(obsSet);
            if (auto cell = aq->cell.lock())
                aq->prevCur = cell->ownFactSet;
            else
                aq->prevCur = envFactSetHash;
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        envWalk.push_back(std::move(obsSet));
        prevQFactSetHash = envFactSetHash;
        /* Facts have fromHash=0; no subject state hash evolves — no
           per-Q Q evolution triggered here. */
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
        const trace::SelectorVariant & query,
        const trace::ResultVariant & result,
        Subject subject,
        Hash argAncestry = Hash(HashAlgorithm::SHA256),
        const std::shared_ptr<const ArgCell> & attributionCell = {});

    /**
     * Record one observation the outer made on a callback firing's
     * contra-arg. Routes to the matching CallbackCell by `applyId`
     * and pushes an `{selectorHash, responsePayload}` entry into that
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
        const trace::SelectorVariant & query,
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
            trace::SelectorVariant stampedQuery = query;
            auto par = pathAndRootsFromSubject(subject);
            std::vector<trace::SelectorLeaf> fromStateHashes;
            fromStateHashes.reserve(par.roots.size());
            for (auto & root : par.roots) {
                auto cid = stateHashAfter(root, argAncestry, {});
                fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::visit([&](auto & q) {
                if constexpr (requires { q.from; }) {
                    q.from = fromStateHashes.empty()
                        ? trace::SelectorLeaf{std::string{}}
                        : fromStateHashes[0];
                }
                if constexpr (requires { q.perArgFrame; }) {
                    q.perArgFrame.path = par.path;
                    q.perArgFrame.fromStateHashes = fromStateHashes;
                }
                if constexpr (requires { q.fromStateHashes = fromStateHashes; }) {  // SelectorApply
                    q.fromStateHashes = fromStateHashes;
                }
            }, stampedQuery);
            auto qh = std::visit(
                [](const auto & q) {
                    return TracingDecisionGraph::computeSelectorHash(q);
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
            decisionGraph->insertAsk(innermost->currentQ, prevQFactSetHash, requestSetHash);
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        ObservationSet obsSet;
        obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        envWalk.push_back(std::move(obsSet));
        prevQFactSetHash = envFactSetHash;
    }

    /**
     * Defer a Requests-pool insert until logResult. Insert key is the
     * hash of the payload (the apply Q's own selectorHash).
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
     * referenced from a SelectorCallbackApply request.
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
     * `logSelector` time.
     */
    template<typename R>
    std::optional<TriePosition> logResult(ValueHandle valueNum, const R & result, const SelectorHandle & qh)
    {
        sink.logResult(valueNum, result);

        if (!decisionGraph || !qh.selectorHash) {
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
            ? *qh.selectorHash
            : activeQueryStack.back()->currentQ;
        /* #177 reader switch: Terminal keyed at the completing Q's
           cell.ownFactSet (sibling-isolated by construction — each
           cell has its own accumulator). Env facts broadcast-folded
           into every active cell so ownFactSet includes them. */
        Hash terminalCur = envFactSetHash;
        if (!activeQueryStack.empty()) {
            if (auto cell = activeQueryStack.back()->cell.lock())
                terminalCur = cell->ownFactSet;
        }
        tracingCacheLog("logResult: Q_initial=%s Q_final=%s factSet=%s -> result",
                        qh.selectorHash->to_string(HashFormat::Base16, false).substr(0, 12),
                        finalQ.to_string(HashFormat::Base16, false).substr(0, 12),
                        terminalCur.to_string(HashFormat::Base16, false).substr(0, 12));

        decisionGraph->insertTerminal(finalQ, terminalCur, resultNodeHash);

        /* D3: composite sub-Q emission retired. Under D2, getters no
           longer create their own writer frames — sub-Q completions
           don't need parent-chain composite observations to signal
           reachability. Sibling discrimination is via QCA obsSet
           content (callback-model §7b), not via composite dispatch
           failure. Just pop the completed frame. */
        if (!activeQueryStack.empty())
            activeQueryStack.pop_back();

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = finalQ.to_string(HashFormat::Base16, false),
            .factSetHash = terminalCur,
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

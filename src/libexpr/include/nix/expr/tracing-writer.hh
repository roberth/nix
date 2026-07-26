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

    /* Dedup guard for fact insertion — skips XOR-cancelling duplicates. */
    std::unordered_set<Hash> seenRequests;

    /* request → response lookup, maintained as facts arrive. */
    std::unordered_map<Hash, Hash> responseFor;

    /* Incremental trie of allRequests; gives record() the canonical
       RequestSet hash for the whole-remaining edge in O(1). */
    TracingDecisionGraph::TrieBuilder sessionRequestsTrie;

    /** Currently-active cells. Each Selector push (via logSelectorOnCell
        / logRootSelectorOnCell) appends the cell; logResult pops.
        Used at logResult to identify the completing cell whose facts
        become the Selector's single Ask requestSet (task #183). */
    std::vector<std::shared_ptr<const ArgCell>> activeCells;

    /* All request hashes ever inserted. Not deduped against seenRequests
       (which is fact-hashed, not request-hashed). */
    std::unordered_set<Hash> allRequestHashes;

    /** Emit the SelectorCallbackApply Fact for a callback firing whose
        result is now known. Snapshots the cell's `runningObsSet` into
        the ObservationSet CAS and routes the fact through
        `logOuterObservation`. Idempotent-ish: no-op if runningObsSet
        is empty or if the cell match fails. */

public:
    TracingWriter(TraceSink & sink, TracingDecisionGraph * decisionGraph = nullptr)
        : sink(sink)
        , decisionGraph(decisionGraph)
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
        auto fnInitial = subjectId(*ar->fn, applyArgAncestry);
        auto fnInitialHex = fnInitial.to_string(HashFormat::Base16, false);

        /* Cell-based reader (preferred): if a cell with populated
           callbackState is provided, and its fn matches, read from it
           directly — no writer.callbackCells iteration. */
        auto tryEmitFromCell = [&](const CallbackState & cs) -> bool {
            if (cs.argAncestryHex.empty())
                return false;
            /* #183: cs.fnStateHashHex is Q-space; fnInitialHex is
               subject-space. They don't align. When cell was threaded
               through (callbackCell provided), we trust the caller: the
               cell IS the callback firing being completed. Skip the
               subject-space match. */
            (void) fnInitialHex;
            auto obsSetHash = decisionGraph->insertObservationSet(cs.runningObsSet);
            trace::SelectorCallbackApply qca;
            /* #181: fn = the callback fn's query-space identity,
               captured at firing time by CallbackState. Discriminates
               callbackApply Q across distinct callbacks with same
               obsSet (mirrors SelectorApply.fn treatment). */
            qca.fn = cs.fnStateHashHex;
            qca.argObsSet = obsSetHash.to_string(HashFormat::Base16, false);
            /* #177 attribution: QCA observation folds into the
               enclosing SelectorApply's cell (the innermost active
               Q's cell), not callbackCell. callbackCell is a lookup
               handle for the runningObsSet content, not the fold
               target — the QCA is an outer probe on the SelectorApply
               being evaluated, so it belongs to that cell's chain.
               Without this, sibling SelectorApply Terminals collapse
               at cur=∅ and warm returns wrong-sibling responses
               (cb-obsset-mismatch-clean-miss, sibling tests). */
            std::shared_ptr<const ArgCell> attrCell;
            if (!activeCells.empty())
                attrCell = activeCells.back()->qState->cell.lock();
            logOuterObservation(
                trace::SelectorVariant{std::move(qca)},
                trace::ResultVariant{whnf},
                *ar->fn,
                applyArgAncestry,
                attrCell);
            return true;
        };
        if (callbackCell && callbackCell->callbackState
            && tryEmitFromCell(*callbackCell->callbackState))
            return;

        /* #184 step 2: fallback loop over writer.callbackCells retired.
           Every caller of emitCallbackApplyForApplyResult now threads a
           cell whose callbackState is populated (TE::apply since #184
           step 1; OuterApply::run + cbApplyOrigin wrappers since
           earlier). If we reach here, the primary path failed to
           match — probe the reason. */
        tracingCacheLog(
            "emitCallbackApplyForApplyResult: primary path returned false; "
            "callbackCell=%p callbackState=%p fnInitialHex=%s",
            (void*) callbackCell.get(),
            callbackCell ? (void*) callbackCell->callbackState.get() : nullptr,
            fnInitialHex.substr(0, 12).c_str());
    }

    /**
     * Opaque handle linking a query to its result.
     */
    struct SelectorHandle
    {
        std::optional<Hash> selectorHash;
    };

    /**
     * Log a root query (evalFile, evalExpr, apply) on a cell. Root
     * queries have no evolving from-subject, so qState carries the Q
     * as fixed. Pushes `cell` onto `activeCells` — cells ARE the
     * active-set under the #179 retire of the old activeQueryStack.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logRootSelectorOnCell(
        const std::shared_ptr<const ArgCell> & cell,
        const Q & query)
    {
        auto valueNum = sink.logSelector(query);
        if (!decisionGraph)
            return {valueNum, {}};
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logRootSelectorOnCell: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        auto qState = std::make_shared<QState>();
        qState->currentQ = selectorHash;
        if (cell) {
            cell->qState = qState;
            qState->cell = cell;
            activeCells.push_back(cell);
        }
        return {valueNum, {selectorHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.) on
     * a cell. If `fromSubject` is provided, the writer will re-derive
     * Q's `from` field after each observation that evolves that
     * subject's state hash. Pushes `cell` onto `activeCells`.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logSelectorOnCell(
        const std::shared_ptr<const ArgCell> & cell,
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
            "writer logSelectorOnCell: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        SelectorHandle qh{selectorHash};
        (void) parent;  // structuralParentFactSetHash retired
        (void) fromSubject;
        (void) fromSubjectArgAncestry;
        auto qState = std::make_shared<QState>();
        qState->currentQ = selectorHash;
        /* #178: Q evolution retires. fromSubject / precondition-fold /
           payloadTemplate.from rewriting all gone. Q hashes stable
           per operation; cur at (Q, cur) discriminates. */

        if (cell) {
            cell->qState = qState;
            qState->cell = cell;
            activeCells.push_back(cell);
        }
        return {valueNum, qh};
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
        const std::optional<TriePosition> & parent,
        const std::shared_ptr<const ArgCell> & cell = {})
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
        (void) parent;
        (void) cell;  // #183: cell no longer needed at push — facts accumulate on it directly
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
        const Hash & anchorCur,
        const std::shared_ptr<const ArgCell> & cell = {})
    {
        sink.logResult(valueNum, result);
        if (!decisionGraph || !qh.selectorHash)
            return std::nullopt;
        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        /* #182: if a cell is provided (getter was pushed via logQuery),
           write Terminal at the LIVE post-fold cell.factSetHash() —
           not the pre-fold anchor snapshot. Then pop the matching
           in-progress entry. */
        Hash terminalCur = cell ? cell->factSetHash() : anchorCur;
        decisionGraph->insertResult(resultNodeHash, resultPayload);
        /* #183: one Ask per Selector containing all facts from cell +
           ancestors. Walker dispatches all → folds all → reaches
           terminalCur = cell.factSetHash(). Order-independent (XOR). */
        if (cell) {
            std::vector<Hash> reqHashes;
            for (auto c = cell.get(); c; c = c->parent.get()) {
                for (auto & [req, resp] : c->facts) {
                    (void) resp;
                    reqHashes.push_back(req);
                }
            }
            if (!reqHashes.empty()) {
                auto requestSetHash = decisionGraph->insertRequestSet(reqHashes);
                decisionGraph->insertAsk(*qh.selectorHash,
                    TracingDecisionGraph::emptySetHash(), requestSetHash);
            }
        }
        decisionGraph->insertTerminal(*qh.selectorHash, terminalCur, resultNodeHash);
        tracingCacheLog(
            "writer logQueryResult: Q=%s anchor=%s -> result=%s",
            qh.selectorHash->to_string(HashFormat::Base16, false).substr(0, 12),
            terminalCur.to_string(HashFormat::Base16, false).substr(0, 12),
            resultNodeHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.selectorHash->to_string(HashFormat::Base16, false),
            .factSetHash = terminalCur,
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
        /* #183: env facts append to session-root cell's fact set.
           Descendants inherit via factSetHash()'s parent-chain walk.
           Ask rows are inserted per-Selector-completion. */
        sessionRootCell->addFact(selectorHash, responseHash);
        responseFor.emplace(selectorHash, responseHash);
        sessionRequestsTrie.insert(selectorHash);
        allRequestHashes.insert(selectorHash);
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
        /* #183: fact appends to sessionRootCell. Ask insertion happens
           at Selector completion. */
        sessionRootCell->addFact(request, response);
        sessionRequestsTrie.insert(request);
        allRequestHashes.insert(request);
    }

    /**
     * Insert a Query payload into the Requests pool at its natural
     * (payload-hash) key. Historically deferred and batched via
     * closeAsksEdge; direct-insert now that the batching machinery
     * (pendingRequests / pendingNewRequests / flushPending /
     * closeAsksEdge) has retired.
     */
    void deferRequest(nlohmann::json payload)
    {
        if (!decisionGraph)
            return;
        auto key = hashString(HashAlgorithm::SHA256, payload.dump());
        decisionGraph->insertRequest(key, jsonToCborString(payload));
    }

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
            if (!activeCells.empty())
                activeCells.pop_back();
            return std::nullopt;
        }

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        sessionRequestsTrie.persist(*decisionGraph);

        Hash finalQ = activeCells.empty()
            ? *qh.selectorHash
            : activeCells.back()->qState->currentQ;
        /* #177 pull model: Terminal keyed at the completing Q's
           cell.factSetHash() — this cell's own facts XORed with
           ancestor factSetHashes on demand. Sibling isolation is
           structural (siblings have separate cells → separate
           ownFactSets, ancestors shared via parent chain). */
        Hash terminalCur = sessionRootCell->factSetHash();
        std::shared_ptr<const ArgCell> completingCell;
        if (!activeCells.empty()) {
            completingCell = activeCells.back();
            terminalCur = completingCell->factSetHash();
        }
        tracingCacheLog("logResult: Q_initial=%s Q_final=%s factSet=%s -> result",
                        qh.selectorHash->to_string(HashFormat::Base16, false).substr(0, 12),
                        finalQ.to_string(HashFormat::Base16, false).substr(0, 12),
                        terminalCur.to_string(HashFormat::Base16, false).substr(0, 12));

        /* #183: one Ask per Selector with all facts from cell + ancestors. */
        if (completingCell) {
            std::vector<Hash> reqHashes;
            for (auto c = completingCell.get(); c; c = c->parent.get()) {
                for (auto & [req, resp] : c->facts) {
                    (void) resp;
                    reqHashes.push_back(req);
                }
            }
            if (!reqHashes.empty()) {
                auto requestSetHash = decisionGraph->insertRequestSet(reqHashes);
                decisionGraph->insertAsk(finalQ,
                    TracingDecisionGraph::emptySetHash(), requestSetHash);
            }
        }
        decisionGraph->insertTerminal(finalQ, terminalCur, resultNodeHash);

        /* D3: composite sub-Q emission retired. Under D2, getters no
           longer create their own writer frames — sub-Q completions
           don't need parent-chain composite observations to signal
           reachability. Sibling discrimination is via QCA obsSet
           content (callback-model §7b), not via composite dispatch
           failure. Just pop the completed frame. */
        if (!activeCells.empty())
            activeCells.pop_back();

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

#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * decision-graph index.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/q-state.hh"
#include "nix/expr/observation-set.hh"
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

    /* #188: activeCells retired. QCA attribution reaches the enclosing
       SelectorApply's cell via callbackCell->parent (forwarded by
       OuterObject::queryApply's applyCell construction, not looked up
       via global stack). logResult reads the cell parameter directly. */

    /* All request hashes ever inserted. Not deduped against seenRequests
       (which is fact-hashed, not request-hashed). */
    std::unordered_set<Hash> allRequestHashes;

    /* Writer-global barrier counter (Foundational principle 9). Each
       fact recorded gets stamped with the CURRENT value; bumped AFTER
       a value probe. Non-value facts (env-file, env-var) do not bump.
       At logResult time, facts are grouped by barrier to insert a
       causally-ordered Ask chain. Not persisted. */
    uint64_t nextBarrier = 0;

public:
    /** Current barrier value — call before adding a fact to stamp it
        at the current writer step. */
    uint64_t peekBarrier() const { return nextBarrier; }

    /** Bump the barrier — call AFTER recording a value probe's fact. */
    void bumpBarrier() { ++nextBarrier; }

private:

    /** #187: insert a barrier-grouped Ask chain from `startCur` over
        `facts`. Each Ask edge adds one barrier group (at most one
        value probe per group). Foundational principle 9. */
    void insertBarrieredChain(
        const Hash & selectorHash,
        const Hash & startCur,
        const std::map<Hash, std::pair<uint64_t, Hash>> & facts)
    {
        if (facts.empty())
            return;
        /* Group by barrier: request-keyed map into barrier-keyed groups. */
        std::map<uint64_t, std::vector<std::pair<Hash, Hash>>> byBarrier;
        for (auto & [req, br_resp] : facts)
            byBarrier[br_resp.first].emplace_back(req, br_resp.second);
        auto cur = startCur;
        for (auto & [barrier, entries] : byBarrier) {
            (void) barrier;
            std::vector<Hash> reqHashes;
            reqHashes.reserve(entries.size());
            for (auto & [req, resp] : entries)
                reqHashes.push_back(req);
            auto requestSetHash = decisionGraph->insertRequestSet(reqHashes);
            decisionGraph->insertAsk(selectorHash, cur, requestSetHash);
            for (auto & [req, resp] : entries)
                cur = TracingDecisionGraph::xorFactIntoHash(cur, req, resp);
        }
    }

    /** #187: insert Ask chains for Q at both anchors, per Foundational
        principle 9 + user's fix "A":

        - Full chain from ∅, over cell + ancestor facts. Walker starting
          from ∅ (or ∅-fallback) traverses this chain.
        - Delta chain from parent.terminalCur (= cell.parent.factSetHash),
          over THIS cell's own facts only. Walker at parent-anchor finds
          this shorter chain directly without needing the ∅-fallback.

        Both chains reach the same final cur = cell.factSetHash(). */
    void insertBarrieredAskChain(
        const Hash & selectorHash,
        const std::shared_ptr<const ArgCell> & cell)
    {
        /* Full chain: cell + ancestors, from ∅. */
        std::map<Hash, std::pair<uint64_t, Hash>> allFacts;
        for (auto c = cell.get(); c; c = c->parent.get())
            for (auto & [req, entry] : c->facts)
                allFacts.try_emplace(req, entry.barrier, entry.response);
        insertBarrieredChain(selectorHash,
            TracingDecisionGraph::emptySetHash(), allFacts);

        /* Delta chain: cell's own facts, from parent.terminalCur.
           Only useful when cell has a parent AND own facts to add;
           otherwise the delta chain would be either unanchored or empty. */
        if (cell->parent && !cell->facts.empty()) {
            std::map<Hash, std::pair<uint64_t, Hash>> ownFacts;
            for (auto & [req, entry] : cell->facts)
                ownFacts.try_emplace(req, entry.barrier, entry.response);
            auto parentCur = cell->parent->factSetHash();
            insertBarrieredChain(selectorHash, parentCur, ownFacts);
        }
    }

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
        const trace::SelectorVariant & applyResultProducer,
        const trace::ResultWHNF & whnf)
    {
        if (!decisionGraph)
            return;
        auto * ap = std::get_if<trace::SelectorApply>(&applyResultProducer);
        if (!ap)
            return;
        auto fnInitialHex = ap->fn;

        /* Cell-based reader (preferred): if a cell with populated
           callbackState is provided, and its fn matches, read from it
           directly — no writer.callbackCells iteration. */
        auto tryEmitFromCell = [&](const CallbackState & cs) -> bool {
            /* Skip until at least one contra-arg observation has fired —
               otherwise the obsSet is empty and QCA emission is
               premature. */
            if (cs.runningObsSet.empty())
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
            /* #188 attribution: QCA folds into the callback's
               enclosing scope — the cell the callback provider
               constructed applyCell against (callerScope). That's
               reachable via callbackCell->parent under one-cell-per-
               apply construction: OuterObject::queryApply makes
               applyCell = ArgCell::make(callerScope, argObj), so
               callbackCell.parent IS callerScope, i.e. the enclosing
               SelectorApply's cell whose factSetHash the enclosing
               getter's Terminal reads. Forwarded via the constructed
               cell chain, not looked up via global stack. */
            std::shared_ptr<const ArgCell> attrCell = callbackCell ? callbackCell->parent : nullptr;
            /* producer describe: we only have `fn` as a hex string here
               (SelectorApply payload's fn field); the identity that
               matters is qca's own content hash. */
            logOuterObservation(
                trace::SelectorVariant{std::move(qca)},
                trace::ResultVariant{whnf},
                "fn=" + ap->fn.substr(0, 12),
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
     * as fixed.
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
        }
        return {valueNum, {selectorHash}};
    }

    /**
     * Log a query on an existing value (getAttr, getString, etc.) on
     * a cell.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logSelectorOnCell(
        const std::shared_ptr<const ArgCell> & cell,
        const Q & query,
        const std::optional<TriePosition> & parent)
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
        auto qState = std::make_shared<QState>();
        qState->currentQ = selectorHash;
        /* #178: Q evolution retires. fromSubject / precondition-fold /
           payloadTemplate.from rewriting all gone. Q hashes stable
           per operation; cur at (Q, cur) discriminates. */

        if (cell) {
            cell->qState = qState;
            qState->cell = cell;
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
        /* #187 principle 9: barrier-based Ask chain, one Ask per barrier
           group (one value probe per group). Walker dispatches each
           edge's requestSet live, folds, reaches next cur; a divergent
           live response at any barrier misses cleanly there. */
        if (cell)
            insertBarrieredAskChain(*qh.selectorHash, cell);
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
           Ask rows are inserted per-Selector-completion.

           #187 principle 9: env-file / env-var are NOT value probes —
           stamp with current barrier, do not bump. */
        sessionRootCell->addFact(selectorHash, responseHash, peekBarrier());
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
        std::string producerDesc,
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
           at Selector completion. #187: env fact — peek barrier, no bump. */
        sessionRootCell->addFact(request, response, peekBarrier());
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
     * Insert the apply query payload into the Requests pool so the
     * walker can look it up by request hash. Post-#184 the writer no
     * longer maintains a callbackCells vector — the actual
     * callback-firing state lives on `ArgCell::callbackState`,
     * populated by the caller alongside this insertion.
     */
    void createCallbackCell(const nlohmann::json & applyQueryPayload);

    /**
     * Log a d=0 Result. Records (Q, current factSet) -> Result in the
     * decision graph and returns a TriePosition for use by child
     * queries. Under Q-space identity, Q is stable per operation —
     * the walker computes the same Q hash the writer emits here.
     */
    template<typename R>
    std::optional<TriePosition> logResult(
        ValueHandle valueNum,
        const R & result,
        const SelectorHandle & qh,
        const std::shared_ptr<const ArgCell> & cell = {})
    {
        sink.logResult(valueNum, result);

        if (!decisionGraph || !qh.selectorHash)
            return std::nullopt;

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph->insertResult(resultNodeHash, resultPayload);

        sessionRequestsTrie.persist(*decisionGraph);

        Hash finalQ = *qh.selectorHash;
        /* #177 pull model: Terminal keyed at the completing Q's
           cell.factSetHash() — this cell's own facts XORed with
           ancestor factSetHashes on demand. */
        Hash terminalCur = cell ? cell->factSetHash() : sessionRootCell->factSetHash();
        tracingCacheLog("logResult: Q=%s factSet=%s -> result",
                        finalQ.to_string(HashFormat::Base16, false).substr(0, 12),
                        terminalCur.to_string(HashFormat::Base16, false).substr(0, 12));

        /* #187 principle 9: barrier-based Ask chain (see comment on
           insertBarrieredAskChain). One Ask edge per barrier group,
           each carrying at most one value probe. */
        if (cell)
            insertBarrieredAskChain(finalQ, cell);
        decisionGraph->insertTerminal(finalQ, terminalCur, resultNodeHash);

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

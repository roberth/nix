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
    TracingDecisionGraph & decisionGraph;

    /* Dedup guard for fact insertion — skips XOR-cancelling duplicates. */
    std::unordered_set<Hash> seenRequests;

    /* request → response lookup, maintained as facts arrive. */
    std::unordered_map<Hash, Hash> responseFor;

    /* Incremental trie of allRequests; gives record() the canonical
       RequestSet hash for the whole-remaining edge in O(1). */
    TracingDecisionGraph::TrieBuilder sessionRequestsTrie;


    /* State-creep canonicalisation record: for every fact that
       canonicalisation replaced on the arg cell, remember what request
       hash it replaced. At Ask insertion time (`insertBarrieredChain`),
       any requestSet containing a key here gets a companion alt
       requestSet with the key substituted back to the pre-canonical
       request hash — stamped as the Ask row's altRequestSet. Walker's
       one-shot alt fallback uses that alt to reach recordings that
       used the pre-canonical shape. See main doc's
       "state/observation-creep canonicalisation" note, record side. */
    std::unordered_map<Hash, Hash> canonicalReplacements;

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

    /** #187 + follow-then-insert: at each cur, first follow any
        existing outgoing Ask whose requestSet is a subset of our
        remaining reqs (no live dispatch — we fold in our
        already-known responses). This lets a second writer piggyback
        on an earlier writer's chain, reducing multi-outgoing
        ambiguity. When no existing Ask matches, insert the remaining
        facts as barrier-grouped Asks from the current cur. */
    void insertBarrieredChain(
        const Hash & selectorHash,
        const Hash & startCur,
        const std::map<Hash, std::pair<uint64_t, Hash>> & facts)
    {
        if (facts.empty())
            return;
        /* responseFor is our known-response lookup for facts; remaining
           tracks reqs not yet consumed (via follow or insert). */
        std::unordered_map<Hash, Hash> responseFor;
        std::unordered_set<Hash> remaining;
        responseFor.reserve(facts.size());
        remaining.reserve(facts.size());
        for (auto & [req, br_resp] : facts) {
            responseFor.emplace(req, br_resp.second);
            remaining.insert(req);
        }
        auto cur = startCur;
        std::unordered_set<Hash> dispatchedSoFar;

        /* Follow phase: greedy consume any existing edge whose useful
           subset is fully covered by our remaining reqs. Reads from
           the DB, folds with our own responses, updates cur. */
        while (true) {
            bool followed = false;
            for (const auto & edge : decisionGraph.getAsks(selectorHash, cur)) {
                auto rsMembers = decisionGraph.getRequestSet(edge.requestSet);
                if (!rsMembers)
                    continue;
                std::vector<Hash> useful;
                useful.reserve(rsMembers->size());
                for (const auto & req : *rsMembers)
                    if (!dispatchedSoFar.count(req))
                        useful.push_back(req);
                if (useful.empty())
                    continue;
                bool subset = std::all_of(useful.begin(), useful.end(),
                    [&](const Hash & req) { return remaining.count(req) > 0; });
                if (!subset)
                    continue;
                /* Follow: fold each useful req/resp into cur, dedupe
                   from remaining/dispatchedSoFar. */
                for (const auto & req : useful) {
                    auto it = responseFor.find(req);
                    /* Guaranteed by `subset` check above. */
                    cur = TracingDecisionGraph::xorFactIntoHash(cur, req, it->second);
                    remaining.erase(req);
                    dispatchedSoFar.insert(req);
                }
                tracingCacheLog("insertBarrieredChain follow Q=%s cur→%s (consumed %zu req)",
                                selectorHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                                cur.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                                useful.size());
                followed = true;
                break; // restart to see fresh outgoing at new cur
            }
            if (!followed)
                break;
        }

        /* Insert phase: barrier-group only the leftover reqs from cur. */
        if (remaining.empty())
            return;
        std::map<uint64_t, std::vector<std::pair<Hash, Hash>>> byBarrier;
        for (auto & [req, br_resp] : facts)
            if (remaining.count(req))
                byBarrier[br_resp.first].emplace_back(req, br_resp.second);
        /* Iterate by barrier group; the barrier value itself isn't
           needed in-loop — sortedness of `byBarrier` drives the order. */
        for (auto & barrierGroup : byBarrier) {
            auto & entries = barrierGroup.second;
            std::vector<TracingDecisionGraph::Fact> factList;
            factList.reserve(entries.size());
            for (auto & [req, resp] : entries)
                factList.push_back({req, resp});

            /* Alt stamping: if any req in this Ask's rs was a canonical
               replacement, build a companion alt rs with the pre-canonical
               req substituted. Walker's one-shot fallback lets replay
               reach cross-session recordings that used the pre-canonical
               shape. See main doc's canonicalisation note, record side. */
            std::optional<Hash> altRequestSetHash;
            std::vector<Hash> altReqHashes;
            altReqHashes.reserve(factList.size());
            bool anySubstituted = false;
            for (const auto & f : factList) {
                auto it = canonicalReplacements.find(f.request);
                if (it != canonicalReplacements.end()) {
                    altReqHashes.push_back(it->second);
                    anySubstituted = true;
                } else {
                    altReqHashes.push_back(f.request);
                }
            }
            if (anySubstituted)
                altRequestSetHash = decisionGraph.insertRequestSet(altReqHashes);

            decisionGraph.insertAskSplitting(
                selectorHash, cur, factList, dispatchedSoFar, altRequestSetHash);
            for (auto & [req, resp] : entries) {
                cur = TracingDecisionGraph::xorFactIntoHash(cur, req, resp);
                dispatchedSoFar.insert(req);
            }
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
        /* Diagnostic: detect a fact attributed to both this cell and
           an ancestor. Under XOR-fold, that would make factSetHash
           silently cancel the fact — the cell's fold reads as if the
           fact never happened. */
        {
            std::unordered_set<Hash> ownReqs;
            for (auto & [req, entry] : cell->facts) ownReqs.insert(req);
            for (auto c = cell->parent.get(); c; c = c->parent.get())
                for (auto & [req, entry] : c->facts)
                    if (ownReqs.count(req))
                        tracingCacheLog(
                            "XOR-CANCEL RISK: req=%s appears in cell.facts AND ancestor.facts",
                            req.to_string(HashFormat::Base16, false).substr(0, 12).c_str());
        }
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
    TracingWriter(TraceSink & sink, TracingDecisionGraph & decisionGraph)
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
        ref<const trace::Selector> applyResultProducer,
        const trace::ResultWHNF & whnf)
    {
        auto * ap = std::get_if<trace::SelectorApply>(&applyResultProducer->node);
        if (!ap)
            return;
        auto fnParent = ap->parent;

        auto tryEmitFromCell = [&](const CallbackState & cs) -> bool {
            if (cs.runningObsSet.empty())
                return false;
            auto obsSetHash = decisionGraph.insertObservationSet(cs.runningObsSet);
            /* fnStateHashHex captures fn's Q-space identity at firing
               time. Look up in pool; fall back to fnParent on miss. */
            ref<const trace::Selector> fnRef = fnParent;
            try {
                auto fnHash = Hash::parseNonSRIUnprefixed(cs.fnStateHashHex, HashAlgorithm::SHA256);
                if (auto found = decisionGraph.selectorPool.find(fnHash))
                    fnRef = *found;
            } catch (...) {}
            auto qcaSel = decisionGraph.selectorPool.intern(trace::SelectorCallbackApply{
                obsSetHash.to_string(HashFormat::Base16, false), fnRef});
            std::shared_ptr<const ArgCell> attrCell = callbackCell ? callbackCell->parent : nullptr;
            logOuterObservation(
                qcaSel,
                trace::ResultVariant{whnf},
                "fn=" + fnParent->cachedHash.to_string(HashFormat::Base16, false).substr(0, 12),
                attrCell);
            return true;
        };
        if (callbackCell && callbackCell->callbackState
            && tryEmitFromCell(*callbackCell->callbackState))
            return;

        tracingCacheLog(
            "emitCallbackApplyForApplyResult: primary path returned false; "
            "callbackCell=%p callbackState=%p fnParent=%s",
            (void*) callbackCell.get(),
            callbackCell ? (void*) callbackCell->callbackState.get() : nullptr,
            fnParent->cachedHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str());
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
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logRootSelectorOnCell: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        auto qState = std::make_shared<QState>();
        qState->currentQ = selectorHash;
        if (cell) {
            /* Cross-link both directions atomically. QState needs the
               back-ref for cell->factSetHash() lookups during Ask/Terminal
               keying; cell needs the forward-ref for cross-cell state
               access on descendants. Setting only one direction leaves a
               dangling half-link that will silently miss on the other side. */
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
        const Q & query)
    {
        auto valueNum = sink.logSelector(query);
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logSelectorOnCell: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        SelectorHandle qh{selectorHash};
        auto qState = std::make_shared<QState>();
        qState->currentQ = selectorHash;
        if (cell) {
            /* Cross-link both directions atomically. QState needs the
               back-ref for cell->factSetHash() lookups during Ask/Terminal
               keying; cell needs the forward-ref for cross-cell state
               access on descendants. Setting only one direction leaves a
               dangling half-link that will silently miss on the other side. */
            cell->qState = qState;
            qState->cell = cell;
        }
        return {valueNum, qh};
    }

    /**
     * Log a getter Selector — trace-only. Contra-observations dispatched
     * during the getter's evaluation attribute to the enclosing apply/root
     * cell, not to a getter-specific frame. Terminal is inserted via
     * logQueryResult at a caller-supplied anchorCur (typically parent's
     * terminalCur) — no factSet chain of the getter's own.
     */
    template<typename Q>
    std::pair<ValueHandle, SelectorHandle> logQuery(const Q & query)
    {
        auto valueNum = sink.logSelector(query);
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
        nlohmann::json qj = query;
        tracingCacheLog(
            "writer logQuery: Q=%s queryJSON=%s",
            selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
            qj.dump());
        return {valueNum, SelectorHandle{selectorHash}};
    }

    /**
     * Log a getter Result. Inserts a direct Terminal at
     * (getterSelectorHash, anchorCur) — no chain walk, no logResult
     * side effects.
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
        if (!qh.selectorHash)
            return std::nullopt;
        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        /* #182: if a cell is provided (getter was pushed via logQuery),
           write Terminal at the LIVE post-fold cell.factSetHash() —
           not the pre-fold anchor snapshot. Then pop the matching
           in-progress entry. */
        Hash terminalCur = cell ? cell->factSetHash() : anchorCur;
        decisionGraph.insertResult(resultNodeHash, resultPayload);
        /* #187 principle 9: barrier-based Ask chain, one Ask per barrier
           group (one value probe per group). Walker dispatches each
           edge's requestSet live, folds, reaches next cur; a divergent
           live response at any barrier misses cleanly there. */
        if (cell)
            insertBarrieredAskChain(*qh.selectorHash, cell);
        decisionGraph.insertTerminal(*qh.selectorHash, terminalCur, resultNodeHash);
        tracingCacheLog(
            "writer logQueryResult: Q=%s anchor=%s -> result=%s",
            qh.selectorHash->to_string(HashFormat::Base16, false).substr(0, 12),
            terminalCur.to_string(HashFormat::Base16, false).substr(0, 12),
            resultNodeHash.to_string(HashFormat::Base16, false).substr(0, 12));
        if (cell) {
            tracingCacheLog("  logResult cell chain (%p):", (const void *) cell.get());
            for (auto c = cell.get(); c; c = c->parent.get()) {
                tracingCacheLog("    cell=%p depth=%d facts=%zu factSetHash=%s",
                    (const void *) c, c->depth, c->facts.size(),
                    c->factSetHash().to_string(HashFormat::Base16, false).substr(0, 12).c_str());
                for (const auto & [req, entry] : c->facts) {
                    tracingCacheLog("      fact req=%s resp=%s",
                        req.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
                        entry.response.to_string(HashFormat::Base16, false).substr(0, 12).c_str());
                }
            }
        }
        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.selectorHash->to_string(HashFormat::Base16, false),
            .factSetHash = terminalCur,
        };
    }

    /**
     * Log a response (file read, env lookup, etc.) — a d>0
     * Request/Response pair. Records the fact on the session-root
     * cell (env facts default there per #183; descendants inherit
     * via ArgCell::factSetHash's parent-chain walk) and dedupes via
     * seenRequests so repeated identical probes don't double-add.
     */
    template<typename Req>
    void logResponse(const trace::Response<Req> & resp)
    {
        sink.log(nlohmann::json(resp));
        nlohmann::json reqJson = resp.request;
        nlohmann::json respJson = resp.response;
        auto selectorHash = TracingDecisionGraph::computeSelectorHash(resp.request);
        auto responsePayload = jsonToCborString(respJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
        decisionGraph.insertRequest(selectorHash, jsonToCborString(reqJson));
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
        ref<const trace::Selector> query,
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
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), request, response);
        if (!seenRequests.insert(factHash).second)
            return;
        responseFor.emplace(request, response);
        /* #183: fact appends to sessionRootCell. Ask insertion happens
           at Selector completion. #187: env fact — peek barrier, no bump. */
        sessionRootCell->addFact(request, response, peekBarrier());
        sessionRequestsTrie.insert(request);
    }

    /**
     * Insert a Query payload into the Requests pool at its natural
     * (payload-hash) key.
     */
    void deferRequest(nlohmann::json payload)
    {
        auto key = hashString(HashAlgorithm::SHA256, payload.dump());
        decisionGraph.insertRequest(key, jsonToCborString(payload));
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

        if (!qh.selectorHash)
            return std::nullopt;

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph.insertResult(resultNodeHash, resultPayload);

        sessionRequestsTrie.persist(decisionGraph);

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
        decisionGraph.insertTerminal(finalQ, terminalCur, resultNodeHash);

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

    TracingDecisionGraph & getDecisionGraph() const
    {
        return decisionGraph;
    }
};

} // namespace nix

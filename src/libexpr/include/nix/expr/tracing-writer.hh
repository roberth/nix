#pragma once
/**
 * @file
 * Trace writer that logs evaluation events to a JSON sink and the
 * decision-graph index.
 */

#include "nix/expr/arg-cell.hh"
#include "nix/expr/q-state.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/request-set-trie.hh"
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
    TracingHash resultNodeHash;   // ResultHash for this result
    std::string queryHashStr;     // hex of the selectorHash that produced it
    /* Walker-side: the cur the history landed on when committing
       this terminal. Used by child Q lookups as a candidate startCur
       (= structurally-anchored lookup position) so a child history
       starts from its parent's reached factSet rather than from
       session-leaky envCur. Empty hash on TracingReplayObjects synthesized
       outside the walker (recording side). */
    TracingHash factSetHash = trace::tracingZeroHash();
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
    std::unordered_set<TracingHash> seenRequests;

    /* request → response lookup, maintained as facts arrive. */
    std::unordered_map<TracingHash, TracingHash> responseFor;

    /* Incremental trie of all requests observed in this session.
       Built via the new rst:: MutableNode + FrozenNodeCache pair:
       inserts are O(depth) with cachedFrozen invalidation walking
       the modified path; the persist walk at logResult only enqueues
       subtrees not already known to the cache. */
    trace::rst::MutableNode sessionRequestsMutable;
    trace::rst::FrozenNodeCache sessionRequestsCache;


    /* State-creep canonicalisation record: for every fact that
       canonicalisation replaced on the arg cell, remember what request
       hash it replaced. At Ask insertion time (`insertBarrieredChain`),
       any requestSet containing a key here gets a companion alt
       requestSet with the key substituted back to the pre-canonical
       request hash — stamped as the Ask row's altRequestSet. Walker's
       one-shot alt fallback uses that alt to reach recordings that
       used the pre-canonical shape. See main doc's
       "state/observation-creep canonicalisation" note, record side. */
    std::unordered_map<TracingHash, TracingHash> canonicalReplacements;

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

    /** Per-fact staging entry carried into insertBarrieredChain — mirrors
        ArgCell::FactEntry (response + elementHash + barrier). The
        elementHash is the precomputed BLAKE3(req||resp) so the follow
        loop's XOR-fold uses it directly instead of re-hashing. */
    struct ChainFact
    {
        uint64_t barrier;
        TracingHash response;
        TracingHash elementHash;
    };

    /** #187 + follow-then-insert: at each cur, first follow any
        existing outgoing Ask whose requestSet is a subset of our
        remaining reqs (no live dispatch — we fold in our
        already-known responses). This lets a second writer piggyback
        on an earlier writer's chain, reducing multi-outgoing
        ambiguity. When no existing Ask matches, insert the remaining
        facts as barrier-grouped Asks from the current cur. */
    void insertBarrieredChain(
        const TracingHash & selectorHash,
        const TracingHash & startCur,
        const std::unordered_map<TracingHash, ChainFact> & facts)
    {
        if (facts.empty())
            return;
        /* dispatchedSoFar tracks reqs already folded (via follow or,
           at the tail, by insertAskSplitting). `remaining ∈ facts`
           iff `!dispatchedSoFar.count(req)` — no need to materialise
           a separate set; querying `facts` directly is O(1) too. */
        auto cur = startCur;
        std::unordered_set<TracingHash> dispatchedSoFar;

        /* Follow phase: greedy consume any existing edge whose useful
           subset is fully covered by facts \ dispatchedSoFar. Reads
           from the DB, folds with our own responses, updates cur. */
        while (true) {
            bool followed = false;
            for (const auto & edge : decisionGraph.getAsks(selectorHash, cur)) {
                auto rsMembers = decisionGraph.getRequestSet(edge.requestSet);
                if (!rsMembers)
                    continue;
                std::vector<TracingHash> useful;
                useful.reserve(rsMembers->size());
                for (const auto & req : *rsMembers)
                    if (!dispatchedSoFar.count(req))
                        useful.push_back(req);
                if (useful.empty())
                    continue;
                bool subset = std::all_of(useful.begin(), useful.end(),
                    [&](const TracingHash & req) { return facts.count(req) > 0; });
                if (!subset)
                    continue;
                /* Follow: fold each useful req/resp into cur, mark
                   dispatched. `subset` above guarantees facts.at(req)
                   succeeds. Uses the precomputed elementHash so this
                   loop is BLAKE3-free. */
                for (const auto & req : useful) {
                    auto it = facts.find(req);
                    cur.xorInPlace(it->second.elementHash);
                    dispatchedSoFar.insert(req);
                }
                tracingCacheLog("insertBarrieredChain follow Q=%s cur→%s (consumed %zu req)",
                                selectorHash.toHex().substr(0, 12).c_str(),
                                cur.toHex().substr(0, 12).c_str(),
                                useful.size());
                followed = true;
                break; // restart to see fresh outgoing at new cur
            }
            if (!followed)
                break;
        }

        /* Insert phase: barrier-group only the leftover reqs from cur. */
        if (dispatchedSoFar.size() == facts.size())
            return;
        std::map<uint64_t, std::vector<TracingDecisionGraph::Fact>> byBarrier;
        for (auto & [req, cf] : facts)
            if (!dispatchedSoFar.count(req))
                byBarrier[cf.barrier].push_back({req, cf.response});
        /* Iterate by barrier group; the barrier value itself isn't
           needed in-loop — sortedness of `byBarrier` drives the order. */
        for (auto & barrierGroup : byBarrier) {
            auto & factList = barrierGroup.second;

            /* Alt stamping: if any req in this Ask's rs was a canonical
               replacement, build a companion alt rs with the pre-canonical
               req substituted. Walker's one-shot fallback lets replay
               reach cross-session recordings that used the pre-canonical
               shape. See main doc's canonicalisation note, record side.

               First pass computes the alt rs's identity (XOR of the
               substituted request hashes) streaming — no vector alloc.
               If the identity is already in the trie cache we skip the
               vector build entirely. Under matching-until-divergence
               and heavy rs-reuse, the fast path fires often. */
            std::optional<TracingHash> altRequestSetHash;
            TracingHash altXor = trace::tracingZeroHash();
            bool anySubstituted = false;
            for (const auto & f : factList) {
                auto it = canonicalReplacements.find(f.request);
                TracingHash req = f.request;
                if (it != canonicalReplacements.end()) {
                    req = it->second;
                    anySubstituted = true;
                }
                altXor.xorInPlace(req);
            }
            if (anySubstituted) {
                trace::rst::FrozenNodePtr altNode = [&] {
                    if (auto existing = decisionGraph.tryFindRequestSet(altXor))
                        return *existing;
                    /* Miss: materialise the vector and intern. */
                    std::vector<TracingHash> altReqHashes;
                    altReqHashes.reserve(factList.size());
                    for (const auto & f : factList) {
                        auto it = canonicalReplacements.find(f.request);
                        altReqHashes.push_back(
                            it != canonicalReplacements.end() ? it->second : f.request);
                    }
                    return decisionGraph.internRequestSet(std::move(altReqHashes));
                }();
                altRequestSetHash = decisionGraph.insertRequestSet(altNode);
            }

            decisionGraph.insertAskSplitting(
                selectorHash, cur, factList, dispatchedSoFar, altRequestSetHash);
            for (const auto & f : factList) {
                cur = TracingDecisionGraph::xorFactIntoHash(cur, f.request, f.response);
                dispatchedSoFar.insert(f.request);
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
        const TracingHash & selectorHash,
        const std::shared_ptr<const ArgCell> & cell)
    {
        /* Structural chain (task 1a, task 1b enabler): if this
           Selector is a getter whose parent is also a getter recorded
           on this cell, insert a delta chain anchored at parent's
           OLDEST terminalCur (only facts with barrier > parent's
           barrierAtRecord). Walker's structural-anchor fallback
           (task 237) will start at parent-TRO's terminalCur when the
           cell-anchor walk misses, hitting this chain.

           When it fires, we skip the ∅-chain and the cell-topology
           delta chain — they'd duplicate this coverage under the
           walker's new anchor sequence.

           Limited to getter→getter to avoid cross-cell interactions
           at Apply/CallbackApply boundaries. */
        bool structuralInserted = false;
        if (auto sel = decisionGraph.selectorPool.find(selectorHash)) {
            std::optional<TracingHash> parentHash;
            std::visit(overloaded{
                [&](const trace::SelectorGetAttr & g) {
                    std::visit(overloaded{
                        [&](const trace::SelectorGetAttr &)     { parentHash = g.parent->cachedHash; },
                        [&](const trace::SelectorGetListElem &) { parentHash = g.parent->cachedHash; },
                        [&](const auto &) {}
                    }, g.parent->node);
                },
                [&](const trace::SelectorGetListElem & g) {
                    std::visit(overloaded{
                        [&](const trace::SelectorGetAttr &)     { parentHash = g.parent->cachedHash; },
                        [&](const trace::SelectorGetListElem &) { parentHash = g.parent->cachedHash; },
                        [&](const auto &) {}
                    }, g.parent->node);
                },
                [&](const auto &) {}
            }, (**sel).node);
            if (parentHash) {
                auto it = cell->firstTerminalCurs.find(*parentHash);
                if (it != cell->firstTerminalCurs.end()
                    && it->second.epochAtRecord == cell->canonicalisationEpochChain())
                {
                    /* Epoch matches — no canonicalisation-driven fact
                       removal on this cell or any ancestor since
                       parent's record. Under reverse-De-Bruijn
                       SelectorArg{depth}, obsset members are globally
                       unique across nested firings, and every fact-add
                       bumps the barrier — so the delta filter precisely
                       separates before-parent-record from after, no
                       XOR-cancel arithmetic. */
                    /* factsInOrder is sorted by barrier (addFact only
                       appends, peekBarrier is monotonic), so binary
                       search finds the first tail entry >= threshold
                       and we iterate only what we need — no scan of the
                       pre-parent-record prefix. */
                    std::unordered_map<TracingHash, ChainFact> deltaFacts;
                    auto threshold = it->second.barrierAtRecord;
                    for (auto c = cell.get(); c; c = c->parent.get()) {
                        auto & v = c->factsInOrder;
                        auto lb = std::lower_bound(v.begin(), v.end(), threshold,
                            [](const auto & p, uint64_t t) { return p.second.barrier < t; });
                        for (auto j = lb; j != v.end(); ++j)
                            deltaFacts.try_emplace(j->first,
                                ChainFact{j->second.barrier, j->second.response, j->second.elementHash});
                    }
                    if (!deltaFacts.empty())
                        insertBarrieredChain(selectorHash,
                            it->second.terminalCur, deltaFacts);
                    structuralInserted = true;
                }
            }
        }

        if (!structuralInserted) {
            /* Full chain: cell + ancestors, from ∅. */
            std::unordered_map<TracingHash, ChainFact> allFacts;
            for (auto c = cell.get(); c; c = c->parent.get())
                for (auto & [req, entry] : c->factsInOrder)
                    allFacts.try_emplace(req,
                        ChainFact{entry.barrier, entry.response, entry.elementHash});
            insertBarrieredChain(selectorHash,
                TracingDecisionGraph::emptySetHash(), allFacts);

            /* Cell-topology delta chain: cell's own facts from
               parent.terminalCur. Covers callback-firing nesting where
               the structural-parent isn't on the same cell. */
            if (cell->parent && !cell->factsInOrder.empty()) {
                std::unordered_map<TracingHash, ChainFact> ownFacts;
                for (auto & [req, entry] : cell->factsInOrder)
                    ownFacts.try_emplace(req,
                        ChainFact{entry.barrier, entry.response, entry.elementHash});
                auto parentCur = cell->parent->factSetHash();
                insertBarrieredChain(selectorHash, parentCur, ownFacts);
            }
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
        , sessionRootCell(RegularArgCell::make(nullptr, nullptr))
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
                auto fnHash = trace::parseTracingHex(cs.fnStateHashHex);
                if (auto found = decisionGraph.selectorPool.find(fnHash))
                    fnRef = *found;
            } catch (...) {}
            auto qcaSel = decisionGraph.selectorPool.intern(trace::SelectorCallbackApply{
                obsSetHash, fnRef});
            std::shared_ptr<const ArgCell> attrCell = callbackCell ? callbackCell->parent : nullptr;
            logOuterObservation(
                qcaSel,
                trace::ResultVariant{whnf},
                "fn=" + fnParent->cachedHash.toHex().substr(0, 12),
                attrCell);
            return true;
        };
        if (callbackCell)
            if (auto * cs = callbackCell->getCallbackState())
                if (tryEmitFromCell(*cs))
                    return;

        tracingCacheLog(
            "emitCallbackApplyForApplyResult: primary path returned false; "
            "callbackCell=%p callbackState=%p fnParent=%s",
            (void*) callbackCell.get(),
            callbackCell ? (void*) callbackCell->getCallbackState() : nullptr,
            fnParent->cachedHash.toHex().substr(0, 12).c_str());
    }

    /** Phantom tag used to keep SelectorHandle distinct from any
        other HashOf<X> type. */
    struct SelectorPhantom
    {};
    /** Opaque handle linking a query to its result. */
    using SelectorHandle = HashOf<SelectorPhantom>;

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
            selectorHash.toHex().substr(0, 12),
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
        return {valueNum, SelectorHandle{selectorHash}};
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
            selectorHash.toHex().substr(0, 12),
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
            selectorHash.toHex().substr(0, 12),
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
        const TracingHash & anchorCur,
        const std::shared_ptr<const ArgCell> & cell = {})
    {
        sink.logResult(valueNum, result);
        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        /* #182: if a cell is provided (getter was pushed via logQuery),
           write Terminal at the LIVE post-fold cell.factSetHash() —
           not the pre-fold anchor snapshot. Then pop the matching
           in-progress entry. */
        TracingHash terminalCur = cell ? cell->factSetHash() : anchorCur;
        decisionGraph.insertResult(resultNodeHash, resultPayload);
        /* #187 principle 9: barrier-based Ask chain, one Ask per barrier
           group (one value probe per group). Walker dispatches each
           edge's requestSet live, folds, reaches next cur; a divergent
           live response at any barrier misses cleanly there. */
        if (cell) {
            insertBarrieredAskChain(qh.raw, cell);
            /* task 1a: record oldest terminalCur per Selector on the
               cell so descendant Qs can anchor structural chains here.
               Bump the barrier and snapshot post-bump so subsequent
               facts (env or value) get a barrier >= barrierAtRecord
               only when they were added after this record. Env facts
               (which don't bump per F9) would otherwise share the
               barrier with parent's record moment and be spuriously
               double-counted in a descendant's delta. */
            bumpBarrier();
            cell->firstTerminalCurs.try_emplace(qh.raw,
                ArgCell::FirstTerminalRecord{terminalCur, peekBarrier(),
                    cell->canonicalisationEpochChain()});
        }
        decisionGraph.insertTerminal(qh.raw, terminalCur, resultNodeHash);
        tracingCacheLog(
            "writer logQueryResult: Q=%s anchor=%s -> result=%s",
            qh.raw.toHex().substr(0, 12),
            terminalCur.toHex().substr(0, 12),
            resultNodeHash.toHex().substr(0, 12));
        if (cell) {
            tracingCacheLog("  logResult cell chain (%p):", (const void *) cell.get());
            for (auto c = cell.get(); c; c = c->parent.get()) {
                tracingCacheLog("    cell=%p depth=%d facts=%zu factSetHash=%s",
                    (const void *) c, c->depth, c->facts.size(),
                    c->factSetHash().toHex().substr(0, 12).c_str());
                for (const auto & [req, entry] : c->facts) {
                    tracingCacheLog("      fact req=%s resp=%s",
                        req.toHex().substr(0, 12).c_str(),
                        entry.response.toHex().substr(0, 12).c_str());
                }
            }
        }
        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = qh.raw.toHex(),
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
            trace::tracingZeroHash(), selectorHash, responseHash);
        if (!seenRequests.insert(factHash).second)
            return;
        /* #183: env facts append to session-root cell's fact set.
           Descendants inherit via factSetHash()'s parent-chain walk.
           Ask rows are inserted per-Selector-completion.

           #187 principle 9: env-file / env-var are NOT value probes —
           stamp with current barrier, do not bump. Multiple env facts
           batch into one Ask row (safely batchable per F9). Delta
           filter correctness is maintained by bumping the barrier at
           logQueryResult time, which separates facts folded into
           parent's terminalCur from later ones. */
        sessionRootCell->addFact(selectorHash, responseHash, peekBarrier());
        responseFor.emplace(selectorHash, responseHash);
        sessionRequestsMutable.insert(selectorHash);
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
     * Insert a Query payload into the Requests pool at its natural
     * (payload-hash) key.
     */
    void deferRequest(nlohmann::json payload)
    {
        /* Hash the CBOR bytes rather than payload.dump() — one blake3
           call over one buffer instead of dump-string-into-vector +
           blake3, and CBOR is what gets stored anyway so key + payload
           share their derivation. Also robust to any binary_t fields
           in the JSON (dump() throws on those). */
        auto cbor = jsonToCborString(payload);
        auto key = trace::tracingHash(cbor);
        decisionGraph.insertRequest(key, std::move(cbor));
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

        nlohmann::json j = result;
        auto resultPayload = jsonToCborString(j);
        auto resultNodeHash = TracingDecisionGraph::computeResponseHash(resultPayload);
        decisionGraph.insertResult(resultNodeHash, resultPayload);

        {
            /* Freeze the incrementally-built mutable trie, then walk
               and enqueue any unpersisted subtree. Both steps reuse
               unchanged branches via the cachedFrozen / persisted
               flags, so a second logResult in the same session only
               does work for facts recorded since the previous one. */
            auto frozen = sessionRequestsMutable.freeze(sessionRequestsCache);
            trace::rst::FrozenNodeCache::PersistSink sink =
                [this](const TracingHash & h, std::string_view payload) {
                    decisionGraph.persistRequestSetNode(h, payload);
                };
            sessionRequestsCache.persist(frozen, sink);
        }

        TracingHash finalQ = qh.raw;
        /* #177 pull model: Terminal keyed at the completing Q's
           cell.factSetHash() — this cell's own facts XORed with
           ancestor factSetHashes on demand. */
        TracingHash terminalCur = cell ? cell->factSetHash() : sessionRootCell->factSetHash();
        tracingCacheLog("logResult: Q=%s factSet=%s -> result",
                        finalQ.toHex().substr(0, 12),
                        terminalCur.toHex().substr(0, 12));

        /* #187 principle 9: barrier-based Ask chain (see comment on
           insertBarrieredAskChain). One Ask edge per barrier group,
           each carrying at most one value probe. */
        if (cell) {
            insertBarrieredAskChain(finalQ, cell);
            /* task 1a: record oldest terminalCur per Selector on the
               cell so descendant Qs can anchor structural chains here. */
            cell->firstTerminalCurs.try_emplace(finalQ,
                ArgCell::FirstTerminalRecord{terminalCur, peekBarrier(),
                    cell->canonicalisationEpochChain()});
        }
        decisionGraph.insertTerminal(finalQ, terminalCur, resultNodeHash);

        return TriePosition{
            .resultNodeHash = resultNodeHash,
            .queryHashStr = finalQ.toHex(),
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

    TracingDecisionGraph & getDecisionGraph() const
    {
        return decisionGraph;
    }
};

} // namespace nix

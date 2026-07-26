#include "nix/expr/tracing-writer.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <set>

namespace nix {


void TracingWriter::logOuterObservation(
    const trace::SelectorVariant & query,
    const trace::ResultVariant & result,
    Subject subject,
    Hash argAncestry,
    const std::shared_ptr<const ArgCell> & attributionCell)
{
    if (!decisionGraph)
        return;

    /* Task #110 (C3): SelectorCallbackApply emission moved to
       TracingObject::whnf() where the applyResult's WHNF is
       actually known. No preamble here — a WHNF query always
       precedes any structural access on an applyResult, so cold
       will have emitted QCA-with-WHNF by the time non-WHNF probes
       on that applyResult happen. */

    /* #178: state-hash `from` field stamping retires. Under the
       per-cell factset model, cur at (Q, cur) does the discrimination
       the `from` state hash used to do. Q hashes become stable per
       operation. The caller-supplied `query` is used as-is; its
       `from`/`perArgFrame`/`fromStateHashes` fields (still present
       on the wire until the Selector types get pruned) stay at their
       caller-set values (typically defaults). */
    (void) argAncestry;  // no longer used for stamping
    Hash fromStateHash(HashAlgorithm::SHA256);

    std::string queryTag = std::visit(
        [](const auto & q) -> std::string { return std::string(q.tag); }, query);
    tracingCacheLog(
        "logOuterObservation: subject=%s query=%s",
        describe(subject), queryTag);

    trace::SelectorVariant stampedQuery = query;
    nlohmann::json queryJson = trace::toJson(stampedQuery);
    nlohmann::json resultJson;
    std::visit([&](const auto & r) { resultJson = r; }, result);

    auto selectorHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
    auto responsePayload = jsonToCborString(resultJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

    tracingCacheLog(
        "  reqHash=%s reqJSON=%s",
        selectorHash.to_string(HashFormat::Base16, false).substr(0, 12),
        queryJson.dump());
    tracingCacheLog(
        "  respHash=%s respJSON=%s",
        responseHash.to_string(HashFormat::Base16, false).substr(0, 12),
        resultJson.dump());
    if (provenanceEnabled()) {
        recordProvenance(selectorHash, "requestHash-d1",
                         {{"queryJson", queryJson},
                          {"subject", describe(subject)},
                          {"argAncestry", argAncestry.to_string(HashFormat::Base16, false)}});
        recordProvenance(responseHash, "responseHash-d1",
                         {{"resultJson", resultJson},
                          {"selectorHash", selectorHash.to_string(HashFormat::Base16, false)}});
    }

    decisionGraph->insertRequest(selectorHash, jsonToCborString(queryJson));

    /* #178: secondary getter-index at initial-history state hash
       retires with the primary stamping. Selector hashes are now
       stable per operation; the fallback lookup index is redundant. */

    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), selectorHash, responseHash);
    auto factHash = elementHash;

    /* Dedup by (request, response). If already recorded this session,
       skip both envFactSet fold AND envWalk push — pushing a duplicate
       ObservationSet would XOR-cancel its earlier contribution to any
       Subject's own-loop fold (see the design's XOR audit). */
    if (!seenRequests.insert(factHash).second)
        return;

    envFactSet.push_back({selectorHash, responseHash});
    envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
        envFactSetHash, selectorHash, responseHash);
    /* #177 C: fold observation into caller-supplied arg cell's
       ownFactSet. Dual-write for now; existing envFactSet/envWalk
       path continues. attributionCell is null when caller doesn't
       have one (transitional — QCA emission from
       emitCallbackApplyForApplyResult passes callbackCell; queryFn
       passes the arg proxy's cell). */
    if (attributionCell) {
        attributionCell->ownFactSet = TracingDecisionGraph::xorFactIntoHash(
            attributionCell->ownFactSet, selectorHash, responseHash);
    }
    responseFor.emplace(selectorHash, responseHash);
    sessionRequestsTrie.insert(selectorHash);
    allRequestHashes.insert(selectorHash);

    /* Per-probe Ask push. Task #110 Q-evolution: an observation
       happening during Q's walk is part of Q's Ask chain — for EVERY
       Q currently active on the stack. Parent Q's evaluation includes
       child Q's observations too; each Q's chain must be complete for
       the walker to follow it. Insert Ask immediately under every
       active Q's currentQ.

       Order per active Q: (1) record Ask at (Q_before-fold, cur_before),
       (2) fold observation into cur/envWalk, (3) re-derive Q_after-fold
       (see below). */
    auto requestSetHash = decisionGraph->insertRequestSet({selectorHash});
    /* Task #110 (correct model): each observation belongs to exactly
       one Q — the innermost active one. Sub-Qs' observations are
       NOT part of parent Q's chain; parent observes the sub-Q as a
       composite (via its own logResult that folds sub-Q's Terminal
       into parent's envWalk). Skip Ask insertion when the stack is
       empty (no attributable Q). */
    if (!activeCells.empty()) {
        auto & innermost = activeCells.back()->qState;
        /* #177 reader switch: Ask keyed at innermost's cell factSetHash
           (before-fold value in innermost->prevCur). Falls back to
           prevQFactSetHash when innermost has no cell backpointer. */
        decisionGraph->insertAsk(innermost->currentQ, innermost->prevCur, requestSetHash);
    }
    envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
    ObservationSet obsSet;
    obsSet.observations.push_back({fromStateHash, elementHash});
    envWalk.push_back(obsSet);
    /* Task #110: append to innermost Q's perQEnvWalk. Session envWalk
       stays 1:1-aligned with envAsksEdges for other bookkeeping. */
    if (!activeCells.empty()) {
        auto & innermost = activeCells.back()->qState;
        innermost->perQEnvWalk.push_back(std::move(obsSet));
        /* #177 pull model: advance innermost's prevCur to
           cell.factSetHash() (own XOR ancestors). */
        if (auto cell = innermost->cell.lock())
            innermost->prevCur = cell->factSetHash();
        else
            innermost->prevCur = envFactSetHash;
    }
    tracingCacheLog(
        "logOuterObservation: inserted Ask under %zu active Q(s) from=%s (env=%zu)",
        activeCells.size(),
        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
        envWalk.size());
    prevQFactSetHash = envFactSetHash;

    /* #178: Q evolution retires. Q hashes stable per operation; cur
       at (Q, cur) does the discrimination that state-hash Q evolution
       used to do. */
}

void TracingWriter::flushPending(bool processApplies)
{
    if (!decisionGraph)
        return;

    /* Pass A: insert deferred Requests at their natural keys
       (= hash of the payload). */
    for (auto & payload : pendingRequests) {
        auto key = hashString(HashAlgorithm::SHA256, payload.dump());
        decisionGraph->insertRequest(key, jsonToCborString(payload));
    }
    pendingRequests.clear();

    /* Outer-value probes are stamped and pushed per-probe in
       `logOuterObservation`, not batched here. Callback observations
       accumulate in their enclosing CallbackCell's runningObsSet and
       get snapshotted at sampling moments; nothing to flush here. */

    if (!processApplies) {
        /* Intermediate flush: pending state stays buffered until the
           enclosing evaluation reaches logResult. */
        return;
    }

    /* Finalize: close the trailing chunk of file/env-var reads (which
       flow through logResponse, not through the per-probe
       logOuterObservation path). One Ask per closeAsksEdge covers
       these; they don't need per-observation state evolution because
       their requestHashes don't carry a `from` field. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        /* Task #110 (correct model): innermost active Q only. */
        if (!activeCells.empty()) {
            auto & innermost = activeCells.back()->qState;
            decisionGraph->insertAsk(innermost->currentQ, prevQFactSetHash, requestSetHash);
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        envWalk.push_back({});  // 1:1 with envAsksEdges; empty is harmless for stateHashAt.
        tracingCacheLog("finalize: final env Asks edge from=%s rs-size=%zu (perQ=%zu env=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        envAsksEdges.size(),
                        envWalk.size());
        prevQFactSetHash = envFactSetHash;
        pendingNewRequests.clear();
    }
}

void TracingWriter::closeAsksEdge(bool processApplies)
{
    if (!decisionGraph)
        return;

    /* Insert any deferred Requests at their natural keys. */
    flushPending(processApplies);

    /* Close the trailing file/env-read batch (logResponse path only —
       outer-value probes push their own Ask per probe). One Ask row
       per closeAsksEdge covers whatever file/env reads have
       accumulated since the last close; walker's envWalk gets an
       empty ObservationSet (file/env reads don't advance any
       Subject's state hash) so the 1:1 alignment holds. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        /* Task #110 (correct model): innermost active Q only. */
        if (!activeCells.empty()) {
            auto & innermost = activeCells.back()->qState;
            decisionGraph->insertAsk(innermost->currentQ, prevQFactSetHash, requestSetHash);
        }
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        envWalk.push_back({});  // 1:1 with envAsksEdges; empty is harmless for stateHashAt.
        tracingCacheLog("closeAsksEdge: new Asks edge from=%s rs-size=%zu (perQ=%zu env=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        envAsksEdges.size(),
                        envWalk.size());
        prevQFactSetHash = envFactSetHash;
        pendingNewRequests.clear();
    }
}

void TracingWriter::createCallbackCell(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;

    /* Suppressed during walker re-dispatch of an already-recorded
       apply. Re-dispatch is validation, not a new cb-apply event —
       skip cell creation so `envWalk` alignment doesn't drift. The
       apply Request payload is still inserted so walker lookups
       find it. */
    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    if (suppressCbApply > 0) {
        tracingCacheLog("createCallbackCell: SUPPRESSED");
        decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);
        return;
    }

    if (provenanceEnabled())
        recordProvenance(applyReqHash, "applyRequestHash",
                         {{"applyQueryPayload", applyQueryPayload},
                          {"prevQFactSetHash", prevQFactSetHash.to_string(HashFormat::Base16, false)},
                          {"envFactSetHash", envFactSetHash.to_string(HashFormat::Base16, false)}});
    decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);

    /* Extract fn's state hash from the applyQueryPayload (params.fn)
       so we can look up the cell at CallbackApplyRef stamping time
       without re-parsing. Empty string on legacy payloads where the
       field isn't present (in which case CallbackApply emission is
       skipped). */
    std::string fnStateHashHex;
    try {
        /* Flat envelope: fn lives at top level of applyQueryPayload. */
        if (applyQueryPayload.contains("fn")
            && applyQueryPayload["fn"].is_object()
            && applyQueryPayload["fn"].contains("stateHash"))
            fnStateHashHex = applyQueryPayload["fn"]["stateHash"].get<std::string>();
        else if (applyQueryPayload.contains("fn") && applyQueryPayload["fn"].is_string())
            fnStateHashHex = applyQueryPayload["fn"].get<std::string>();
    } catch (...) {}
    CallbackCell cell;
    cell.applyId = applyReqHash;
    cell.fnStateHashHex = std::move(fnStateHashHex);
    callbackCells.push_back(std::move(cell));
    tracingCacheLog("createCallbackCell: applyReqHash=%s cells=%zu",
                    applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    callbackCells.size());
}

} // namespace nix

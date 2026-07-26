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
       ownFactSet. */
    if (attributionCell) {
        attributionCell->addFact(selectorHash, responseHash);
    }
    responseFor.emplace(selectorHash, responseHash);
    sessionRequestsTrie.insert(selectorHash);
    allRequestHashes.insert(selectorHash);

    /* #183: per-observation Ask insertion retired. Ask rows are
       written per-Selector-completion in logResult/logQueryResult
       from the completing cell + ancestor facts. The fact was already
       appended to attributionCell->facts above. */
    auto requestSetHash = decisionGraph->insertRequestSet({selectorHash});
    envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
    ObservationSet obsSet;
    obsSet.observations.push_back({fromStateHash, elementHash});
    envWalk.push_back(std::move(obsSet));
    prevQFactSetHash = envFactSetHash;
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

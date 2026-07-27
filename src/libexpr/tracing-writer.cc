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
       operation. */
    std::string queryTag = std::visit(
        [](const auto & q) -> std::string { return std::string(q.tag); }, query);
    tracingCacheLog(
        "logOuterObservation: subject=%s query=%s",
        describe(subject), queryTag);

    nlohmann::json queryJson = trace::toJson(query);
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
                          {"subject", describe(subject)}});
        recordProvenance(responseHash, "responseHash-d1",
                         {{"resultJson", resultJson},
                          {"selectorHash", selectorHash.to_string(HashFormat::Base16, false)}});
    }

    decisionGraph->insertRequest(selectorHash, jsonToCborString(queryJson));

    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), selectorHash, responseHash);
    auto factHash = elementHash;

    /* Dedup by (request, response). Duplicate would XOR-cancel. */
    if (!seenRequests.insert(factHash).second)
        return;

    /* #183: fact appends to attributionCell's fact set. Ask rows
       inserted per-Selector-completion. */
    if (attributionCell) {
        attributionCell->addFact(selectorHash, responseHash);
    }
    responseFor.emplace(selectorHash, responseHash);
    sessionRequestsTrie.insert(selectorHash);
    allRequestHashes.insert(selectorHash);
}

void TracingWriter::createCallbackCell(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;
    /* #184: reduced to inserting the apply-request payload into the
       Requests pool. The writer-side CallbackCell vector + SuppressApplyBoundary
       guard retired — cell.callbackState (populated by the caller) is
       the single source of truth for pending-payload storage. */
    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    if (provenanceEnabled())
        recordProvenance(applyReqHash, "applyRequestHash",
                         {{"applyQueryPayload", applyQueryPayload}});
    decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);
}

} // namespace nix

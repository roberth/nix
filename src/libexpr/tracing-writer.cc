#include "nix/expr/tracing-writer.hh"
#include "nix/expr/observation-set.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/trace-types.hh"

#include <set>

namespace nix {

namespace {

/* State-creep canonicalisation (see main doc's "state/observation-
   creep canonicalisation" note). Given an incoming outer observation
   whose request is a SelectorGetAttr with from=<SelectorCallbackApply
   hex>, look for an existing fact on the cell with the same predicate
   (name + CBApply.fn) and same response but different CBApply.argObsSet.
   If found, intersect the two obsSets (join in the precondition
   lattice = intersection under double inversion), construct a canonical
   getAttr referencing a CBApply with the intersected obsSet, insert its
   payload into the pool, remove the pre-existing fact, and return the
   canonical (reqHash, wasCanonicalised=true). Caller uses the returned
   reqHash for the subsequent addFact.

   Returns nullopt when no canonicalisation applies (either the
   incoming isn't the right shape, or no matching existing fact). */
struct CanonicaliseResult {
    Hash canonicalReqHash;
    Hash existingReqHashRemoved;
};

std::optional<CanonicaliseResult> tryStateCreepCanonicalise(
    TracingDecisionGraph & dg,
    const std::shared_ptr<const ArgCell> & cell,
    ref<const trace::Selector> incomingSelector,
    const Hash & incomingRespHash)
{
    if (!cell) return std::nullopt;
    /* Incoming must be getAttr(...) on CallbackApply(...). Everything else
       is out of scope for state-creep canonicalisation. */
    auto * incGA = std::get_if<trace::SelectorGetAttr>(&incomingSelector->node);
    if (!incGA) return std::nullopt;
    auto * incCBA = std::get_if<trace::SelectorCallbackApply>(&incGA->parent->node);
    if (!incCBA) return std::nullopt;

    auto incomingObsSet = dg.getObservationSet(incCBA->argObsSet);
    if (!incomingObsSet) return std::nullopt;

    /* Scan cell's facts for a matching predicate + response. Each
       existingReqHash keys an OuterValueRequest envelope in the Requests
       pool; the typed getRequest recovers the Selector for field comparison. */
    for (auto & [existingReqHash, existingEntry] : cell->facts) {
        if (existingEntry.response != incomingRespHash) continue;
        auto existingReq = dg.getRequest(existingReqHash);
        if (!existingReq) continue;
        auto * existingOVR = std::get_if<trace::OuterValueRequest>(&*existingReq);
        if (!existingOVR) continue;
        auto * exGA = std::get_if<trace::SelectorGetAttr>(&existingOVR->query->node);
        if (!exGA || exGA->name != incGA->name) continue;
        auto * exCBA = std::get_if<trace::SelectorCallbackApply>(&exGA->parent->node);
        if (!exCBA) continue;
        /* Same fn (identity via cachedHash), different argObsSet. */
        if (exCBA->parent->cachedHash != incCBA->parent->cachedHash) continue;
        if (exCBA->argObsSet == incCBA->argObsSet) continue;

        auto existingObsSet = dg.getObservationSet(exCBA->argObsSet);
        if (!existingObsSet) continue;

        /* Intersect: keep entries present in both, matched by
           (reqHash, responsePayload). */
        std::vector<TracingDecisionGraph::InlineFact> intersected;
        for (const auto & a : *incomingObsSet) {
            for (const auto & b : *existingObsSet) {
                if (a.reqHash == b.reqHash
                    && a.responsePayload == b.responsePayload) {
                    intersected.push_back(a);
                    break;
                }
            }
        }

        auto canonicalObsSetHash = dg.insertObservationSet(intersected);
        auto canonicalCbaSel = dg.selectorPool.intern(trace::SelectorCallbackApply{
            canonicalObsSetHash, incCBA->parent});
        dg.insertRequest(canonicalCbaSel->cachedHash,
                         jsonToCborString(trace::toJson(*canonicalCbaSel)));
        auto canonicalGetterSel = dg.selectorPool.intern(trace::SelectorGetAttr{
            incGA->name, canonicalCbaSel});
        /* Wrap the canonical getter in the OVR envelope and insert
           under the wrapped hash — cell.facts keys on wrapped hashes
           (see logOuterObservation). */
        trace::OuterValueRequest canonicalOuterReq{canonicalGetterSel};
        auto canonicalReqPayload = jsonToCborString(nlohmann::json(canonicalOuterReq));
        auto canonicalReqHash = TracingDecisionGraph::computeResponseHash(canonicalReqPayload);
        dg.insertRequest(canonicalReqHash, canonicalReqPayload);

        tracingCacheLog(
            "state-creep collapse: existing=%s incoming=%s -> canonical=%s (obsSet %s + %s -> %s)",
            existingReqHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
            incomingSelector->cachedHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
            canonicalReqHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
            exCBA->argObsSet.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
            incCBA->argObsSet.to_string(HashFormat::Base16, false).substr(0, 12).c_str(),
            canonicalObsSetHash.to_string(HashFormat::Base16, false).substr(0, 12).c_str());

        auto existingHashCopy = existingReqHash;
        cell->removeFact(existingHashCopy);
        return CanonicaliseResult{canonicalReqHash, existingHashCopy};
    }
    return std::nullopt;
}

} // namespace


void TracingWriter::logOuterObservation(
    ref<const trace::Selector> query,
    const trace::ResultVariant & result,
    std::string producerDesc,
    const std::shared_ptr<const ArgCell> & attributionCell)
{
    std::string queryTag = std::visit(
        [](const auto & q) -> std::string { return std::string(q.tag); }, query->node);
    tracingCacheLog(
        "logOuterObservation: producer=%s query=%s attributionCell=%p depth=%d facts_before=%zu%s",
        producerDesc, queryTag,
        (const void *) attributionCell.get(),
        attributionCell ? attributionCell->depth : -1,
        attributionCell ? attributionCell->facts.size() : 0,
        attributionCell && attributionCell->parent ? " (has parent)" : "");

    /* Wrap the Selector in an OuterValueRequest envelope so it takes
       its place in the Env-layer Request variant (design: outer probes
       are Env-layer Requests, alongside FileReadRequest/GetEnvRequest).
       reqHash keys the Requests pool + cell.facts as the wrapped
       request's hash, not the bare Selector's cachedHash. */
    trace::OuterValueRequest outerReq{query};
    nlohmann::json reqJson = outerReq;
    auto reqPayload = jsonToCborString(reqJson);
    auto reqHash = TracingDecisionGraph::computeResponseHash(reqPayload);

    nlohmann::json resultJson;
    std::visit([&](const auto & r) { resultJson = r; }, result);
    auto responsePayload = jsonToCborString(resultJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

    tracingCacheLog(
        "  reqHash=%s reqJSON=%s",
        reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        reqJson.dump());
    tracingCacheLog(
        "  respHash=%s respJSON=%s",
        responseHash.to_string(HashFormat::Base16, false).substr(0, 12),
        resultJson.dump());
    if (provenanceEnabled()) {
        recordProvenance(reqHash, "requestHash-d1",
                         {{"reqJson", reqJson},
                          {"producer", producerDesc}});
        recordProvenance(responseHash, "responseHash-d1",
                         {{"resultJson", resultJson},
                          {"reqHash", reqHash.to_string(HashFormat::Base16, false)}});
    }

    decisionGraph.insertRequest(reqHash, reqPayload);

    /* #183: fact appends to attributionCell's fact set. Ask rows
       inserted per-Selector-completion.
       #187 principle 9: outer probes ARE value probes — stamp with
       current barrier, then bump so the next value probe gets a
       distinct barrier group.

       Dedupe by (cell, reqHash) — cell.facts.addFact is idempotent
       per reqHash. Two cells legitimately want the same fact when
       the same probe is observed through both writer-side flows
       (e.g., walker-attempt's phantom callback firing on cell A and
       inner-cold's proper cell B for the same probe under pre-
       populated obs). Each cell's chain needs its own fact so its
       factSetHash reflects the actual observations attributed to
       it. The writer-global side effects below (responseFor,
       sessionRequestsTrie) are idempotent primitives, so writer-wide
       dedupe falls out for free. */
    if (attributionCell) {
        /* State-creep canonicalisation: if this fact has a getAttr
           request with from=CBApply and matches an existing fact by
           predicate+response, replace with the intersected-obsSet
           canonical form. See main doc's "state/observation-creep
           canonicalisation" note. */
        auto canonical = tryStateCreepCanonicalise(
            decisionGraph, attributionCell, query, responseHash);
        auto factReqHash = canonical ? canonical->canonicalReqHash : reqHash;
        auto barrier = peekBarrier();
        bool added = attributionCell->addFact(factReqHash, responseHash, barrier);
        if (added)
            bumpBarrier();
        if (canonical && canonical->canonicalReqHash != canonical->existingReqHashRemoved) {
            /* Record for Ask-time alt stamping: any Ask carrying
               canonicalReqHash gets a companion alt with
               existingReqHashRemoved substituted in. */
            canonicalReplacements.emplace(
                canonical->canonicalReqHash,
                canonical->existingReqHashRemoved);
        }
    } else {
        /* No attribution cell — bump barrier anyway so ordering
           within the writer stays monotonic. */
        bumpBarrier();
    }
    responseFor.emplace(reqHash, responseHash);
    sessionRequestsTrie.insert(reqHash);
}

void TracingWriter::createCallbackCell(const nlohmann::json & applyQueryPayload)
{
    /* Insert the apply-request payload into the Requests pool.
       Pending-payload storage lives on cell.callbackState (populated
       by the caller). */
    auto applyReqHash = trace::tracingHash(applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    if (provenanceEnabled())
        recordProvenance(applyReqHash, "applyRequestHash",
                         {{"applyQueryPayload", applyQueryPayload}});
    decisionGraph.insertRequest(applyReqHash, applyPayloadCbor);
}

} // namespace nix

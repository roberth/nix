#include "nix/expr/tracing-writer.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix {

void TracingWriter::flushPendingAmbient()
{
    if (!decisionGraph)
        return;

    /* Per the via-Asks design: each fact's `from` is the subject's
       content id at the precondition of the Asks edge it belongs to.
       For the default single-edge case, precondition is the empty
       factset, so `from = cidasks::contentIdAfter(subject, {})`.

       Pass A: insert deferred Requests at their natural keys. */
    for (auto & req : pendingRequests) {
        if (req.keyPlaceholder) {
            /* Sidecar: keyPlaceholder is the local arg's
               positional initial content id. Insert at that key. */
            auto key = Hash::parseNonSRIUnprefixed(*req.keyPlaceholder, HashAlgorithm::SHA256);
            decisionGraph->insertRequest(key, jsonToCborString(req.payload));
        } else {
            /* Apply Q: key = hash of payload itself. */
            auto key = hashString(HashAlgorithm::SHA256, req.payload.dump());
            decisionGraph->insertRequest(key, jsonToCborString(req.payload));
        }
    }

    /* Partition facts by layer:
        - Depth-1 facts (depth2ApplyId == zero) fold into v13FactSet
          as before; we build a single-edge walk for them since v13
          uses XOR-fold semantics over the whole set.
        - Depth-2 facts group by cb-apply id. Each group becomes a
          multi-edge walk (= one Asks edge per fact, in observation
          order) with each fact's `from` substituted at its own
          edgeIndex via cidasks. The resulting chained AmbientAsks
          rows let the walker advance probe-by-probe and let sibling
          cb-applies fork at their first divergent response. */
    std::vector<PendingFact *> depth1Facts;
    std::map<Hash, std::vector<PendingFact *>> depth2FactsByApply;
    for (auto & pf : pendingFacts) {
        if (pf.depth2ApplyId == Hash(HashAlgorithm::SHA256))
            depth1Facts.push_back(&pf);
        else
            depth2FactsByApply[pf.depth2ApplyId].push_back(&pf);
    }

    auto rewriteFromInQuery = [](nlohmann::json & queryJson, const std::string & fromHex) {
        if (queryJson.is_object() && queryJson.contains("params")) {
            auto & params = queryJson["params"];
            if (params.is_object() && params.contains("from"))
                params["from"] = fromHex;
        }
    };

    /* Depth-1: stamp every ambient fact's `from` at the cb_arg
       root's static CDI (= contentIdAfter at the empty walk).
       Per the per-arg-completion doc's Fix A, dropping cross-flush
       evolution removes the writer/walker edgeIndex-alignment
       requirement: the walker computes the same static CID at edge
       0 unconditionally, so resolveCdiId always matches the cell
       chain regardless of how many prior flushes preceded.
       Cumulative dependency (Foundational #9) is preserved — facts
       still accumulate; only the per-fact `from` encoding becomes
       stable. */
    for (auto * pf : depth1Facts) {
        auto [path, roots] = cidasks::pathAndRootsFromSubject(pf->subject);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(roots.size());
        for (auto & root : roots) {
            auto cid = cidasks::contentIdAfter(root, pf->inheritedScope, {});
            fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
        }
        std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();

        std::string queryTag = std::visit(
            [](const auto & q) -> std::string { return std::string(q.tag); }, pf->query);
        tracingCacheLog(
            "flush d1 fact: subject=%s query=%s from=%s path=%zu fromCIDs=%zu",
            cidasks::describe(pf->subject), queryTag, fromHex.substr(0, 12),
            path.steps.size(), fromCIDs.size());

        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, pf->query);
        rewriteFromInQuery(queryJson, fromHex);
        if (!path.steps.empty())
            queryJson["params"]["path"] = path;
        if (!fromCIDs.empty())
            queryJson["params"]["fromCIDs"] = fromCIDs;
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, pf->result);

        auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
        auto responsePayload = jsonToCborString(resultJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        decisionGraph->insertLocalResponse(queryHash, responsePayload);

        /* Dedupe by (request, response). See logResponse. */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({queryHash, responseHash});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, queryHash, responseHash);
            responseFor.emplace(queryHash, responseHash);
            allRequestsTrie.insert(queryHash);
            if (allRequestHashes.insert(queryHash).second)
                pendingNewRequests.push_back(queryHash);
        }
    }

    /* Depth-2: per cb-apply, build the AmbientAsks chain. Each
       probe's `from` uses the local root's STATIC cdi (same Fix A
       framing as the depth-1 path above); chain progression is
       captured by cumulativeFactSet through AmbientAsks edges, not
       by evolving the per-fact `from` field. */
    auto emptySet = TracingDecisionGraph::emptySetHash();
    for (auto & [applyId, group] : depth2FactsByApply) {
        Hash cumulativeFactSet = emptySet;
        for (size_t i = 0; i < group.size(); ++i) {
            auto * pf = group[i];
            /* Stamp each probe's `from` at the local root's STATIC
               cdi (= contentIdAfter at empty walk). The d2 chain's
               AmbientAsks `(fromFactSet, requestSet) → toFactSet`
               still captures probe order/structure via cumulativeFactSet
               progression; encoding the local's evolution into each
               fact's `from` is redundant and breaks walker alignment
               (= same Fix A reasoning as depth-1: walker computes
               static cdi for the local subject, so evolved-cdi
               `from` values can't be resolved through the cell
               chain). */
            auto [path, roots] = cidasks::pathAndRootsFromSubject(pf->subject);
            std::vector<trace::QueryLeaf> fromCIDs;
            fromCIDs.reserve(roots.size());
            for (auto & root : roots) {
                auto cid = cidasks::contentIdAfter(root, pf->inheritedScope, {});
                fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();

            std::string queryTag = std::visit(
                [](const auto & q) -> std::string { return std::string(q.tag); }, pf->query);
            tracingCacheLog(
                "flush d2 fact: applyId=%s i=%zu subject=%s query=%s from=%s path=%zu fromCIDs=%zu",
                applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                i, cidasks::describe(pf->subject), queryTag, fromHex.substr(0, 12),
                path.steps.size(), fromCIDs.size());

            nlohmann::json queryJson;
            std::visit([&](const auto & q) { queryJson = q; }, pf->query);
            rewriteFromInQuery(queryJson, fromHex);
            if (!path.steps.empty())
                queryJson["params"]["path"] = path;
            if (!fromCIDs.empty())
                queryJson["params"]["fromCIDs"] = fromCIDs;
            nlohmann::json resultJson;
            std::visit([&](const auto & r) { resultJson = r; }, pf->result);

            auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
            auto responsePayload = jsonToCborString(resultJson);
            auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

            decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
            decisionGraph->insertLocalResponse(queryHash, responsePayload);

            auto requestSet = decisionGraph->insertRequestSet({queryHash});
            auto toFactSet = TracingDecisionGraph::xorFactIntoHash(
                cumulativeFactSet, queryHash, responseHash);
            decisionGraph->insertAmbientAsks(cumulativeFactSet, requestSet, toFactSet);
            cumulativeFactSet = toFactSet;
        }
    }

    pendingFacts.clear();
    pendingRequests.clear();
}

} // namespace nix

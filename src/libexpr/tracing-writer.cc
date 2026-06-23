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

    /* Depth-1: single-edge walk (preserve v13 XOR-fold semantics for
       input tracing). */
    cidasks::Edge d1Edge;
    for (auto * pf : depth1Facts)
        d1Edge.facts.push_back(cidasks::factFromQR(pf->query, pf->result));
    std::vector<cidasks::Edge> d1Walk{std::move(d1Edge)};

    auto rewriteFromInQuery = [](nlohmann::json & queryJson, const std::string & fromHex) {
        if (queryJson.is_object() && queryJson.contains("params")) {
            auto & params = queryJson["params"];
            if (params.is_object() && params.contains("from"))
                params["from"] = fromHex;
        }
    };

    for (auto * pf : depth1Facts) {
        auto fromCdi = cidasks::contentIdAt(pf->subject, pf->inheritedScope, d1Walk, /*edgeIndex=*/ 0);
        auto fromHex = fromCdi.to_string(HashFormat::Base16, false);

        std::string queryTag = std::visit(
            [](const auto & q) -> std::string { return std::string(q.tag); }, pf->query);
        tracingCacheLog(
            "flush d1 fact: subject=%s query=%s from=%s",
            cidasks::describe(pf->subject), queryTag, fromHex.substr(0, 12));

        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, pf->query);
        rewriteFromInQuery(queryJson, fromHex);
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
        }
    }

    /* Depth-2: per cb-apply, build a multi-edge walk incrementally
       so each fact's substituted `from` is computed against prior
       SUBSTITUTED facts (= the walker sees the same chain it
       constructs probe-by-probe). Without this, the writer's
       contentIdAt evaluation uses each fact's ORIGINAL `from`
       (the constant `localId()` recorded by TracingLocalObject),
       which mismatches the walker's evolved cdi and breaks the
       cidasks filter for derived children. */
    auto emptySet = TracingDecisionGraph::emptySetHash();
    for (auto & [applyId, group] : depth2FactsByApply) {
        std::vector<cidasks::Edge> walk;
        walk.reserve(group.size());

        Hash cumulativeFactSet = emptySet;
        for (size_t i = 0; i < group.size(); ++i) {
            auto * pf = group[i];
            /* `from` at this fact's edge precondition, against the
               substituted-so-far walk. */
            auto fromCdi = cidasks::contentIdAt(pf->subject, pf->inheritedScope, walk, /*edgeIndex=*/ i);
            auto fromHex = fromCdi.to_string(HashFormat::Base16, false);

            std::string queryTag = std::visit(
                [](const auto & q) -> std::string { return std::string(q.tag); }, pf->query);
            tracingCacheLog(
                "flush d2 fact: applyId=%s i=%zu subject=%s query=%s from=%s",
                applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                i, cidasks::describe(pf->subject), queryTag, fromHex.substr(0, 12));

            nlohmann::json queryJson;
            std::visit([&](const auto & q) { queryJson = q; }, pf->query);
            rewriteFromInQuery(queryJson, fromHex);
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

            /* Append the SUBSTITUTED fact (= with fromHash = fromCdi)
               so the next iteration's contentIdAt sees the chain the
               walker reconstructs. */
            cidasks::Edge edge;
            auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), queryHash, responseHash);
            edge.facts.push_back({fromCdi, elementHash});
            walk.push_back(std::move(edge));
        }
    }

    pendingFacts.clear();
    pendingRequests.clear();
}

} // namespace nix

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

       The walk is built from pendingFacts as a single edge; each
       fact's `from` is computed against the empty precondition
       (edgeIndex = 0). Multi-edge handling (Patricia split, per-edge
       precondition factsets) is the natural follow-up: build a walk
       with multiple edges and pass the correct edgeIndex per fact.

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

    /* Build a single-edge walk from pendingFacts. Each fact's
       per-edge precondition is edgeIndex 0 (= the empty factset). */
    cidasks::Edge edge;
    for (auto & pf : pendingFacts)
        edge.facts.push_back(cidasks::factFromQR(pf.query, pf.result));
    std::vector<cidasks::Edge> walk{std::move(edge)};

    /* Pass B: rewrite each fact's `from` to its subject's content
       id at this Asks edge's precondition (using the fact's own
       inheritedScope so sibling cached calls get distinct ids),
       then insert. */
    for (auto & fact : pendingFacts) {
        auto fromCdi = cidasks::contentIdAt(fact.subject, fact.inheritedScope, walk, /*edgeIndex=*/ 0);
        auto fromHex = fromCdi.to_string(HashFormat::Base16, false);

        std::string queryTag = std::visit(
            [](const auto & q) -> std::string { return std::string(q.tag); }, fact.query);
        tracingCacheLog(
            "flush fact: subject=%s query=%s from=%s",
            cidasks::describe(fact.subject), queryTag, fromHex.substr(0, 12));

        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, fact.query);
        if (queryJson.is_object() && queryJson.contains("params")) {
            auto & params = queryJson["params"];
            if (params.is_object() && params.contains("from"))
                params["from"] = fromHex;
        }
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, fact.result);

        auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
        auto responsePayload = jsonToCborString(resultJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        decisionGraph->insertResponse(queryHash, responsePayload);

        if (seenRequests.insert(queryHash).second) {
            v13FactSet.push_back({queryHash, responseHash});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, queryHash, responseHash);
            responseFor.emplace(queryHash, responseHash);
            allRequestsTrie.insert(queryHash);
        }
    }

    pendingFacts.clear();
    pendingRequests.clear();
}

} // namespace nix

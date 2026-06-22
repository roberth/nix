#include "nix/expr/tracing-writer.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix {

void TracingWriter::flushPendingAmbient()
{
    if (!decisionGraph)
        return;

    /* Under the design in
       doc/design/tracing-eval-cache-content-identity-via-asks.md,
       the default single-Asks-edge case is precondition = empty
       factset, and facts emit with from = subject's positional
       initial content id. The recorder doesn't substitute; pool
       entries land at their natural reqHashes.

       Multi-edge cases (Patricia split etc.) — where per-edge
       rewriting of `from` would matter — are deferred. The
       walker's record() integration still uses the v13 fast path
       which produces a single edge per recording. */

    /* Pass A: insert deferred Requests at their natural keys. */
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

    /* Pass B: insert facts at qH(query) and update v13FactSet. */
    for (auto & fact : pendingFacts) {
        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, fact.query);
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

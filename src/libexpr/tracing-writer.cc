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

    /* Depth-1: this flush's ambient facts form ONE edge appended to
       the persistent `d1CidasksWalk` chain (= principles 3/5/7). Each
       fact's `from` substitutes against `d1CidasksWalk[walk.size()]`
       — the precondition for the new edge — so per-arg roots evolve
       across logResults. Env/file facts (= logged via `logResponse`)
       are tracked separately in `pendingNewRequests` and end up in
       the same per-Q Asks edge as this flush's ambient facts at
       logResult time (= AmbientQueries are depth-1 like file reads;
       both are observations on outer state). */
    size_t d1EdgeIndex = d1CidasksWalk.size();
    cidasks::Edge d1NewEdge;

    for (auto * pf : depth1Facts) {
        /* Per-arg with multi-root (= task #87): `from` is the first
           cb_arg's CDI; `fromCIDs[]` carries all cb_arg roots reached
           via the subject tree (= fn-root and arg-root for applies);
           `path` encodes the access expression that walks from
           fromCIDs[0] to the observed subject. */
        auto [path, roots] = cidasks::pathAndRootsFromSubject(pf->subject);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(roots.size());
        for (auto & root : roots) {
            auto cid = cidasks::contentIdAt(root, pf->inheritedScope, d1CidasksWalk, d1EdgeIndex);
            fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
        }
        std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();
        auto fromCdi = fromCIDs.empty()
            ? Hash(HashAlgorithm::SHA256)
            : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

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

        /* Append the substituted fact to the new d1 cidasks edge so
           later logResults' contentIdAt sees it in the own-loop. */
        auto elementHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        d1NewEdge.facts.push_back({fromCdi, elementHash});

        /* Dedupe by (request, response). See logResponse. */
        auto factHash = elementHash;
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
    if (!d1NewEdge.facts.empty())
        d1CidasksWalk.push_back(std::move(d1NewEdge));

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
            /* Per-arg with multi-root (task #87): compute fromCIDs +
               path; substitute `from = fromCIDs[0]` and add
               fromCIDs+path to the JSON. Multi-root is needed for
               higher-order applies where fn and arg come from
               different cb_args. */
            auto [path, roots] = cidasks::pathAndRootsFromSubject(pf->subject);
            std::vector<trace::QueryLeaf> fromCIDs;
            fromCIDs.reserve(roots.size());
            for (auto & root : roots) {
                auto cid = cidasks::contentIdAt(root, pf->inheritedScope, walk, /*edgeIndex=*/ i);
                fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();
            auto fromCdi = fromCIDs.empty()
                ? Hash(HashAlgorithm::SHA256)
                : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

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

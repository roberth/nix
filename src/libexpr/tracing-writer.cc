#include "nix/expr/tracing-writer.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

namespace nix {

void TracingWriter::flushPendingAmbient(bool finalize)
{
    if (!decisionGraph)
        return;

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
    pendingRequests.clear();

    /* Depth-1 facts (= ambient observations on outer state) fold into
       v13FactSet immediately; we build a single edge per flush appended
       to d1CidasksWalk for cidasks own-fold evolution. Depth-2 facts
       group by cb-apply id and are NOT folded into v13FactSet — they
       live only in AmbientAsks rows, processed at `finalize=true`
       (= logResult) when each cb-apply's chain is known to be complete. */

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
       across logResults. */
    size_t d1EdgeIndex = d1CidasksWalk.size();
    cidasks::Edge d1NewEdge;

    for (auto & pf : pendingDepth1Facts) {
        /* Per-arg with multi-root: `from` is the first cb_arg's CDI;
           `fromCIDs[]` carries all cb_arg roots reached via the
           subject tree; `path` encodes the access expression that
           walks from fromCIDs[0] to the observed subject. */
        auto [path, roots] = cidasks::pathAndRootsFromSubject(pf.subject);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(roots.size());
        for (auto & root : roots) {
            auto cid = cidasks::contentIdAt(root, pf.inheritedScope, d1CidasksWalk, d1EdgeIndex);
            fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
        }
        std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();
        auto fromCdi = fromCIDs.empty()
            ? Hash(HashAlgorithm::SHA256)
            : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

        std::string queryTag = std::visit(
            [](const auto & q) -> std::string { return std::string(q.tag); }, pf.query);
        tracingCacheLog(
            "flush d1 fact: subject=%s query=%s from=%s path=%zu fromCIDs=%zu",
            cidasks::describe(pf.subject), queryTag, fromHex.substr(0, 12),
            path.steps.size(), fromCIDs.size());

        nlohmann::json queryJson;
        std::visit([&](const auto & q) { queryJson = q; }, pf.query);
        rewriteFromInQuery(queryJson, fromHex);
        if (!path.steps.empty())
            queryJson["params"]["path"] = path;
        if (!fromCIDs.empty())
            queryJson["params"]["fromCIDs"] = fromCIDs;
        nlohmann::json resultJson;
        std::visit([&](const auto & r) { resultJson = r; }, pf.result);

        auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
        auto responsePayload = jsonToCborString(resultJson);
        auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        decisionGraph->insertLocalResponse(queryHash, responsePayload);

        /* Append the substituted fact to the new d1 cidasks edge so
           later logResults' contentIdAt sees it in the own-loop. */
        auto elementHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        d1NewEdge.observations.push_back({fromCdi, elementHash});

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
    if (!d1NewEdge.observations.empty()) {
        d1CidasksWalk.push_back(std::move(d1NewEdge));
        tracingCacheLog("writer d1CidasksWalk += 1 -> %zu (obs=%zu)",
                        d1CidasksWalk.size(),
                        d1CidasksWalk.back().observations.size());
    }
    pendingDepth1Facts.clear();

    if (!finalize) {
        /* Intermediate flush: depth-2 facts stay buffered until
           their apply boundary is finalised at logResult. The cb-apply
           boundary's d=2 chain may not be complete yet (= outer is
           still probing the local), so we can't compute AmbientResult
           here without risking an incomplete chain. */
        return;
    }

    /* Finalize: close the final depth-1 chunk's perQAsksEdge BEFORE
       processing apply boundaries. Without this the d1 facts get
       bundled with the first apply boundary's perQAsksEdge — walker
       would still XOR-fold the same elementHashes (commutative) but
       intermediate Asks(Q, cur) lookups during walk() expect each
       edge to land at a recorded cur, and bundling shifts those
       positions. Separating gives walker the same per-edge curs the
       writer used at record time. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        perQAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        tracingCacheLog("finalize: final d1 Asks edge from=%s rs-size=%zu (perQ=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        perQAsksEdges.size());
        prevQFactSetHash = v13FactSetHash;
        pendingNewRequests.clear();
    }

    /* Finalize pass: process each buffered cb-apply boundary in the
       order recorded. For each boundary:
        1. Look up its d=2 group (may be empty if no probes happened).
        2. Build the d=2 chain via incremental contentIdAt
           substitution — each fact's `from` is computed against the
           chain prefix the walker reconstructs probe-by-probe.
        3. The terminal `cumulativeFactSet` IS the AmbientResult
           (via-Asks §"Recording (depth-2)").
        4. Synthesize the d=1 apply Fact (applyReqHash, AmbientResult);
           fold into v13FactSet and append a synthetic edge to
           d1CidasksWalk (fromHash=Hash(0), elementHash=factHash) —
           the apply boundary contributes to cur but not to any
           subject's own-fold (= phantom edge for walk-index sync).
        5. Add applyReqHash to pendingNewRequests so the trailing
           splitFlush's perQAsksEdge close picks it up.

       Reverse-nested order is acceptable but unnecessary: AmbientResult
       for one apply doesn't depend on another's chain — each group is
       independent because their d=2 chains share no facts.

       Chain rooting: each cb-apply's d=2 chain is rooted at
       `boundary.applyRequestHash` (= the natural hash of the apply
       payload) rather than `emptySetHash()`. This makes each
       cb-apply's chain its own subtree in AmbientAsks, so the
       walker can pick the right chain by knowing the apply Fact's
       reqHash — no ambiguity when multiple cb-applies are recorded.
       via-Asks discrimination via inherited CDI propagation still
       flows through: different inherited CDIs → different argId →
       different applyReqHash → different chain root. Same-shape
       collapse still holds for identical cb-applies (= same fnId,
       same argId, same observations → same applyReqHash → same
       chain). The loss is atom sharing across cb-applies whose
       chains happen to coincide but whose applyReqHashes differ — a
       storage trade-off, not correctness. */
    /* Same-shape collapse happens automatically via content-addressed
       AmbientAsks rows: two boundaries with the same applyId and the
       same probes produce identical rows that INSERT OR IGNORE
       deduplicates; v13FactSet's seenRequests deduplicates the
       synthesised d=1 Fact too. Process each boundary independently
       so its own pendingDepth2FactsByApply slice gets folded into
       its own chain — collapsing the boundary list before processing
       conflates probes across boundaries and the resulting
       AmbientResult doesn't match what a per-call walker would
       reproduce. */
    for (auto & boundary : pendingApplyBoundaries) {
        auto & group = boundary.facts;

        std::vector<cidasks::Edge> walk;
        walk.reserve(group.size());

        Hash cumulativeFactSet = boundary.applyRequestHash;
        for (size_t i = 0; i < group.size(); ++i) {
            auto & pf = group[i];
            /* Per-arg with multi-root: compute fromCIDs + path against
               the prefix-walk built so far (so the walker observes the
               same evolved CDI at each step). */
            auto [path, roots] = cidasks::pathAndRootsFromSubject(pf.subject);
            std::vector<trace::QueryLeaf> fromCIDs;
            fromCIDs.reserve(roots.size());
            for (auto & root : roots) {
                auto cid = cidasks::contentIdAt(root, pf.inheritedScope, walk, /*edgeIndex=*/ i);
                fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();
            auto fromCdi = fromCIDs.empty()
                ? Hash(HashAlgorithm::SHA256)
                : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

            std::string queryTag = std::visit(
                [](const auto & q) -> std::string { return std::string(q.tag); }, pf.query);
            tracingCacheLog(
                "flush d2 fact: applyId=%s i=%zu subject=%s query=%s from=%s path=%zu fromCIDs=%zu",
                boundary.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                i, cidasks::describe(pf.subject), queryTag, fromHex.substr(0, 12),
                path.steps.size(), fromCIDs.size());

            nlohmann::json queryJson;
            std::visit([&](const auto & q) { queryJson = q; }, pf.query);
            rewriteFromInQuery(queryJson, fromHex);
            if (!path.steps.empty())
                queryJson["params"]["path"] = path;
            if (!fromCIDs.empty())
                queryJson["params"]["fromCIDs"] = fromCIDs;
            nlohmann::json resultJson;
            std::visit([&](const auto & r) { resultJson = r; }, pf.result);

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

            /* Append the SUBSTITUTED fact so the next iteration's
               contentIdAt sees the chain the walker reconstructs. */
            cidasks::Edge edge;
            auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), queryHash, responseHash);
            edge.observations.push_back({fromCdi, elementHash});
            walk.push_back(std::move(edge));
        }

        /* AmbientResult = terminal of the d=2 chain (per via-Asks
           §"Recording (depth-2)"). For an empty chain, this is the
           XOR-fold identity (= empty factSet hash) — "no probes
           happened" is a meaningful AmbientResult that any cb-apply
           with the same applyId and no probes will reconstruct. */
        auto ambientResult = cumulativeFactSet;
        tracingCacheLog(
            "finalize apply boundary: applyId=%s probes=%zu AmbientResult=%s",
            boundary.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
            group.size(),
            ambientResult.to_string(HashFormat::Base16, false).substr(0, 12));

        /* Synthesize the d=1 apply Fact at (applyReqHash, AmbientResult).
           Fold into v13FactSet symmetrically with logResponse. The
           d1CidasksWalk gets one phantom edge with fromHash=Hash(0)
           (= walk-advance marker; doesn't contribute to any subject's
           own-fold per the cidasks formula's `f.fromHash == myCidAtK`
           check, which can't match because no subject CDI equals 0). */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), boundary.applyRequestHash, ambientResult);
        if (seenRequests.insert(factHash).second) {
            v13FactSet.push_back({boundary.applyRequestHash, ambientResult});
            v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                v13FactSetHash, boundary.applyRequestHash, ambientResult);
            responseFor.emplace(boundary.applyRequestHash, ambientResult);
            allRequestsTrie.insert(boundary.applyRequestHash);
            if (allRequestHashes.insert(boundary.applyRequestHash).second)
                pendingNewRequests.push_back(boundary.applyRequestHash);
        }

        cidasks::Edge applyEdge;
        applyEdge.observations.push_back({
            Hash(HashAlgorithm::SHA256), // fromHash = 0: walk-advance marker
            factHash,                    // elementHash = factElementHash(reqHash, AmbientResult)
        });
        d1CidasksWalk.push_back(std::move(applyEdge));

        /* Each finalised boundary gets its own perQAsksEdge — mirrors
           the original synchronous markApplyBoundary's per-ε closure
           so the walker sees one Asks edge per cb-apply Fact. */
        if (!pendingNewRequests.empty()) {
            auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
            perQAsksEdges.push_back({prevQFactSetHash, requestSetHash});
            tracingCacheLog("finalize: ε Asks edge from=%s rs-size=%zu (perQ=%zu)",
                            prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                            pendingNewRequests.size(),
                            perQAsksEdges.size());
            prevQFactSetHash = v13FactSetHash;
            pendingNewRequests.clear();
        }
    }

    pendingApplyBoundaries.clear();
}

void TracingWriter::splitFlush(bool finalize)
{
    if (!decisionGraph)
        return;

    /* Process pending ambient observations into one new Asks edge
       transition (= advances v13FactSetHash and d1CidasksWalk when
       observations are present). At finalize=true this also computes
       AmbientResults for each buffered cb-apply boundary and folds
       the synthetic d=1 apply Facts in. */
    flushPendingAmbient(finalize);

    /* Materialise the perQAsksEdge boundary so the trailing logResult
       (or a later splitFlush) inserts an Asks(Q, fromFactSet, RS) row
       for this transition into Q's namespace. Skip-on-empty is
       deliberate: an edge with no requests has nothing to advance, so
       neither writer's d1CidasksWalk nor walker's cidasksWalk grows
       for it (= principles 4 + 7). */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        perQAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        tracingCacheLog("splitFlush: new Asks edge from=%s rs-size=%zu (perQ=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        perQAsksEdges.size());
        prevQFactSetHash = v13FactSetHash;
        pendingNewRequests.clear();
    }
}

void TracingWriter::markApplyBoundary(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;

    /* Close any preceding observations into their own Asks edge
       (= β1). The intermediate flush only drains depth-1 facts; any
       depth-2 facts from prior unfinalised cb-applies (= nested case)
       stay buffered, waiting for their own boundary's finalize. */
    splitFlush(/*finalize=*/ false);

    /* Insert the apply Request payload into the CAS pool now — its
       hash is known immediately, payload doesn't depend on AmbientResult.
       The walker needs this entry by the time it dispatches the apply
       Fact at warm replay. */
    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);

    /* Buffer the boundary. The synthetic d=1 apply Fact's respHash
       is the AmbientResult = terminal of this cb-apply's d=2 chain,
       only known after the body finishes. flushPendingAmbient at
       logResult walks pendingApplyBoundaries in order and finalises
       each one. */
    pendingApplyBoundaries.push_back({applyReqHash, applyReqHash});
    tracingCacheLog("markApplyBoundary: buffered (applyReqHash=%s, pendingBoundaries=%zu)",
                    applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    pendingApplyBoundaries.size());
}

} // namespace nix

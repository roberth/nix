#include "nix/expr/tracing-writer.hh"
#include "nix/expr/content-identity-via-asks.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <set>

namespace nix {

void TracingWriter::flushPendingAmbient(bool finalize)
{
    if (!decisionGraph)
        return;

    /* Pass A: insert deferred Requests at their natural keys. */
    for (auto & req : pendingRequests) {
        if (req.keyPlaceholder) {
            /* Sidecar: keyPlaceholder is the local arg's
               positional initial scope state id. Insert at that key. */
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
       across logResults.

       Under the 1:1 alignment invariant, the new edge is NOT pushed
       here; it's staged in `pendingD1Edge` for `splitFlush` to push
       paired with the corresponding perQAsksEdge. This keeps
       writer.d1CidasksWalk.size() == perQAsksEdges.size() at every
       transition. */
    size_t d1EdgeIndex = d1CidasksWalk.size();
    cidasks::Edge & d1NewEdge = pendingD1Edge;
    d1NewEdge = {};
    /* Per-edge dedup of observations by elementHash. An Asks edge is a
       set, not a list — XOR-folding the same observation twice cancels
       its contribution to cur. The walker dispatches each unique
       observation once, so the writer must too. */
    std::set<Hash> d1NewEdgeSeen;

    for (auto & pf : pendingDepth1Facts) {
        /* Per-arg with multi-root: `from` is the first cb_arg's argStateId;
           `fromCIDs[]` carries all cb_arg roots reached via the
           subject tree; `path` encodes the access expression that
           walks from fromCIDs[0] to the observed subject. */
        auto [path, roots] = cidasks::pathAndRootsFromSubject(pf.subject);
        std::vector<trace::QueryLeaf> fromCIDs;
        fromCIDs.reserve(roots.size());
        for (auto & root : roots) {
            auto cid = cidasks::scopeStateIdAt(root, pf.inheritedScope, d1CidasksWalk, d1EdgeIndex);
            fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
            /* Buffer stamp site for SubjectStampSites — drained at
               logResult with the current Q. Subject-content Merkle
               hash = scopeStateIdAt(subject, 0, {}, 0). */
            auto subjectHash = cidasks::scopeStateIdAt(root, Hash(HashAlgorithm::SHA256), {}, 0);
            pendingStampSites.push_back({cid, d1EdgeIndex, pf.inheritedScope, subjectHash});
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

        /* Diff-ready logging: render the exact bytes that fed reqHash and
           respHash (= reqJson.dump() and resultJson.dump()). Compare these
           between cold's flush here and warm's `dispatch ambient:` log to
           isolate which (q, r) pair differs and why curs diverge. */
        tracingCacheLog(
            "  reqHash=%s reqJSON=%s",
            queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
            queryJson.dump());
        tracingCacheLog(
            "  respHash=%s respJSON=%s",
            responseHash.to_string(HashFormat::Base16, false).substr(0, 12),
            resultJson.dump());

        decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
        decisionGraph->insertLocalResponse(queryHash, responsePayload);

        /* Append the substituted fact to the new d1 cidasks edge so
           later logResults' scopeStateIdAt sees it in the own-loop.

           Per-edge dedup by elementHash: an Asks edge is a SET of
           observations (per the design's principle 4), not a list. The
           walker's `commitEdge` already dedups by edge-fingerprint, so
           if the writer leaves duplicates in the edge here the XOR-fold
           on each side computes different cumulative cur — when a fact
           appears EVEN times here it cancels, ODD it contributes; the
           walker's single contribution per unique fact then mismatches.
           This used to be papered over by symmetric over-recording
           where every observation type fired an even number of times
           and canceled together; WHNF memoization (which records once
           per Object instance) broke that symmetry by making one
           observation appear once while siblings still fire many.
           Dedup makes the writer match the walker independent of how
           often each method is called. */
        auto elementHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), queryHash, responseHash);
        if (d1NewEdgeSeen.insert(elementHash).second)
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
    /* d1NewEdge is staged in pendingD1Edge — splitFlush pushes it
       paired with the perQAsksEdge so the 1:1 alignment holds. */
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
        /* 1:1 alignment: push the staged d1 edge alongside the
           perQAsksEdge. The d1 edge may be empty (= file-read-only
           Asks edge with no ambient observations) — still pushed so
           the indices match. */
        d1CidasksWalk.push_back(std::move(pendingD1Edge));
        pendingD1Edge = {};
        tracingCacheLog("finalize: final d1 Asks edge from=%s rs-size=%zu (perQ=%zu d1=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        perQAsksEdges.size(),
                        d1CidasksWalk.size());
        prevQFactSetHash = v13FactSetHash;
        pendingNewRequests.clear();
    }

    /* Finalize pass: process each buffered cb-apply boundary in the
       order recorded. For each boundary:
        1. Look up its d=2 group (may be empty if no probes happened).
        2. Build the d=2 chain via incremental scopeStateIdAt
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
       via-Asks discrimination via inherited argStateId propagation still
       flows through: different inherited argStateIds → different argId →
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
       reproduce.

       Chronological ε insertion: each boundary's ε perQAsksEdge is
       INSERTED at boundary.insertionIndex (= captured at
       markApplyBoundary time, AFTER splitFlush(false) drained the
       pre-boundary d=1 chunk), not appended at the end. This puts
       ε BEFORE its body's d=1 facts in walker dispatch order. Each
       insertion shifts subsequent indices by 1, tracked via `shift`.
       Each ε's elementHash propagates into all subsequent
       perQAsksEdges' fromFactSetHash (= walker's cur advances by
       ε at that position). priorEpsilonAccum accumulates earlier
       ε contributions so each new ε's own fromFactSetHash reflects
       all prior ε contributions to its left. */
    size_t shift = 0;
    Hash priorEpsilonAccum(HashAlgorithm::SHA256);
    for (auto & boundary : pendingApplyBoundaries) {
        auto & group = boundary.facts;

        /* Helper: stamp the i-th fact and emit Request/LocalResponse
           into the pool. AmbientAsks is inserted iff `withAmbientAsks`
           is true. Returns (cumulativeFactSet, walk-edge-to-append).
           Used both for first-finalize processing (with AmbientAsks)
           and for late-d2-obs re-processing (without AmbientAsks —
           see commentary at the "late probe" branch below). */
        auto stampAndEmit = [&](size_t i, const std::vector<cidasks::Edge> & walk,
                                Hash cumulativeFactSet, bool withAmbientAsks)
            -> std::pair<Hash, cidasks::Edge>
        {
            auto & pf = group[i];
            auto [path, roots] = cidasks::pathAndRootsFromSubject(pf.subject);
            std::vector<trace::QueryLeaf> fromCIDs;
            fromCIDs.reserve(roots.size());
            for (auto & root : roots) {
                auto cid = cidasks::scopeStateIdAt(root, pf.inheritedScope, walk, /*edgeIndex=*/ i);
                fromCIDs.emplace_back(cid.to_string(HashFormat::Base16, false));
                /* SubjectStampSites: d=2 site. */
                auto subjectHash = cidasks::scopeStateIdAt(root, Hash(HashAlgorithm::SHA256), {}, 0);
                pendingStampSites.push_back({cid, i, pf.inheritedScope, subjectHash});
            }
            std::string fromHex = fromCIDs.empty() ? std::string{} : fromCIDs[0].contentHash();
            auto fromCdi = fromCIDs.empty()
                ? Hash(HashAlgorithm::SHA256)
                : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

            std::string queryTag = std::visit(
                [](const auto & q) -> std::string { return std::string(q.tag); }, pf.query);
            tracingCacheLog(
                "flush d2 fact: applyId=%s i=%zu subject=%s query=%s from=%s path=%zu fromCIDs=%zu ambientAsks=%s",
                boundary.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                i, cidasks::describe(pf.subject), queryTag, fromHex.substr(0, 12),
                path.steps.size(), fromCIDs.size(), withAmbientAsks ? "yes" : "no");

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

            auto toFactSet = TracingDecisionGraph::xorFactIntoHash(
                cumulativeFactSet, queryHash, responseHash);
            if (withAmbientAsks) {
                auto requestSet = decisionGraph->insertRequestSet({queryHash});
                decisionGraph->insertAmbientAsks(cumulativeFactSet, requestSet, toFactSet);
            }

            cidasks::Edge edge;
            auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), queryHash, responseHash);
            edge.observations.push_back({fromCdi, elementHash});
            return {toFactSet, std::move(edge)};
        };

        if (!boundary.finalized) {
            /* First finalize for this boundary. Process all facts
               accumulated so far, insert d=1 apply Fact, ε edge, and
               propagate the factHash to downstream perQAsksEdges. */
            std::vector<cidasks::Edge> walk;
            walk.reserve(group.size());
            Hash cumulativeFactSet = boundary.applyRequestHash;
            for (size_t i = 0; i < group.size(); ++i) {
                auto [nextCfs, edge] = stampAndEmit(i, walk, cumulativeFactSet, /*withAmbientAsks=*/ true);
                cumulativeFactSet = nextCfs;
                walk.push_back(std::move(edge));
            }
            auto ambientResult = cumulativeFactSet;
            tracingCacheLog(
                "finalize apply boundary: applyId=%s probes=%zu AmbientResult=%s",
                boundary.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                group.size(),
                ambientResult.to_string(HashFormat::Base16, false).substr(0, 12));

            auto factHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), boundary.applyRequestHash, ambientResult);
            if (seenRequests.insert(factHash).second) {
                v13FactSet.push_back({boundary.applyRequestHash, ambientResult});
                v13FactSetHash = TracingDecisionGraph::xorFactIntoHash(
                    v13FactSetHash, boundary.applyRequestHash, ambientResult);
                responseFor.emplace(boundary.applyRequestHash, ambientResult);
                allRequestsTrie.insert(boundary.applyRequestHash);
                allRequestHashes.insert(boundary.applyRequestHash);
            }

            cidasks::Edge applyEdge;
            applyEdge.observations.push_back({
                Hash(HashAlgorithm::SHA256),
                factHash,
            });

            auto epsilonReqSet = decisionGraph->insertRequestSet({boundary.applyRequestHash});
            size_t pos = boundary.insertionIndex + shift;
            Hash epsilonFromHash = TracingDecisionGraph::xorHashes(
                boundary.fromFactSetHashAtBoundary, priorEpsilonAccum);
            perQAsksEdges.insert(perQAsksEdges.begin() + pos,
                {epsilonFromHash, epsilonReqSet});
            d1CidasksWalk.insert(d1CidasksWalk.begin() + pos, std::move(applyEdge));
            /* Shift pending SubjectStampSites entries whose K >= pos.
               Mid-insertion into d1CidasksWalk invalidates stamped K
               for all subsequent positions; without this, warm walker
               reproduces cold's stamp with stale K. */
            for (auto & site : pendingStampSites)
                if (site.edgeIndex >= pos)
                    site.edgeIndex++;
            tracingCacheLog("finalize: ε Asks edge inserted at pos=%zu from=%s (insertionIndex=%zu shift=%zu perQ=%zu)",
                            pos,
                            epsilonFromHash.to_string(HashFormat::Base16, false).substr(0, 12),
                            boundary.insertionIndex,
                            shift,
                            perQAsksEdges.size());
            ++shift;

            for (size_t i = pos + 1; i < perQAsksEdges.size(); ++i)
                perQAsksEdges[i].fromFactSetHash = TracingDecisionGraph::xorHashes(
                    perQAsksEdges[i].fromFactSetHash, factHash);
            priorEpsilonAccum = TracingDecisionGraph::xorHashes(priorEpsilonAccum, factHash);

            /* Stash state on the boundary so subsequent re-processing
               passes (= late d2 obs) can pick up where this finalize
               left off. */
            boundary.finalized = true;
            boundary.cumulativeFactSet = ambientResult;
            boundary.factHash = factHash;
            boundary.pos = pos;
            boundary.lastProcessedCount = group.size();
        } else if (group.size() > boundary.lastProcessedCount) {
            /* Late d2 obs path: the boundary was finalized in a
               previous flush, but new probes have since arrived
               (= cb-sibling's `{f,x}: f x` only forces its local on
               the outer's `.whatever` access, after `TO_A.getType`'s
               logResult already ran the boundary's first finalize).

               Process the tail facts `[lastProcessedCount..end]` with
               `withAmbientAsks=false`: extending the AmbientAsks
               chain would change what `dispatchApplyLive` returns at
               warm (= different AmbientResult), so the walker's cur
               would diverge from the recorded factHash that
               `[finalize: ε Asks edge]` baked into downstream
               perQAsksEdges. Instead we only need the late probes'
               request payloads and recorded responses in the pool
               so `ReplayLocalObject`'s `readResponse` finds them.
               Validation against AmbientAsks is skipped for boundaries
               whose chain is empty at chainStart — see
               `ReplayLocalObject::withChainStart`. */
            std::vector<cidasks::Edge> walk;
            walk.reserve(group.size());
            Hash cumulativeFactSet = boundary.applyRequestHash;
            for (size_t i = 0; i < boundary.lastProcessedCount; ++i) {
                /* Re-stamp prior facts to rebuild walk; inserts are
                   idempotent (INSERT OR IGNORE) so the duplicate
                   Request/LocalResponse calls are harmless. */
                auto [nextCfs, edge] = stampAndEmit(i, walk, cumulativeFactSet, /*withAmbientAsks=*/ false);
                cumulativeFactSet = nextCfs;
                walk.push_back(std::move(edge));
            }
            for (size_t i = boundary.lastProcessedCount; i < group.size(); ++i) {
                auto [nextCfs, edge] = stampAndEmit(i, walk, cumulativeFactSet, /*withAmbientAsks=*/ false);
                cumulativeFactSet = nextCfs;
                walk.push_back(std::move(edge));
            }
            tracingCacheLog(
                "late-d2 process: applyId=%s tail=%zu..%zu (probes now %zu)",
                boundary.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                boundary.lastProcessedCount, group.size() - 1, group.size());
            boundary.lastProcessedCount = group.size();
        }
    }

    /* Don't clear pendingApplyBoundaries — finalized entries stay
       so `logDepth2Observation` can find them on late probes
       (option (b)). */
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
       for it (= principles 4 + 7).

       1:1 alignment: push the staged d1 edge alongside the
       perQAsksEdge. May be empty (= file-read-only Asks edge with no
       ambient observations contributing observations to d1) — still
       pushed so the indices match the walker's commitEdge counts. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        perQAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        d1CidasksWalk.push_back(std::move(pendingD1Edge));
        pendingD1Edge = {};
        tracingCacheLog("splitFlush: new Asks edge from=%s rs-size=%zu (perQ=%zu d1=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        perQAsksEdges.size(),
                        d1CidasksWalk.size());
        prevQFactSetHash = v13FactSetHash;
        pendingNewRequests.clear();
    }
}

void TracingWriter::markApplyBoundary(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;

    /* Suppressed during walker re-dispatch of an already-recorded
       apply (= `dispatchApplyLive`). Re-dispatch is validation, not a
       new cb-apply event — each re-dispatch would otherwise add a
       redundant ε edge to d1CidasksWalk, breaking the 1:1 alignment
       with walker.cidasksWalk at warm. */
    if (suppressApplyBoundary > 0) {
        tracingCacheLog("markApplyBoundary: SUPPRESSED (in dispatchApplyLive)");
        if (suppressedBoundaryHook) {
            auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
            /* Insert the apply Request payload into the CAS pool even
               when suppressed so the ε obs's factHash can be looked up
               later by the walker's ambient-asks walk. */
            auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
            decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);
            suppressedBoundaryHook(applyReqHash);
        }
        return;
    }

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
       each one, INSERTING the ε perQAsksEdge at the chronological
       insertionIndex (= position in perQAsksEdges captured AFTER
       splitFlush(false) drained the pre-boundary d=1 chunk). This
       puts ε BEFORE its body's d=1 facts in walker dispatch order,
       so the lambda-standin's seedCell extension fires before
       seed(N+1) probes try to resolve. */
    pendingApplyBoundaries.push_back({
        applyReqHash,
        applyReqHash,
        {},
        perQAsksEdges.size(),  // insertionIndex AFTER pre-boundary chunk
        prevQFactSetHash       // fromFactSetHashAtBoundary
    });
    tracingCacheLog("markApplyBoundary: buffered (applyReqHash=%s, pendingBoundaries=%zu, insertionIndex=%zu)",
                    applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    pendingApplyBoundaries.size(),
                    perQAsksEdges.size());
}

} // namespace nix

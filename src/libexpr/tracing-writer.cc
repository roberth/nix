#include "nix/expr/tracing-writer.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <set>

namespace nix {

namespace {
void rewriteFromInQuery(nlohmann::json & queryJson, const std::string & fromHex)
{
    if (queryJson.is_object() && queryJson.contains("params")) {
        auto & params = queryJson["params"];
        if (params.is_object() && params.contains("from"))
            params["from"] = fromHex;
    }
}
} // namespace

void TracingWriter::logOuterObservation(
    const trace::QueryVariant & query,
    const trace::ResultVariant & result,
    Subject subject,
    Hash argAncestry)
{
    if (!decisionGraph)
        return;

    /* Per-probe stamping. `from` is computed against the WRITER's
       current `envWalk` (which reflects every prior probe's fold),
       so successive probes on the same Subject stamp against evolved
       state — the design's per-observation state evolution. */
    auto [path, roots] = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(roots.size());
    for (auto & root : roots) {
        Hash rootSelfHash = stateHashAt(
            root, Hash(HashAlgorithm::SHA256), {}, 0);
        auto cid = stateHashAtStamping(
            root, argAncestry, envWalk, envWalk.size(),
            [&](const EvolutionStep & step) {
                insertSubjectEvolutionEdge(
                    rootSelfHash, step.curBefore,
                    step.obsFromHash, step.obsElementHash,
                    step.curAfter);
            });
        fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
    }
    std::string fromHex = fromStateHashes.empty() ? std::string{} : fromStateHashes[0].stateHash();
    auto fromStateHash = fromStateHashes.empty()
        ? Hash(HashAlgorithm::SHA256)
        : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

    std::string queryTag = std::visit(
        [](const auto & q) -> std::string { return std::string(q.tag); }, query);
    tracingCacheLog(
        "logOuterObservation: subject=%s query=%s from=%s path=%zu fromStateHashes=%zu",
        describe(subject), queryTag, fromHex.substr(0, 12),
        path.steps.size(), fromStateHashes.size());

    nlohmann::json queryJson;
    std::visit([&](const auto & q) { queryJson = q; }, query);
    rewriteFromInQuery(queryJson, fromHex);
    if (!path.steps.empty())
        queryJson["params"]["path"] = path;
    if (!fromStateHashes.empty())
        queryJson["params"]["fromStateHashes"] = fromStateHashes;
    nlohmann::json resultJson;
    std::visit([&](const auto & r) { resultJson = r; }, result);

    auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
    auto responsePayload = jsonToCborString(resultJson);
    auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);

    tracingCacheLog(
        "  reqHash=%s reqJSON=%s",
        queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
        queryJson.dump());
    tracingCacheLog(
        "  respHash=%s respJSON=%s",
        responseHash.to_string(HashFormat::Base16, false).substr(0, 12),
        resultJson.dump());
    if (provenanceEnabled()) {
        recordProvenance(queryHash, "requestHash-d1",
                         {{"queryJson", queryJson},
                          {"subject", describe(subject)},
                          {"argAncestry", argAncestry.to_string(HashFormat::Base16, false)}});
        recordProvenance(responseHash, "responseHash-d1",
                         {{"resultJson", resultJson},
                          {"queryHash", queryHash.to_string(HashFormat::Base16, false)}});
    }

    decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));

    /* Secondary index for producer queries — see comment on the
       original loop for the reasoning. Preserved verbatim. */
    if ((queryTag == "getAttr" || queryTag == "getListElem") && !roots.empty()) {
        std::vector<trace::QueryLeaf> initialFromStateHashes;
        initialFromStateHashes.reserve(roots.size());
        for (auto & root : roots) {
            auto initStateHash = stateHashAt(
                root, argAncestry, {}, 0);
            initialFromStateHashes.emplace_back(
                initStateHash.to_string(HashFormat::Base16, false));
        }
        std::string initialFromHex = initialFromStateHashes[0].stateHash();
        nlohmann::json initialQueryJson;
        std::visit([&](const auto & q) { initialQueryJson = q; }, query);
        rewriteFromInQuery(initialQueryJson, initialFromHex);
        if (!path.steps.empty())
            initialQueryJson["params"]["path"] = path;
        initialQueryJson["params"]["fromStateHashes"] = initialFromStateHashes;
        auto initialReqHash = hashString(
            HashAlgorithm::SHA256, initialQueryJson.dump());
        if (initialReqHash != queryHash) {
            tracingCacheLog(
                "  secondary insert at initial-history reqHash=%s from=%s",
                initialReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                initialFromHex.substr(0, 12));
            decisionGraph->insertRequest(
                initialReqHash, jsonToCborString(initialQueryJson));
        }
    }

    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), queryHash, responseHash);
    auto factHash = elementHash;

    /* Dedup by (request, response). If already recorded this session,
       skip both envFactSet fold AND envWalk push — pushing a duplicate
       ObservationSet would XOR-cancel its earlier contribution to any
       Subject's own-loop fold (see the design's XOR audit). */
    if (!seenRequests.insert(factHash).second)
        return;

    envFactSet.push_back({queryHash, responseHash});
    envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
        envFactSetHash, queryHash, responseHash);
    responseFor.emplace(queryHash, responseHash);
    sessionRequestsTrie.insert(queryHash);
    allRequestHashes.insert(queryHash);

    /* Per-probe Ask/envWalk push: single-observation ObservationSet
       at a single-request Ask. Walker at replay dispatches this Ask's
       one request, folds the response, advances cur by exactly this
       observation's elementHash. Next probe's stamping (walker side)
       sees the advanced envWalk and stamps its own request with the
       evolved `from`. */
    auto requestSetHash = decisionGraph->insertRequestSet({queryHash});
    envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
    ObservationSet obsSet;
    obsSet.observations.push_back({fromStateHash, elementHash});
    envWalk.push_back(std::move(obsSet));
    tracingCacheLog(
        "logOuterObservation: pushed Ask+envWalk from=%s (perQ=%zu env=%zu)",
        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
        envAsksEdges.size(), envWalk.size());
    prevQFactSetHash = envFactSetHash;
}

void TracingWriter::flushAmbient(bool processApplies)
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

    /* Depth-1 facts (outer-value probes) are now stamped and pushed
       per-probe in `logOuterObservation`, not batched here. Depth-2
       facts (cb-apply ambient chain probes) still group by cb-apply id
       and are processed at `processApplies=true` below. */

    if (!processApplies) {
        /* Intermediate flush: ambient layer facts stay buffered until
           their cb-apply is finalised at logResult. The cb-apply
           boundary's ambient chain may not be complete yet (= outer is
           still probing the local), so we can't compute AmbientResult
           here without risking an incomplete chain. */
        return;
    }

    /* Finalize: close the trailing chunk of file/env-var reads (which
       flow through logResponse, not through the per-probe
       logOuterObservation path). One Ask per closeAsksEdge covers
       these; they don't need per-observation state evolution because
       their requestHashes don't carry a `from` field. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        envWalk.push_back({});  // 1:1 with envAsksEdges; empty is harmless for stateHashAt.
        tracingCacheLog("finalize: final env Asks edge from=%s rs-size=%zu (perQ=%zu env=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        envAsksEdges.size(),
                        envWalk.size());
        prevQFactSetHash = envFactSetHash;
        pendingNewRequests.clear();
    }

    /* Finalize pass: process each buffered cb-apply in the
       order recorded. For each boundary:
        1. Look up its ambient group (may be empty if no probes happened).
        2. Build the ambient chain via incremental stateHashAt
           substitution — each fact's `from` is computed against the
           chain prefix the walker reconstructs probe-by-probe.
        3. The terminal `cumulativeFactSet` IS the AmbientResult
           (via-Asks §"Recording (ambient layer)").
        4. Synthesize the env apply Fact (applyReqHash, AmbientResult);
           fold into envFactSet and append a synthetic edge to
           envWalk (fromHash=Hash(0), elementHash=factHash) —
           the cb-apply contributes to cur but not to any
           subject's own-fold (= phantom edge for history-index sync).
        5. Add applyReqHash to pendingNewRequests so the trailing
           closeAsksEdge's perQAsksEdge close picks it up.

       Reverse-nested order is acceptable but unnecessary: AmbientResult
       for one apply doesn't depend on another's chain — each group is
       independent because their ambient chains share no facts.

       Chain rooting: each cb-apply's ambient chain is rooted at
       `pendingApply.applyRequestHash` (= the natural hash of the apply
       payload) rather than `emptySetHash()`. This makes each
       cb-apply's chain its own subtree in AmbientAsks, so the
       walker can pick the right chain by knowing the apply Fact's
       reqHash — no ambiguity when multiple cb-applies are recorded.
       via-Asks discrimination via inherited state hash propagation still
       flows through: different inherited state hashes → different argSubject →
       different applyReqHash → different chain root. Same-shape
       collapse still holds for identical cb-applies (= same fnId,
       same argSubject, same observations → same applyReqHash → same
       chain). The loss is atom sharing across cb-applies whose
       chains happen to coincide but whose applyReqHashes differ — a
       storage trade-off, not correctness. */
    /* Same-shape collapse happens automatically via content-addressed
       AmbientAsks rows: two boundaries with the same applyId and the
       same probes produce identical rows that INSERT OR IGNORE
       deduplicates; envFactSet's seenRequests deduplicates the
       synthesised env Fact too. Process each boundary independently
       so its own pendingDepth2FactsByApply slice gets folded into
       its own chain — collapsing the boundary list before processing
       conflates probes across boundaries and the resulting
       AmbientResult doesn't match what a per-call walker would
       reproduce.

       Chronological cb-apply Ask insertion: each observed cb-apply's
       Ask is INSERTED at pendingApply.insertionIndex (= captured at
       openCbApply time, AFTER closeAsksEdge(false) drained the
       pre-boundary env chunk), not appended at the end. This puts
       the cb-apply Ask BEFORE its body's env facts in walker
       dispatch order. Each insertion shifts subsequent indices by 1,
       tracked via `shift`. Each cb-apply's elementHash propagates
       into all subsequent envAsksEdges' fromFactSetHash (= walker's
       cur advances by that Fact at that position).
       `priorApplyFactAccum` accumulates earlier cb-apply Fact
       contributions so each new cb-apply Ask's own fromFactSetHash
       reflects all prior cb-apply contributions to its left. */
    size_t shift = 0;
    Hash priorApplyFactAccum(HashAlgorithm::SHA256);
    for (auto & pendingApply : pendingCbApplies) {
        auto & group = pendingApply.facts;

        /* Helper: stamp the i-th fact and emit Request/InnerValueResponse
           into the pool. AmbientAsks is inserted iff `withAmbientAsks`
           is true. Returns (cumulativeFactSet, history-edge-to-append).
           Used both for first-finalize processing (with AmbientAsks)
           and for late-d2-obs re-processing (without AmbientAsks —
           see commentary at the "late probe" branch below). */
        auto stampAndEmit = [&](size_t i, const std::vector<ObservationSet> & history,
                                Hash cumulativeFactSet, bool withAmbientAsks,
                                Hash contextCur = Hash(HashAlgorithm::SHA256))
            -> std::pair<Hash, ObservationSet>
        {
            auto & pf = group[i];
            auto [path, roots] = pathAndRootsFromSubject(pf.subject);
            std::vector<trace::QueryLeaf> fromStateHashes;
            fromStateHashes.reserve(roots.size());
            for (auto & root : roots) {
                /* Subject-evolution fast-path: stamp SubjectEvolutionEdges via hook. */
                Hash rootSelfHash = stateHashAt(
                    root, Hash(HashAlgorithm::SHA256), {}, 0);
                auto cid = stateHashAtStamping(
                    root, pf.argAncestry, history, /*step=*/ i,
                    [&](const EvolutionStep & step) {
                        insertSubjectEvolutionEdge(
                            rootSelfHash, step.curBefore,
                            step.obsFromHash, step.obsElementHash,
                            step.curAfter);
                    });
                fromStateHashes.emplace_back(cid.to_string(HashFormat::Base16, false));
            }
            std::string fromHex = fromStateHashes.empty() ? std::string{} : fromStateHashes[0].stateHash();
            auto fromStateHash = fromStateHashes.empty()
                ? Hash(HashAlgorithm::SHA256)
                : Hash::parseNonSRIUnprefixed(fromHex, HashAlgorithm::SHA256);

            std::string queryTag = std::visit(
                [](const auto & q) -> std::string { return std::string(q.tag); }, pf.query);
            tracingCacheLog(
                "flush ambient fact: applyId=%s i=%zu subject=%s query=%s from=%s path=%zu fromStateHashes=%zu ambientAsks=%s",
                pendingApply.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                i, describe(pf.subject), queryTag, fromHex.substr(0, 12),
                path.steps.size(), fromStateHashes.size(), withAmbientAsks ? "yes" : "no");

            nlohmann::json queryJson;
            std::visit([&](const auto & q) { queryJson = q; }, pf.query);
            rewriteFromInQuery(queryJson, fromHex);
            if (!path.steps.empty())
                queryJson["params"]["path"] = path;
            if (!fromStateHashes.empty())
                queryJson["params"]["fromStateHashes"] = fromStateHashes;
            nlohmann::json resultJson;
            std::visit([&](const auto & r) { resultJson = r; }, pf.result);

            auto queryHash = hashString(HashAlgorithm::SHA256, queryJson.dump());
            auto responsePayload = jsonToCborString(resultJson);
            auto responseHash = TracingDecisionGraph::computeResponseHash(responsePayload);
            /* Record BEFORE insertRequest so the richer ambient
               provenance beats the generic RequestHash from the
               insertRequest macro. */
            if (provenanceEnabled()) {
                recordProvenance(queryHash, "requestHash-ambient",
                                 {{"queryJson", queryJson},
                                  {"subject", describe(pf.subject)},
                                  {"argAncestry", pf.argAncestry.to_string(HashFormat::Base16, false)},
                                  {"applyId", pendingApply.applyId.to_string(HashFormat::Base16, false)},
                                  {"boundaryFactIndex", i},
                                  {"fromHex", fromHex}});
                recordProvenance(responseHash, "responseHash-ambient",
                                 {{"resultJson", resultJson},
                                  {"queryHash", queryHash.to_string(HashFormat::Base16, false)}});
            }

            decisionGraph->insertRequest(queryHash, jsonToCborString(queryJson));
            /* contextHash is the walker's Env `cur` at the time the
               response is recorded (vocab, "Ambient payload types and
               edges"). At the writer that value is `contextCur`
               (= `envCurAtOpen XOR priorApplyFactAccum`), which also
               equals `boundaryAskFromHash` (below), so the walker at
               warm sees the identical Hash as its Ask edge's
               `fromFactSetHash` and lands on the row inserted here.
               Cross-cached-call disambiguation is handled upstream by
               `callArgAncestry` inside `requestHash`; no outer
               contribution needed here. */
            decisionGraph->insertInnerValueResponse(queryHash, contextCur, responsePayload);

            auto toFactSet = TracingDecisionGraph::xorFactIntoHash(
                cumulativeFactSet, queryHash, responseHash);
            if (withAmbientAsks) {
                auto requestSet = decisionGraph->insertRequestSet({queryHash});
                decisionGraph->insertAmbientAsk(cumulativeFactSet, requestSet, toFactSet);
            }

            ObservationSet edge;
            auto elementHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), queryHash, responseHash);
            edge.observations.push_back({fromStateHash, elementHash});
            return {toFactSet, std::move(edge)};
        };

        if (!pendingApply.finalized) {
            /* First finalize for this boundary. Process all facts
               accumulated so far. If any were made, insert the
               boundary Ask carrying the applyRequest, fold the
               (applyRequestHash, AmbientResult) Fact into
               envFactSet, and propagate that factHash to downstream
               envAsksEdges.

               `contextCur` = the walker's outer env cur at the
               moment this boundary's cb-apply Request will be
               dispatched at warm. Equals
               `pendingApply.envCurAtOpen XOR priorApplyFactAccum`
               (= state at openCbApply time + all prior
               boundary Ask contributions). Used as the
               InnerValueResponse key discriminator so ambient chain
               facts within different apply boundaries store under
               distinct rows, letting cb-repeated's two applies with
               the same abstract reqHash resolve to their respective
               responses. */
            Hash contextCur = TracingDecisionGraph::xorHashes(
                pendingApply.envCurAtOpen, priorApplyFactAccum);
            pendingApply.contextCur = contextCur;
            std::vector<ObservationSet> history;
            history.reserve(group.size());
            Hash cumulativeFactSet = pendingApply.applyRequestHash;
            for (size_t i = 0; i < group.size(); ++i) {
                auto [nextCfs, edge] = stampAndEmit(i, history, cumulativeFactSet, /*withAmbientAsks=*/ true, contextCur);
                cumulativeFactSet = nextCfs;
                history.push_back(std::move(edge));
            }
            auto ambientResult = cumulativeFactSet;
            tracingCacheLog(
                "finalize cb-apply: applyId=%s probes=%zu AmbientResult=%s",
                pendingApply.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                group.size(),
                ambientResult.to_string(HashFormat::Base16, false).substr(0, 12));

            /* Unobserved cb-apply: the outer's callback body ran
               without probing the inner-supplied arg (group is empty).
               No content crossed the boundary → no Ambient Fact to
               record → no boundary Ask needed. Leave
               `pendingApply.finalized` false so a later flush with
               newly-arrived probes re-enters this branch and records
               the boundary once observations exist. */
            if (group.empty())
                continue;

            auto factHash = TracingDecisionGraph::xorFactIntoHash(
                Hash(HashAlgorithm::SHA256), pendingApply.applyRequestHash, ambientResult);
            /* Dedup gate: `seenRequests` tracks (applyRequestHash,
               ambientResult) pairs. Under matching-until-divergence
               with XOR-evolution, same-shape sibling boundaries
               produce identical (applyRequestHash, ambientResult),
               so `seenRequests.insert(factHash).second` returns
               false for the second sibling. When that happens, we
               must NOT propagate the boundary contribution
               downstream either — otherwise XORing the same
               factHash twice cancels the first boundary's
               contribution and the walker's cur at recorded edges
               drifts. */
            bool isNewFact = seenRequests.insert(factHash).second;
            if (isNewFact) {
                envFactSet.push_back({pendingApply.applyRequestHash, ambientResult});
                envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
                    envFactSetHash, pendingApply.applyRequestHash, ambientResult);
                responseFor.emplace(pendingApply.applyRequestHash, ambientResult);
                sessionRequestsTrie.insert(pendingApply.applyRequestHash);
                allRequestHashes.insert(pendingApply.applyRequestHash);
            }

            ObservationSet applyEdge;
            applyEdge.observations.push_back({
                Hash(HashAlgorithm::SHA256),
                factHash,
            });

            auto boundaryAskRequestSet = decisionGraph->insertRequestSet({pendingApply.applyRequestHash});
            size_t pos = pendingApply.insertionIndex + shift;
            Hash boundaryAskFromHash = TracingDecisionGraph::xorHashes(
                pendingApply.envCurAtOpen, priorApplyFactAccum);
            envAsksEdges.insert(envAsksEdges.begin() + pos,
                {boundaryAskFromHash, boundaryAskRequestSet});
            envWalk.insert(envWalk.begin() + pos, std::move(applyEdge));
            tracingCacheLog("finalize: cb-apply Ask inserted at pos=%zu from=%s (insertionIndex=%zu shift=%zu perQ=%zu)",
                            pos,
                            boundaryAskFromHash.to_string(HashFormat::Base16, false).substr(0, 12),
                            pendingApply.insertionIndex,
                            shift,
                            envAsksEdges.size());
            ++shift;

            if (isNewFact) {
                for (size_t i = pos + 1; i < envAsksEdges.size(); ++i)
                    envAsksEdges[i].fromFactSetHash = TracingDecisionGraph::xorHashes(
                        envAsksEdges[i].fromFactSetHash, factHash);
                priorApplyFactAccum = TracingDecisionGraph::xorHashes(priorApplyFactAccum, factHash);
            }
            /* Keep prevQFactSetHash aligned with envFactSetHash after
               the boundary XOR-fold. Without this, subsequent Q's
               `openCbApply` captures a stale (pre-boundary)
               envCurAtOpen, and subsequent `finalize`
               pushes edges indexed at a pre-boundary state that walker
               can't reach from its post-boundary cur. cb-repeated
               variant 2's Q=6063a6243f6c history misses at cur=99566783ffd7
               because cold indexed its edges at pre-boundary state
               3dc1fe6c5b76 = 99566783ffd7 XOR factHash_boundary0. */
            prevQFactSetHash = envFactSetHash;

            /* Stash state on the boundary so subsequent re-processing
               passes (= late ambient obs) can pick up where this finalize
               left off. */
            pendingApply.finalized = true;
            pendingApply.cumulativeFactSet = ambientResult;
            pendingApply.factHash = factHash;
            pendingApply.pos = pos;
            pendingApply.lastProcessedCount = group.size();
        } else if (group.size() > pendingApply.lastProcessedCount) {
            /* Late ambient obs path: the boundary was finalized in a
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
               envAsksEdges. Instead we only need the late probes'
               request payloads and recorded responses in the pool
               so `ReplayCallbackArg`'s `readResponse` finds them.
               Validation against AmbientAsks is skipped for boundaries
               whose chain is empty at chainStart — see
               `ReplayCallbackArg::withChainStart`. */
            std::vector<ObservationSet> history;
            history.reserve(group.size());
            Hash cumulativeFactSet = pendingApply.applyRequestHash;
            for (size_t i = 0; i < pendingApply.lastProcessedCount; ++i) {
                /* Re-stamp prior facts to rebuild history; inserts are
                   idempotent (INSERT OR IGNORE) so the duplicate
                   Request/InnerValueResponse calls are harmless. */
                auto [nextCfs, edge] = stampAndEmit(i, history, cumulativeFactSet, /*withAmbientAsks=*/ false, pendingApply.contextCur);
                cumulativeFactSet = nextCfs;
                history.push_back(std::move(edge));
            }
            for (size_t i = pendingApply.lastProcessedCount; i < group.size(); ++i) {
                auto [nextCfs, edge] = stampAndEmit(i, history, cumulativeFactSet, /*withAmbientAsks=*/ false, pendingApply.contextCur);
                cumulativeFactSet = nextCfs;
                history.push_back(std::move(edge));
            }
            tracingCacheLog(
                "late-ambient process: applyId=%s tail=%zu..%zu (probes now %zu)",
                pendingApply.applyId.to_string(HashFormat::Base16, false).substr(0, 12),
                pendingApply.lastProcessedCount, group.size() - 1, group.size());
            pendingApply.lastProcessedCount = group.size();
        }
    }

    /* Don't clear pendingCbApplies — finalized entries stay
       so `logAmbientObservation` can find them on late probes
       (option (b)). */
}

void TracingWriter::closeAsksEdge(bool processApplies)
{
    if (!decisionGraph)
        return;

    /* Process pending ambient observations into one new Asks edge
       transition (= advances envFactSetHash and envWalk when
       observations are present). At processApplies=true this also computes
       AmbientResults for each buffered cb-apply and folds
       the synthetic env apply Facts in. */
    flushAmbient(processApplies);

    /* Close the trailing file/env-read batch (logResponse path only —
       outer-value probes push their own Ask per probe). One Ask row
       per closeAsksEdge covers whatever file/env reads have
       accumulated since the last close; walker's envWalk gets an
       empty ObservationSet (file/env reads don't advance any
       Subject's state hash) so the 1:1 alignment holds. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
        envWalk.push_back({});  // 1:1 with envAsksEdges; empty is harmless for stateHashAt.
        tracingCacheLog("closeAsksEdge: new Asks edge from=%s rs-size=%zu (perQ=%zu env=%zu)",
                        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        pendingNewRequests.size(),
                        envAsksEdges.size(),
                        envWalk.size());
        prevQFactSetHash = envFactSetHash;
        pendingNewRequests.clear();
    }
}

void TracingWriter::openCbApply(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;

    /* Suppressed during walker re-dispatch of an already-recorded
       apply (= `dispatchApplyLive`). Re-dispatch is validation, not a
       new cb-apply event — each re-dispatch would otherwise add a
       redundant ε edge to envWalk, breaking the 1:1 alignment
       with walker.envWalk at warm. */
    if (suppressCbApply > 0) {
        tracingCacheLog("openCbApply: SUPPRESSED (in dispatchApplyLive)");
        /* Insert the apply Request payload into the CAS pool even when
           suppressed so walker's ambient-asks history can look it up.
           Hook-based ε obs push in walker (iter <=91) is now redundant
           after writer prev-post-boundary alignment + walker retry loop. */
        auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
        auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
        decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);
        return;
    }

    /* Close any preceding observations into their own Asks edge
       (= β1). The intermediate flush only drains env layer facts; any
       ambient layer facts from prior unfinalised cb-applies (= nested case)
       stay buffered, waiting for their own boundary's finalize. */
    closeAsksEdge(/*processApplies=*/ false);

    /* Insert the apply Request payload into the CAS pool now — its
       hash is known immediately, payload doesn't depend on AmbientResult.
       The walker needs this entry by the time it dispatches the apply
       Fact at warm replay. */
    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    /* Record BEFORE insertRequest so the richer applyRequestHash
       provenance beats the generic RequestHash entry from the
       insertRequest macro (first-registration-wins). */
    Hash outerEnvCurAtOpen = outerWriter
        ? outerWriter->getV13FactSetHash()
        : Hash(HashAlgorithm::SHA256);
    if (provenanceEnabled())
        recordProvenance(applyReqHash, "applyRequestHash",
                         {{"applyQueryPayload", applyQueryPayload},
                          {"prevQFactSetHash", prevQFactSetHash.to_string(HashFormat::Base16, false)},
                          {"envFactSetHash", envFactSetHash.to_string(HashFormat::Base16, false)},
                          {"outerEnvCurAtOpen", outerEnvCurAtOpen.to_string(HashFormat::Base16, false)}});
    decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);

    /* Buffer the boundary. The synthetic env apply Fact's respHash
       is the AmbientResult = terminal of this cb-apply's ambient chain,
       only known after the body finishes. flushAmbient at
       logResult walks pendingCbApplies in order and finalises
       each one, INSERTING the ε perQAsksEdge at the chronological
       insertionIndex (= position in envAsksEdges captured AFTER
       closeAsksEdge(false) drained the pre-boundary env chunk). This
       puts ε BEFORE its body's env facts in walker dispatch order,
       so the lambda-ReplayCallbackArg's seedCell extension fires before
       arg(N+1) probes try to resolve. */
    pendingCbApplies.push_back({
        applyReqHash,
        applyReqHash,
        {},
        envAsksEdges.size(),  // insertionIndex AFTER pre-boundary chunk
        /* envCurAtOpen: per-Q factSet at cb-apply-open moment. Aligns
           with walker's per-walk factSet at the corresponding
           dispatchApplyLive on the read side. */
        perQFactSetHash(),
        outerEnvCurAtOpen,     // captured for InnerValueResponse contextHash
        Hash(HashAlgorithm::SHA256)  // contextCur (populated at first finalize)
    });
    tracingCacheLog("openCbApply: buffered (applyReqHash=%s, pendingBoundaries=%zu, insertionIndex=%zu)",
                    applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    pendingCbApplies.size(),
                    envAsksEdges.size());
}

} // namespace nix

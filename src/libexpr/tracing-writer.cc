#include "nix/expr/tracing-writer.hh"
#include "nix/expr/subject-id.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-decision-graph.hh"

#include <set>

namespace nix {


void TracingWriter::logOuterObservation(
    const trace::QueryVariant & query,
    const trace::ResultVariant & result,
    Subject subject,
    Hash argAncestry)
{
    if (!decisionGraph)
        return;

    /* Task #110 (C3): QueryCallbackApply emission moved to
       TracingObject::whnf() where the applyResult's WHNF is
       actually known. No preamble here — a WHNF query always
       precedes any structural access on an applyResult, so cold
       will have emitted QCA-with-WHNF by the time non-WHNF probes
       on that applyResult happen. */

    /* Per-probe stamping. `from` is computed against the WRITER's
       current `envWalk` (which reflects every prior probe's fold),
       so successive probes on the same Subject stamp against evolved
       state — the design's per-observation state evolution. */
    auto [path, roots] = pathAndRootsFromSubject(subject);
    std::vector<trace::QueryLeaf> fromStateHashes;
    fromStateHashes.reserve(roots.size());
    for (auto & root : roots) {
        auto cid = stateHashAt(
            root, argAncestry, envWalk, envWalk.size());
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

    trace::QueryVariant stampedQuery = query;
    std::visit(
        [&](auto & q) {
            using Q = std::decay_t<decltype(q)>;
            if constexpr (requires { q.from; })
                q.from = trace::QueryLeaf{trace::StateHashLeaf{fromHex, {}}};
            if constexpr (requires { q.perArgFrame; }) {
                q.perArgFrame.path = path;
                q.perArgFrame.fromStateHashes = fromStateHashes;
            }
            if constexpr (requires { q.fromStateHashes = fromStateHashes; })  // QueryApply
                q.fromStateHashes = fromStateHashes;
        },
        stampedQuery);
    nlohmann::json queryJson = trace::toJson(stampedQuery);
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
        trace::QueryVariant initialStamped = query;
        std::visit(
            [&](auto & q) {
                using Q = std::decay_t<decltype(q)>;
                if constexpr (requires { q.from; })
                    q.from = trace::QueryLeaf{trace::StateHashLeaf{initialFromHex, {}}};
                if constexpr (requires { q.perArgFrame; }) {
                    q.perArgFrame.path = path;
                    q.perArgFrame.fromStateHashes = initialFromStateHashes;
                }
                if constexpr (requires { q.fromStateHashes = initialFromStateHashes; })  // QueryApply
                    q.fromStateHashes = initialFromStateHashes;
            },
            initialStamped);
        nlohmann::json initialQueryJson = trace::toJson(initialStamped);
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

    /* Per-probe Ask push. Task #110 Q-evolution: an observation
       happening during Q's walk is part of Q's Ask chain — for EVERY
       Q currently active on the stack. Parent Q's evaluation includes
       child Q's observations too; each Q's chain must be complete for
       the walker to follow it. Insert Ask immediately under every
       active Q's currentQ.

       Order per active Q: (1) record Ask at (Q_before-fold, cur_before),
       (2) fold observation into cur/envWalk, (3) re-derive Q_after-fold
       (see below). */
    auto requestSetHash = decisionGraph->insertRequestSet({queryHash});
    /* Task #110 (correct model): each observation belongs to exactly
       one Q — the innermost active one. Sub-Qs' observations are
       NOT part of parent Q's chain; parent observes the sub-Q as a
       composite (via its own logResult that folds sub-Q's Terminal
       into parent's envWalk). Skip Ask insertion when the stack is
       empty (no attributable Q). */
    if (!activeQueryStack.empty()) {
        auto & innermost = activeQueryStack.back();
        decisionGraph->insertAsk(innermost.currentQ, prevQFactSetHash, requestSetHash);
    }
    envAsksEdges.push_back({prevQFactSetHash, requestSetHash});
    ObservationSet obsSet;
    obsSet.observations.push_back({fromStateHash, elementHash});
    envWalk.push_back(obsSet);
    /* Task #110: append to innermost Q's perQEnvWalk. Session envWalk
       stays 1:1-aligned with envAsksEdges for other bookkeeping. */
    if (!activeQueryStack.empty()) {
        activeQueryStack.back().perQEnvWalk.push_back(std::move(obsSet));
    }
    tracingCacheLog(
        "logOuterObservation: inserted Ask under %zu active Q(s) from=%s (env=%zu)",
        activeQueryStack.size(),
        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
        envWalk.size());
    prevQFactSetHash = envFactSetHash;

    /* Q evolution: after folding this observation into the innermost
       Q's perQEnvWalk, if its fromSubject has evolved, re-derive Q's
       from-field and re-hash. Per-Q chain preserves same-shape
       collapse: two Qs with the same fromSubject-initial-state and
       the same own-chain evolve to the same finalQ regardless of
       what other Qs did in the session. */
    if (!activeQueryStack.empty()) {
        auto & aq = activeQueryStack.back();
        if (aq.fromSubject) {
            auto newState = stateHashAt(
                *aq.fromSubject, aq.fromSubjectArgAncestry,
                aq.perQEnvWalk, aq.perQEnvWalk.size());
            if (newState != aq.fromSubjectLastState) {
                aq.fromSubjectLastState = newState;
                trace::rewriteFrom(
                    aq.payloadTemplate,
                    newState.to_string(HashFormat::Base16, false));
                auto newQ = trace::computeQueryHash(aq.payloadTemplate);
                tracingCacheLog(
                    "Q-evolution: Q %s -> %s (fromSubject state %s)",
                    aq.currentQ.to_string(HashFormat::Base16, false).substr(0, 12),
                    newQ.to_string(HashFormat::Base16, false).substr(0, 12),
                    newState.to_string(HashFormat::Base16, false).substr(0, 12));
                aq.currentQ = newQ;
            }
        }
    }
}

void TracingWriter::logCompositeSubQ(
    const nlohmann::json & subQueryPayload,
    Hash subQueryHash,
    const nlohmann::json & subResultPayload,
    Hash subResultHash)
{
    if (!decisionGraph || activeQueryStack.empty())
        return;

    auto payloadCbor = jsonToCborString(subQueryPayload);
    /* subQueryHash = SHA(payload.dump()) already by construction —
       sub-Q's finalQ. Store the payload in the Requests pool. */
    decisionGraph->insertRequest(subQueryHash, payloadCbor);

    auto elementHash = TracingDecisionGraph::xorFactIntoHash(
        Hash(HashAlgorithm::SHA256), subQueryHash, subResultHash);
    auto factHash = elementHash;

    if (!seenRequests.insert(factHash).second)
        return;

    envFactSet.push_back({subQueryHash, subResultHash});
    envFactSetHash = TracingDecisionGraph::xorFactIntoHash(
        envFactSetHash, subQueryHash, subResultHash);
    responseFor.emplace(subQueryHash, subResultHash);
    sessionRequestsTrie.insert(subQueryHash);
    allRequestHashes.insert(subQueryHash);

    auto requestSetHash = decisionGraph->insertRequestSet({subQueryHash});
    auto & parent = activeQueryStack.back();
    decisionGraph->insertAsk(parent.currentQ, prevQFactSetHash, requestSetHash);
    envAsksEdges.push_back({prevQFactSetHash, requestSetHash});

    /* fromHash=0: stateHashAt's `obs.fromHash == subject.stateHash`
       filter never matches any real Subject, so the composite advances
       cur but doesn't perturb any Subject's own-loop. Same-shape sub-Qs
       produce identical (request, response) pairs and dedup above. */
    ObservationSet obsSet;
    obsSet.observations.push_back({Hash(HashAlgorithm::SHA256), elementHash});
    envWalk.push_back(obsSet);
    parent.perQEnvWalk.push_back(std::move(obsSet));
    tracingCacheLog(
        "logCompositeSubQ: parent Q=%s from=%s req=%s (env=%zu)",
        parent.currentQ.to_string(HashFormat::Base16, false).substr(0, 12),
        prevQFactSetHash.to_string(HashFormat::Base16, false).substr(0, 12),
        subQueryHash.to_string(HashFormat::Base16, false).substr(0, 12),
        envWalk.size());
    prevQFactSetHash = envFactSetHash;

    /* Parent Q-evolution rederivation — mirrors logOuterObservation.
       With obs.fromHash=0 no real Subject's own-loop matches, so
       parent.fromSubject state won't advance and the newQ block is a
       no-op in practice — but kept for structural symmetry. */
    if (parent.fromSubject) {
        auto newState = stateHashAt(
            *parent.fromSubject, parent.fromSubjectArgAncestry,
            parent.perQEnvWalk, parent.perQEnvWalk.size());
        if (newState != parent.fromSubjectLastState) {
            parent.fromSubjectLastState = newState;
            trace::rewriteFrom(
                parent.payloadTemplate,
                newState.to_string(HashFormat::Base16, false));
            parent.currentQ = trace::computeQueryHash(parent.payloadTemplate);
        }
    }
}

void TracingWriter::flushPending(bool processApplies)
{
    if (!decisionGraph)
        return;

    /* Pass A: insert deferred Requests at their natural keys
       (= hash of the payload). */
    for (auto & payload : pendingRequests) {
        auto key = hashString(HashAlgorithm::SHA256, payload.dump());
        decisionGraph->insertRequest(key, jsonToCborString(payload));
    }
    pendingRequests.clear();

    /* Outer-value probes are stamped and pushed per-probe in
       `logOuterObservation`, not batched here. Callback observations
       accumulate in their enclosing CallbackCell's runningObsSet and
       get snapshotted at sampling moments; nothing to flush here. */

    if (!processApplies) {
        /* Intermediate flush: pending state stays buffered until the
           enclosing evaluation reaches logResult. */
        return;
    }

    /* Finalize: close the trailing chunk of file/env-var reads (which
       flow through logResponse, not through the per-probe
       logOuterObservation path). One Ask per closeAsksEdge covers
       these; they don't need per-observation state evolution because
       their requestHashes don't carry a `from` field. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        /* Task #110 (correct model): innermost active Q only. */
        if (!activeQueryStack.empty()) {
            auto & innermost = activeQueryStack.back();
            decisionGraph->insertAsk(innermost.currentQ, prevQFactSetHash, requestSetHash);
        }
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
}

void TracingWriter::closeAsksEdge(bool processApplies)
{
    if (!decisionGraph)
        return;

    /* Insert any deferred Requests at their natural keys. */
    flushPending(processApplies);

    /* Close the trailing file/env-read batch (logResponse path only —
       outer-value probes push their own Ask per probe). One Ask row
       per closeAsksEdge covers whatever file/env reads have
       accumulated since the last close; walker's envWalk gets an
       empty ObservationSet (file/env reads don't advance any
       Subject's state hash) so the 1:1 alignment holds. */
    if (!pendingNewRequests.empty()) {
        auto requestSetHash = decisionGraph->insertRequestSet(pendingNewRequests);
        /* Task #110 (correct model): innermost active Q only. */
        if (!activeQueryStack.empty()) {
            auto & innermost = activeQueryStack.back();
            decisionGraph->insertAsk(innermost.currentQ, prevQFactSetHash, requestSetHash);
        }
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

void TracingWriter::createCallbackCell(const nlohmann::json & applyQueryPayload)
{
    if (!decisionGraph)
        return;

    /* Suppressed during walker re-dispatch of an already-recorded
       apply. Re-dispatch is validation, not a new cb-apply event —
       skip cell creation so `envWalk` alignment doesn't drift. The
       apply Request payload is still inserted so walker lookups
       find it. */
    auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQueryPayload.dump());
    auto applyPayloadCbor = jsonToCborString(applyQueryPayload);
    if (suppressCbApply > 0) {
        tracingCacheLog("createCallbackCell: SUPPRESSED");
        decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);
        return;
    }

    if (provenanceEnabled())
        recordProvenance(applyReqHash, "applyRequestHash",
                         {{"applyQueryPayload", applyQueryPayload},
                          {"prevQFactSetHash", prevQFactSetHash.to_string(HashFormat::Base16, false)},
                          {"envFactSetHash", envFactSetHash.to_string(HashFormat::Base16, false)}});
    decisionGraph->insertRequest(applyReqHash, applyPayloadCbor);

    /* Extract fn's state hash from the applyQueryPayload (params.fn)
       so we can look up the cell at CallbackApplyRef stamping time
       without re-parsing. Empty string on legacy payloads where the
       field isn't present (in which case CallbackApply emission is
       skipped). */
    std::string fnStateHashHex;
    try {
        if (applyQueryPayload.contains("params")
            && applyQueryPayload["params"].contains("fn")
            && applyQueryPayload["params"]["fn"].is_object()
            && applyQueryPayload["params"]["fn"].contains("stateHash"))
            fnStateHashHex = applyQueryPayload["params"]["fn"]["stateHash"].get<std::string>();
        else if (applyQueryPayload.contains("params")
                 && applyQueryPayload["params"].contains("fn")
                 && applyQueryPayload["params"]["fn"].is_string())
            fnStateHashHex = applyQueryPayload["params"]["fn"].get<std::string>();
    } catch (...) {}
    CallbackCell cell;
    cell.applyId = applyReqHash;
    cell.fnStateHashHex = std::move(fnStateHashHex);
    callbackCells.push_back(std::move(cell));
    tracingCacheLog("createCallbackCell: applyReqHash=%s cells=%zu",
                    applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    callbackCells.size());
}

} // namespace nix

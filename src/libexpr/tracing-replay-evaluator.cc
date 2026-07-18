#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/arg-cell.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/replay-callback-arg.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-callback-arg.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/expr/tracing-cache-provenance.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"
#include "nix/expr/object-type.hh"

#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace nix {

TracingReplayEvaluator::TracingReplayEvaluator(
    ref<Evaluator> inner,
    Environment & validationEnv,
    TracingWriter & writer,
    TracingDecisionGraph & decisionGraph)
    : inner(inner)
    , decisionGraph(decisionGraph)
    , writer(writer)
    , validationEnv(validationEnv)
{
}

std::optional<TracingReplayEvaluator::WalkResult>
TracingReplayEvaluator::walk(const Hash & queryHash, std::shared_ptr<Object> currentProxy)
{
    /* The entire history is VALIDATION of recorded state — any apply
       queries triggered through `fnObj->queryApply(...)` during
       dispatch (resolveApplyId, navigatePath's Apply step,
       dispatchApplyLive) re-route through `OuterObject::queryApply
       → applyFn → OuterApply::run` and would each fire a fresh
       `createCallbackCell` on the writer if not suppressed. Each fresh
       boundary inflates `envWalk` with a redundant ε edge
       beyond the genuine cb-apply events the recorder already
       captured. Suppress for the history's duration so writer's
       envWalk stays in 1:1 alignment with walker's
       envWalk. */
    TracingWriter::SuppressApplyBoundary suppressBoundary(writer);

    /* Register callback so suppressed createCallbackCell calls (=
       inner cb-apply boundaries fired inside dispatchApplyLive's
       cb-fn execution) synthesise a phantom ε obs in walker's
       envWalk. Cold's writer would have inserted these as ε
       edges into envWalk; without this walker's history-index
       falls short of cold's step for later flushes referencing
       arg(1) at post-inner-apply positions. */

    /* Per-walk scoping: envWalk (and committedEdgeFingerprints) belong
       to this walk only. Each walk builds its own history from ∅ via
       the Ask chain it traverses; the recorded chain determines what
       gets folded in. Under matching-until-divergence, walker's
       per-walk envWalk mirrors the writer's history at the time of
       the recording being matched — no cross-walk pollution.

       See `doc/design/tracing-eval-cache.md` §Replay strategies
       (slow path) for the reasoning. Save the outer scope's state
       so nested walks don't corrupt it. */
    auto savedEnvWalk = std::move(envWalk);
    envWalk.clear();
    auto savedFingerprints = std::move(committedEdgeFingerprints);
    committedEdgeFingerprints.clear();
    struct WalkScope
    {
        std::vector<ObservationSet> & envWalk;
        std::unordered_set<Hash> & committedEdgeFingerprints;
        std::vector<ObservationSet> savedEnvWalk;
        std::unordered_set<Hash> savedFingerprints;
        ~WalkScope()
        {
            envWalk = std::move(savedEnvWalk);
            committedEdgeFingerprints = std::move(savedFingerprints);
        }
    } walkScope{envWalk, committedEdgeFingerprints,
                std::move(savedEnvWalk), std::move(savedFingerprints)};

    ResolutionContext ctx{
        std::move(currentProxy),
        {},
    };
    /* Per-edge buffer: dispatch() appends ambient facts here; the
       history-loop promotes the buffer to a cumulative envWalk
       edge on commit (via commitEdge) or discards it on reject.
       Without the buffer, rejected-edge facts would pollute
       envWalk and throw off the cell-chain state hash computations. */
    std::vector<Observation> pendingEdgeObservations;

    auto commitEdge = [&]() {
        /* 1:1 alignment with writer's envWalk: writer inserts each
           cb-apply's ε obs as a SEPARATE env edge at its
           `insertionIndex`, not bundled with the real-obs edge that
           triggered it. Walker's dispatch() pushes ε obs (fromHash=0)
           into `pendingEdgeObservations` alongside real obs of the
           same Asks edge — we need to split them at commit time so
           each ε lives in its own edge, matching writer's layout.

           Split: partition pending obs into ε (fromHash=0) and real
           (non-zero fromHash). Commit real obs as the primary edge;
           each ε obs becomes its own subsequent edge. */
        std::vector<Observation> realObs;
        std::vector<Observation> epsilonObs;
        realObs.reserve(pendingEdgeObservations.size());
        for (auto & obs : pendingEdgeObservations) {
            if (obs.fromHash == Hash(HashAlgorithm::SHA256))
                epsilonObs.push_back(std::move(obs));
            else
                realObs.push_back(std::move(obs));
        }
        pendingEdgeObservations.clear();

        auto tryPush = [&](std::vector<Observation> obs) {
            if (obs.empty()) {
                tracingCacheLog("dispatch: edge empty, skip commit");
                return;
            }
            Hash fingerprint(HashAlgorithm::SHA256);
            for (const auto & f : obs)
                fingerprint = TracingDecisionGraph::xorFactIntoHash(
                    fingerprint, f.fromHash, f.elementHash);
            if (committedEdgeFingerprints.insert(fingerprint).second) {
                ObservationSet edge;
                edge.observations = std::move(obs);
                envWalk.push_back(std::move(edge));
                tracingCacheLog("dispatch: committed edge, envWalk=%zu (obs=%zu)",
                                envWalk.size(), envWalk.back().observations.size());
            } else {
                tracingCacheLog("dispatch: edge already in envWalk (shared prefix), skip");
            }
        };

        tryPush(std::move(realObs));
        for (auto & obs : epsilonObs)
            tryPush({std::move(obs)});
    };

    /* Dispatcher: turns a Request hash into the current Response
       hash. Memoised in responseFor for stable requests (file
       reads, env vars) where same request always gives same
       response. Ambient queries are NOT memoised because the same
       request hash can dispatch to different responses depending on
       which proxy (cb invocation) the history is grounded in — sibling
       cb apply invocations of the same fn share a request hash but
       must see their own arg's live value, not a memoised sibling's. */
    auto dispatch = [&](const Hash & requestHash, const TracingDecisionGraph::EdgeContext & edgeCtx) -> Hash {
        auto requestPayload = decisionGraph.getRequestPayload(requestHash);
        if (!requestPayload)
            return Hash(HashAlgorithm::SHA256);
        bool isAmbient = false;
        std::optional<Hash> outerFromHash;
        std::string queryTag;
        std::string queryDescription;
        try {
            auto reqJson = cborStringToJson(*requestPayload);
            isAmbient = reqJson.contains("query");
            if (isAmbient) {
                queryTag = reqJson["query"].get<std::string>();
                queryDescription = queryTag;
                if (reqJson.contains("params") && reqJson["params"].is_object()) {
                    auto & params = reqJson["params"];
                    if (params.contains("from")) {
                        try {
                            outerFromHash = Hash::parseNonSRIUnprefixed(
                                params["from"].get<std::string>(), HashAlgorithm::SHA256);
                        } catch (...) {}
                    }
                    if (params.contains("name"))
                        queryDescription += " name=\"" + params["name"].get<std::string>() + "\"";
                    if (params.contains("index"))
                        queryDescription += " index=" + std::to_string(params["index"].get<size_t>());
                    if (queryTag == "apply") {
                        if (params.contains("fn"))
                            queryDescription += " fn=" + params["fn"].get<std::string>().substr(0, 12);
                        if (params.contains("arg"))
                            queryDescription += " arg=" + params["arg"].get<std::string>().substr(0, 12);
                    }
                }
            } else if (reqJson.contains("absPath")) {
                queryDescription = "env-file " + reqJson["absPath"].get<std::string>();
            } else if (reqJson.contains("name")) {
                queryDescription = "env-var " + reqJson["name"].get<std::string>();
            } else {
                queryDescription = "(opaque)";
            }
        } catch (...) {
            queryDescription = "(parse-failed)";
        }
        if (!isAmbient) {
            if (auto it = responseFor.find(requestHash); it != responseFor.end())
                return it->second;
        }
        /* Apply-boundary: AmbientResult split by chain presence.
            - No chain at applyReqHash: AmbientResult = applyReqHash
              (= chain root; matches writer's empty-ambient-group path).
            - Chain present: invoke fn live via dispatchApplyLive,
              which forces the result so outer's f drives probes
              against a fresh ReplayCallbackArg. On divergence, fail dispatch. */
        if (isAmbient && queryTag == "apply") {
            auto outgoing = decisionGraph.getAmbientAsks(requestHash);
            Hash applyRespHash{HashAlgorithm::SHA256};
            if (outgoing.empty()) {
                applyRespHash = requestHash;
            } else {
                nlohmann::json reqJson;
                try {
                    reqJson = cborStringToJson(*requestPayload);
                } catch (const std::exception &) {
                    return Hash(HashAlgorithm::SHA256);
                }
                if (!reqJson.contains("params") || !reqJson["params"].is_object())
                    return Hash(HashAlgorithm::SHA256);
                auto maybeAmbientResult = dispatchApplyLive(
                    requestHash, reqJson["params"], edgeCtx.fromFactSetHash, ctx);
                if (!maybeAmbientResult)
                    return Hash(HashAlgorithm::SHA256);
                applyRespHash = *maybeAmbientResult;
            }
            pendingEdgeObservations.push_back({
                Hash(HashAlgorithm::SHA256),
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, applyRespHash),
            });
            return applyRespHash;
        }
        auto currentResp = getCurrentResponse(*requestPayload, ctx);
        if (!currentResp) {
            tracingCacheLog(
                "dispatch FAIL req=%s payload=%s (no current response)",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription);
            return Hash(HashAlgorithm::SHA256);
        }
        auto h = TracingDecisionGraph::computeResponseHash(*currentResp);
        /* edgeCtx is threaded through walk() for offline-inspection
           consumers; env dispatch MUST NOT read stored responses to
           substitute for a live response that differs from cold's —
           doing so masks legitimate outer-body change detection (per
           the design's capability-mediated invariant) AND, even
           under `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`, breaks
           observation-driven sibling discrimination
           (cb-sibling-discrimination-via-observation): a wrong-sibling
           live response substituted with the FIRST-WRITER InnerValueResponse entry
           routes both siblings to the same recorded terminal, giving
           `200` instead of `100 + 1000 = 1100`. Only substitute on
           DISPATCH FAILURE (see the block above), not on mismatch. */
        (void) edgeCtx;
        if (!isAmbient)
            responseFor.emplace(requestHash, h);
        /* Walker-side dispatch is validation, not new recording.
           The observation being validated was already emitted by the
           original interpreter run (via logResponse or
           logOuterObservation) and lives in the pool. Feeding it back
           into the writer's cumulative envFactSet would conflate
           walker-validation activity with interpreter work — the
           recording session belongs to the Interpreter, not the
           walker. Under DISALLOW mode this pollution was structurally
           breaking slow-path reachability (phantom curs never keyed
           by any real writer emission). */
        /* Buffer ambient facts for this in-flight Asks edge; the
           history-loop commits them via onEdgeCommitted on success. */
        /* Decode for diffing: render the full request + response JSON
           bytes that feed `req` and `resp`. SHA256(reqJson.dump()) = req;
           SHA256(currentResp) = h. Diffing these strings between cold and
           warm is what isolates which exact (q, r) differs. */
        std::string reqJsonStr;
        try {
            reqJsonStr = cborStringToJson(*requestPayload).dump();
        } catch (...) {
            reqJsonStr = "(unparseable)";
        }
        std::string respJsonStr;
        try {
            respJsonStr = cborStringToJson(*currentResp).dump();
        } catch (...) {
            respJsonStr = "(unparseable)";
        }
        if (isAmbient && outerFromHash) {
            pendingEdgeObservations.push_back({
                *outerFromHash,
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, h),
            });
            tracingCacheLog(
                "dispatch ambient: req=%s payload=%s from=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                outerFromHash->to_string(HashFormat::Base16, false).substr(0, 12),
                h.to_string(HashFormat::Base16, false).substr(0, 12),
                reqJsonStr,
                respJsonStr);
        } else if (isAmbient) {
            tracingCacheLog(
                "dispatch ambient (no-from): req=%s payload=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                h.to_string(HashFormat::Base16, false).substr(0, 12),
                reqJsonStr,
                respJsonStr);
        } else {
            tracingCacheLog(
                "dispatch env: req=%s payload=%s resp=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                h.to_string(HashFormat::Base16, false).substr(0, 12));
        }
        return h;
    };

    /* Walk with two anchor candidates in order:
       1. Parent TracingReplayObject's terminalCur — the structural-anchor
          lookup position. Child Q's recording was made starting from
          parent's reached factSet, so anchoring the child history there
          matches the recording's frame.
       2. From ∅ — needed when no parent anchor exists (top-level Q
          like evalFile/evalExpr, no TracingReplayObject) and as a backstop
          when the parent-anchored attempt finds no matching Asks chain.

       Use `ctx.currentProxy` (not `currentProxy`) — the local was
       moved into ctx above, so it's now empty. */
    Hash parentAnchor = TracingDecisionGraph::emptySetHash();
    if (auto * parentTR = dynamic_cast<TracingReplayObject *>(ctx.currentProxy.get())) {
        parentAnchor = parentTR->getTriePos().factSetHash;
    }
    /* Track rejected-edge obs across all attempts. Committed on history
       MISS so subsequent history calls' resolveStateHash sees the obs
       walker produced during the failed traversal — those obs carry
       real (req, resp) pairs from cold's recorded responses, and
       future resolves at deeper step may need them. Only
       preserve on final miss; on hit, the winning edges are already
       committed and the rejected ones represent wrong branches whose
       obs would contaminate the correct chain. */
    std::vector<Observation> rejectedObs;
    auto commitRejected = [&](const std::vector<Hash> &) {
        for (auto & obs : pendingEdgeObservations)
            rejectedObs.push_back(std::move(obs));
        pendingEdgeObservations.clear();
    };
    /* Two structural attempts:
       (1) parent-anchored — walk from the parent TracingReplayObject's
           terminalCur, the lockstep continuation of the enclosing
           traversal.
       (2) walk-from-∅ fallback — used when (1) misses or when there
           is no parent anchor. */
    std::optional<TracingDecisionGraph::WalkHit> walkHit;
    walkHit = decisionGraph.walk(queryHash, dispatch,
        [&](bool committed, const std::vector<Hash> & useful) {
            if (committed) commitEdge();
            else commitRejected(useful);
        },
        parentAnchor);
    if (!walkHit && parentAnchor != TracingDecisionGraph::emptySetHash()) {
        walkHit = decisionGraph.walk(queryHash, dispatch,
            [&](bool committed, const std::vector<Hash> & useful) {
                if (committed) commitEdge();
                else commitRejected(useful);
            });
    }
    if (!walkHit) {
        /* Walker missed. Rejected-edge obs are NOT committed to
           envWalk: they represent wrong paths whose responses
           cold never recorded, so folding them into arg state hashes shifts
           subject_at_k to values cold never stamped. Per Asks-paradigm
           navigation invariant, state hashes are pure functions of the
           committed factset; rejected paths are not in that factset. */
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    auto payload = decisionGraph.getResultPayload(walkHit->resultHash);
    if (!payload) {
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    tracingCacheStats().hits++;
    return WalkResult{std::move(*payload), walkHit->resultHash, walkHit->terminalCur};
}

std::optional<std::string> TracingReplayEvaluator::getCurrentResponse(const std::string & requestCbor, ResolutionContext & ctx)
{
    try {
        auto reqJson = cborStringToJson(requestCbor);
        if (reqJson.contains("absPath")) {
            std::string path = reqJson["absPath"];
            auto currentHash = validationEnv.getFileHash(path);
            nlohmann::json respJson = trace::FileReadResponse{currentHash};
            return jsonToCborString(respJson);
        } else if (reqJson.contains("name")) {
            std::string name = reqJson["name"];
            auto currentVal = validationEnv.getEnv(name);
            nlohmann::json respJson = trace::GetEnvResponse{currentVal};
            return jsonToCborString(respJson);
        } else if (reqJson.contains("query")) {
            return dispatchAmbientQuery(reqJson, ctx);
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to get current response: %s", e.what());
    }
    return std::nullopt;
}

/* Resolve a recorded ambient id (hex of a Hash) to a live Object.
   First check the per-history memo (ctx.memo) for already-resolved ids.
   Then history the proxy graph (ctx.currentProxy.parent → …) looking
   for an argCell cell whose id matches — this is the arg-lookup
   case, grounded in the proxy whose method triggered this history
   rather than in any evaluator-global state.
   Then fall through to producer-Request resolution: find idStr in
   the Requests pool, resolve the parent recursively, dispatch the
   producer's query on the parent. QueryApply payloads invoke the
   live apply against a (frozen) ReplayCallbackArg arg. localArg
   sidecars chase to the apply. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveStateHash(const std::string & idStr, ResolutionContext & ctx)
{
    /* Per-history memo. */
    if (auto it = ctx.memo.find(idStr); it != ctx.memo.end()) {
        tracingCacheLog("resolve %s -> memo hit", idStr.substr(0, 12));
        return it->second;
    }


    /* Walk the proxy's argCell chain looking for a cell whose
       liveObject's state hash matches idStr at some k under
       walker's own envWalk. */
    std::vector<ObservationSet> extendedWalkForMatch = envWalk;
    auto cell = ctx.currentProxy ? ctx.currentProxy->getProxyArgCell() : nullptr;
    int cellDepth = 0;
    for (; cell; cell = cell->parent, ++cellDepth) {
        if (auto live = cell->liveObject) {
            if (auto * subj = live->getSubject()) {
                /* Use the live proxy's own inherited argAncestry so the
                   walker's state hash matches what the recorder
                   computed at this proxy at flush. */
                auto argAncestry = live->getArgAncestry();
                bool found = false;
                /* K=0 fast path — Asks-style initial state hash lookup:
                   subject's initial state hash
                   (before any observation folds in) is a pure
                   function of (subject, argAncestry). Walker computes it
                   as a key and checks equality against the target
                   — no iteration over K, no scanning for "which
                   walker-state produces target". Empirical: a
                   majority of cell-chain matches in the cb-* +
                   builtins-cache suite land at K=0.
                   Structurally an Asks-style navigation: walker's
                   own hashed state (initial state hash) IS the lookup
                   key. */
                {
                    /* No functional test in the current suite reaches
                       this loop with a DerivedSubject subj (verified by
                       instrumentation this session). The strict
                       stateHashAt would trap on Derived, so if the
                       assumption ever breaks — e.g. a future proxy
                       type registers a Derived-subject liveObject at
                       an ArgCell — we need to know. Assert to catch
                       that inversion; swap to stateHashAtSubject
                       under a real repro. */
                    assert(!std::holds_alternative<DerivedSubject>(subj->data)
                           && "resolveStateHash: DerivedSubject in cell-chain match — see task #68 investigation");
                    auto initialStateHash = stateHashAt(*subj, argAncestry, extendedWalkForMatch, 0);
                    if (initialStateHash.to_string(HashFormat::Base16, false) == idStr) {
                        found = true;
                    }
                }
                /* Subject-evolution fast-path walker-side trie navigation for K > 0.
                   Walker's current cur is its own hashed state (key);
                   walker looks up (subject, cur, obs.from, obs.elem)
                   in cold-recorded SubjectEvolutionEdges; if edge
                   exists, folds obs.elem into edge accumulator.
                   ObservationSet-scoped semantics (all obs in one edge check
                   against edge-entry cur) preserved.
                   Empirical: every previously-iterated K-match is
                   also reached by trie navigation, so the loop's
                   K dimension is unnecessary here. */
                if (!found) {
                    Hash argSubjectHash = stateHashAt(
                        *subj, Hash(HashAlgorithm::SHA256), {}, 0);
                    Hash cur = stateHashAt(*subj, argAncestry, extendedWalkForMatch, 0);
                    for (const auto & edge : extendedWalkForMatch) {
                        if (found) break;
                        Hash edgeAcc(HashAlgorithm::SHA256);
                        for (const auto & obs : edge.observations) {
                            auto next = decisionGraph.getSubjectEvolutionEdge(
                                argSubjectHash, cur, obs.fromHash, obs.elementHash);
                            if (next)
                                edgeAcc = TracingDecisionGraph::xorHashes(edgeAcc, obs.elementHash);
                        }
                        cur = TracingDecisionGraph::xorHashes(cur, edgeAcc);
                        if (cur.to_string(HashFormat::Base16, false) == idStr) found = true;
                    }
                }
                /* Observation-permutation navigation, folded to its
                   fixed point. Formerly a multi-round loop that
                   checked the target at every round; instrumentation
                   across the full cb-* + builtins-cache suite showed
                   the loop fires once and its winning round equals
                   the converged fixed point. Replaced with a single
                   call to
                   `stateHashConverged`, which is order- and
                   grouping-independent by construction: walker's
                   convergence value depends only on the SET of
                   observations, not on edge boundaries. Cold's
                   `stateHashAtStamping` stamps SubjectEvolutionEdges
                   trie rows in the same order-invariant way when the
                   subject's stamp state coincides with the fixed
                   point — so both sides reach the same hash on the
                   aligned pool without a per-round hash comparison.

                   The former loop is preserved semantically because
                   `stateHashConverged` iterates the same greedy
                   partition internally, but the caller no longer sees
                   intermediate rounds — the search is a single call.

                   Handles the permuted-order case (cb-385's 5-round
                   evolution): where walker's envWalk carries the
                   same observations as cold's envWalk but the
                   edge boundaries differ, only the fixed point is
                   grouping-invariant and thus safe to compare. */
                if (!found && !extendedWalkForMatch.empty()) {
                    Hash converged = stateHashConverged(
                        *subj, argAncestry, extendedWalkForMatch);
                    if (converged.to_string(HashFormat::Base16, false) == idStr)
                        found = true;
                }
                if (found) {
                    tracingCacheLog(
                        "resolve %s: cell[%d] subject=%s MATCH",
                        idStr.substr(0, 12), cellDepth, describe(*subj));
                    ctx.memo[idStr] = live;
                    return live;
                }
                tracingCacheLog(
                    "resolve %s: cell[%d] subject=%s miss across %zu edges (+collected)",
                    idStr.substr(0, 12), cellDepth,
                    describe(*subj), envWalk.size() + 1);
            } else {
                tracingCacheLog("resolve %s: cell[%d] live has no subject", idStr.substr(0, 12), cellDepth);
            }
        } else {
            tracingCacheLog("resolve %s: cell[%d] no liveObject", idStr.substr(0, 12), cellDepth);
        }
    }
    tracingCacheLog("resolve %s: cell-chain exhausted, falling through to pool", idStr.substr(0, 12));

    Hash idHash{HashAlgorithm::SHA256};
    try {
        idHash = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        return nullptr;
    }

    auto reqPayload = decisionGraph.getRequestPayload(idHash);
    if (!reqPayload) {
        /* "Not in pool" means the id has no recorded provenance — no
           producer Request and no localArg sidecar. Such ids are
           OUTER-direction by elimination: an inner local's argSubject is
           always sidecar-registered by OuterResolver::apply (=
           inserting `{kind: "localArg", applyResultId: ...}` at the
           argSubject), and any derived value has a producer Request. Only
           outer-arg state hashes minted by makeCachedFnPrimOp.impl — e.g.
           a nested OuterObject for the int the callback body passes
           to inner_lambda in cb-higher-order's `g 10` — reach here.

           Live-proxy fallback: the `<replay-local-lambda>` primop
           registers the args[0] it receives under the cb-arg arg's
           initial state hash when fired (= registerAmbientResolverProxy in
           replay-callback-arg.cc). If we find a matching registration
           here, the OUTER walker resolves to that live proxy and
           dispatches the env fact live against outer's actual value
           — capability-mediated, not cached. This closes the arg-
           resolution gap that otherwise kills cb-higher-order's
           DISALLOW_PARSE warm-replay steps.

           Without a registration, fall through to nullptr. The via-
           Asks design forbids serving from the Responses pool for
           OUTER values ("ambient responses are capability-mediated,
           not cached" — primop doc §Replay semantics); the previous
           fallback materialised an ReplayCallbackArg and let its methods read out
           of InnerValueResponse, which was correct for INNER locals but
           wrong here: it served the recorded outer response regardless
           of whether the live outer would produce it, silently masking
           outer-body change (cb-higher-order step 3 returning stale 6
           when outer changed from `g 5` to `g 10`).

           INNER locals are unaffected by this change: their sidecar
           presence routes them via `chaseLocalArgSidecar`, and
           `resolveApplyId` with explicit `isLocalArgId`
           discrimination materialises their ReplayCallbackArg. Serving inner
           locals from the reconstructed value tree backed by
           InnerValueResponse is per design (= ambient layer Replay's
           "walker reconstructs the LocalObject as a live Nix Value
           tree from the CAS pool"). The forbidden thing is treating
           an OUTER-direction id as if it were a local. */
        if (auto resolver = inner->getAmbientResolver()) {
            if (auto live = tryResolveAmbientResolverProxy(*resolver, idHash, envWalk, &decisionGraph)) {
                tracingCacheLog(
                    "resolve %s: not in pool — found live-proxy registration",
                    idStr.substr(0, 12));
                ctx.memo[idStr] = live;
                return live;
            }
        }
        tracingCacheLog(
            "resolve %s: not in pool — no provenance (outer-arg by elimination); returning null",
            idStr.substr(0, 12));
        return nullptr;
    }

    nlohmann::json reqJson;
    try {
        reqJson = cborStringToJson(*reqPayload);
    } catch (const std::exception &) {
        tracingCacheLog("resolve %s: pool payload parse failed", idStr.substr(0, 12));
        return nullptr;
    }

    if (reqJson.contains("kind") && reqJson["kind"] == "localArg") {
        tracingCacheLog("resolve %s: localArg sidecar", idStr.substr(0, 12));
        auto applyResultIdHex = reqJson["applyResultId"].get<std::string>();
        resolveStateHash(applyResultIdHex, ctx);
        if (auto it = ctx.memo.find(idStr); it != ctx.memo.end())
            return it->second;
        return nullptr;
    }

    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    if (tag == "apply") {
        tracingCacheLog("resolve %s: apply producer", idStr.substr(0, 12));
        return resolveApplyId(idStr, params, ctx);
    }

    std::string selector;
    if (params.contains("name")) selector = " name=\"" + params["name"].get<std::string>() + "\"";
    else if (params.contains("index")) selector = " index=" + std::to_string(params["index"].get<size_t>());
    tracingCacheLog(
        "resolve %s: producer-child via %s from %s%s",
        idStr.substr(0, 12), tag,
        params.contains("from") ? params["from"].get<std::string>().substr(0, 12) : std::string("?"),
        selector);
    return resolveProducerChild(idStr, tag, params, ctx);
}

bool TracingReplayEvaluator::isLocalArgId(const Hash & idHash)
{
    auto reqPayload = decisionGraph.getRequestPayload(idHash);
    if (!reqPayload)
        return true;
    try {
        auto j = cborStringToJson(*reqPayload);
        return j.contains("kind") && j["kind"] == "localArg";
    } catch (const std::exception &) {
        return true;
    }
}

/* Local-direction: unknown id in the Requests pool — most commonly an
   inner-side TracingCallbackArg's content-hash whose facts were emitted
   with from=hex(id) but whose id itself isn't a producer Request.
   Materialise a ReplayCallbackArg keyed by it; its methods read
   recorded responses out of InnerValueResponse by qH(query{from=hex(id)}),
   matching what TracingCallbackArg wrote during recording. */
/* Mixed direction: fn is Outer (resolved through the producer chain to
   an OuterObject); arg may be Local (ReplayCallbackArg) or Outer (resolved
   through chain). Invokes the apply live against fn and arg to
   materialise the apply result; OuterObject::queryApply registers the
   result in outerValues. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveApplyId(
    const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx)
{
    auto fnObj = resolveStateHash(params["fn"].get<std::string>(), ctx);
    if (!fnObj) {
        tracingCacheLog("replay: apply %s: cannot resolve fn %s", idStr, params["fn"]);
        return nullptr;
    }
    auto argIdStr = params["arg"].get<std::string>();
    Hash argHash{HashAlgorithm::SHA256};
    try {
        argHash = Hash::parseNonSRIUnprefixed(argIdStr, HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        return nullptr;
    }
    std::shared_ptr<Object> argObj;
    if (isLocalArgId(argHash)) {
        /* The cb apply's local arg. Read the localArg sidecar to
           source the cb-arg's structural subject (depth + argAncestry)
           and construct the ReplayCallbackArg with `Arg{depth}`
           — matching the recorder's TracingCallbackArg subject.

           Do NOT use `PostulatedIdempotentRead{localId}` here.
           `PostulatedIdempotentRead`'s state hash is constant in `k`
           (= no own-loop evolution), so once the ReplayCallbackArg's first
           probe extends the chain, every subsequent probe's
           `stampPerArgFields` reads back `localId` instead of the
           subject-id-evolved state hash the recorder stamped its facts
           against. The recorded reqHashes then can't be found in
           InnerValueResponse → cb-sibling fails with
           "no recorded response for getType on local". Both
           sibling cb-applies share the same first probe's stamped
           reqHash regardless of subject (= at step=0,
           Arg and PostulatedIdempotentRead both yield `localId`),
           which is why this bug stayed latent until cb-sibling
           landed: it's the first test that needs the ReplayCallbackArg's
           state hash to *evolve* via subsequent probes for downstream
           discrimination.

           Opt into ambient layer per-probe validation (= each probe
           must appear in some recorded AmbientAsks edge's
           requestSet, or we throw divergence) and root the chain
           at applyReqHash — different cb-applies' chains live in
           disjoint AmbientAsks subtrees; `idStr` IS this apply's
           chain root. */
        auto sidecarPayload = decisionGraph.getRequestPayload(argHash);
        std::shared_ptr<Object> replayObj;
        if (sidecarPayload) {
            try {
                auto sidecarJson = cborStringToJson(*sidecarPayload);
                if (sidecarJson.contains("depth") && sidecarJson.contains("argAncestry")) {
                    auto sidecarDepth = sidecarJson["depth"].get<int>();
                    auto sidecarScope = Hash::parseNonSRIUnprefixed(
                        sidecarJson["argAncestry"].get<std::string>(), HashAlgorithm::SHA256);
                    Subject rootSubject{Arg{sidecarDepth}};
                    /* Pre-emptive ReplayCallbackArg constructed
                       during memo-cache resolveStateHash. The
                       contextHash it should use is the walker's
                       Env cur at the moment this arg will actually
                       be used (i.e. at boundary open), which isn't
                       known here. Under lockstep the writer inserts
                       InnerValueResponse at the design contextHash
                       computed from (outerCur, walkerCur-at-open),
                       so this fallback's ReplayCallbackArg cannot
                       reach those rows via a walker-cur snapshot
                       taken at construction time. Pass zero as the
                       contextHash — if the arg is actually consumed
                       later, the read will miss and the caller
                       will fall through to inner re-eval, which is
                       the same outcome as a genuine miss. */
                    Hash fallbackContextHash(HashAlgorithm::SHA256);
                    auto rlo = std::make_shared<ReplayCallbackArg>(
                        std::move(rootSubject), sidecarScope,
                        std::make_shared<std::vector<ObservationSet>>(),
                        std::make_shared<Hash>(HashAlgorithm::SHA256),
                        fallbackContextHash, decisionGraph, inner->getEvalState().rootFSRoot,
                        &inner->getEvalState());
                    rlo->withAmbientAsksValidation();
                    try {
                        rlo->withChainStart(
                            Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256));
                    } catch (const std::exception &) {
                        /* idStr should be a valid hex hash here; if not,
                           leave chainCursor at its default
                           (emptySetHash) — the history will fail safely. */
                    }
                    rlo->withApplyContext(sidecarDepth, sidecarScope);
                    replayObj = rlo;
                    ctx.memo[argIdStr] = replayObj;
                }
            } catch (const std::exception &) {
                /* Sidecar malformed — fall through to the PostulatedIdempotentRead
                   fallback below. */
            }
        }
        /* Missing or malformed sidecar = the recorder didn't supply
           the depth/argAncestry needed to reconstruct the cb-arg's
           Arg Subject. Signal resolution failure so the
           caller falls through to inner re-eval. The previous
           PostulatedIdempotentRead fallback violated principle 8's corollary
           (= observation-driven evolution) and produced a ReplayCallbackArg
           whose discrimination was frozen at step=0. */
        argObj = replayObj;
    } else {
        argObj = resolveStateHash(argIdStr, ctx);
    }
    if (!argObj)
        return nullptr;
    ctx.memo[argIdStr] = argObj;
    std::shared_ptr<Object> resultObj;
    try {
        resultObj = fnObj->queryApply(argObj);
    } catch (const std::exception & e) {
        tracingCacheLog("replay: apply %s: queryApply threw: %s", idStr, e.what());
        return nullptr;
    }
    if (resultObj)
        ctx.memo[idStr] = resultObj;
    return resultObj;
}

std::optional<Hash> TracingReplayEvaluator::dispatchApplyLive(
    const Hash & applyReqHash,
    const nlohmann::json & params,
    const Hash & walkerCur,
    ResolutionContext & ctx)
{
    if (provenanceEnabled())
        recordProvenance(applyReqHash, "dispatchApplyLive-entry",
                         {{"walkerCur", walkerCur.to_string(HashFormat::Base16, false)},
                          {"params", params}});
    /* contextHash is the walker's Env `cur` at the time the response
       was recorded (vocab, "Ambient payload types and edges"). Here
       that value is `walkerCur` — the boundary Ask edge's
       `fromFactSetHash` supplied by the caller. Under principle 7's
       1:1 alignment it equals the writer's `contextCur` at the
       corresponding pending cb-apply. Cross-cached-call
       disambiguation is handled upstream by `callArgAncestry` inside
       `applyReqHash`. */
    Hash boundaryContextHash = walkerCur;
    auto fnIdStr = params["fn"].get<std::string>();
    auto fnObj = resolveStateHash(fnIdStr, ctx);
    if (!fnObj) {
        tracingCacheLog(
            "dispatchApplyLive: cannot resolve fn %s for applyReqHash=%s",
            fnIdStr,
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }

    auto argIdStr = params["arg"].get<std::string>();
    Hash argHash{HashAlgorithm::SHA256};
    try {
        argHash = Hash::parseNonSRIUnprefixed(argIdStr, HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        tracingCacheLog(
            "dispatchApplyLive: cannot parse arg id %s", argIdStr);
        return std::nullopt;
    }
    if (!isLocalArgId(argHash)) {
        tracingCacheLog(
            "dispatchApplyLive: arg %s is not a local; no ambient ReplayCallbackArg to drive",
            argIdStr.substr(0, 12));
        return std::nullopt;
    }

    /* Cycle break (interim): the live invocation below can still
       trigger walker re-entry through nested cached-fn impls (=
       inside the cb body's `<cached-fn>` on a TracingCallbackArg). Until that path
       is also rewired, short-circuit re-entries to chain root. */
    if (!inFlightApplyReqs.insert(applyReqHash).second) {
        tracingCacheLog(
            "dispatchApplyLive: re-entry for applyReqHash=%s — return chain root",
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return applyReqHash;
    }
    struct InFlightGuard {
        std::unordered_set<TracingDecisionGraph::RequestHash> & set;
        Hash key;
        ~InFlightGuard() { set.erase(key); }
    } guard{inFlightApplyReqs, applyReqHash};

    /* Fresh per-dispatch ReplayCallbackArg for the inner-supplied
       value. Per via-Asks Replay (ambient layer): the walker reconstructs
       the LocalObject as a live Nix Value tree (= lazily produced
       from CAS atoms), hands it to outer's f, and lets f run
       natively. For lambda LocalObjects, the `<replay-local-lambda>`
       primop the ReplayCallbackArg produces consults AmbientAsks at apply-time.
       Per-call discipline: each cb-apply Fact dispatch creates its
       own ReplayCallbackArg; no ctx.memo lookup. */
    /* Read the writer's localArg sidecar at argHash. depth+argAncestry are
       required: the structural subject (= Arg{depth} at
       argAncestry) evolves with observations on cb_arg the same way the
       writer did, which is what makes the synthetic's apply-result
       CAS reads find the recorded facts. */
    auto sidecarPayload = decisionGraph.getRequestPayload(argHash);
    if (!sidecarPayload)
        throw Error(
            "dispatchApplyLive: no localArg sidecar at argHash=%s",
            argHash.to_string(HashFormat::Base16, false));
    auto sidecarJson = cborStringToJson(*sidecarPayload);
    auto sidecarDepth = sidecarJson["depth"].get<int>();
    auto sidecarScope = Hash::parseNonSRIUnprefixed(
        sidecarJson["argAncestry"].get<std::string>(), HashAlgorithm::SHA256);

    Subject rootSubject{Arg{sidecarDepth}};
    /* Sibling-discriminating walkFacts arg: inject walker's
       currentProxy's applyContext observations into the ReplayCallbackArg's initial
       history. Without this, ReplayCallbackArg's per-arg fields are computed against
       an empty history — so sibling A's ReplayCallbackArg and sibling B's ReplayCallbackArg have
       identical state hashes at their initial `.x` / `.f` probes, and InnerValueResponse's
       first-writer-wins returns whichever sibling recorded first,
       yielding cross-sibling data mixing (cb-sibling-b's int-1000
       result = sibling A's x=1 folded with sibling B's f×1000).
       Injecting the current sibling's applyContext obs makes the
       ReplayCallbackArg's state hashes reflect the SPECIFIC sibling context walker is
       operating under. */
    auto seededWalkFacts = std::make_shared<std::vector<ObservationSet>>();
    if (auto * proxyTR = dynamic_cast<TracingReplayObject *>(ctx.currentProxy.get())) {
        if (auto proxyCtx = proxyTR->getApplyContext()) {
            for (auto & obs : proxyCtx->observations) {
                ObservationSet edge;
                edge.observations.push_back(obs);
                seededWalkFacts->push_back(std::move(edge));
            }
        }
    }
    auto replayLocal = std::make_shared<ReplayCallbackArg>(
        std::move(rootSubject), sidecarScope,
        seededWalkFacts,
        std::make_shared<Hash>(HashAlgorithm::SHA256),
        boundaryContextHash, decisionGraph, inner->getEvalState().rootFSRoot,
        &inner->getEvalState());
    replayLocal->withApplyContext(sidecarDepth, sidecarScope);
    replayLocal->withAmbientAsksValidation().withChainStart(applyReqHash);

    /* Invoke outer's f LIVE via the Object-level apply entry. Object-
       level apply preserves the ReplayCallbackArg replayLocal as an Object through
       the bridging chain (= OuterObject::queryApply → applyFn →
       resolver->apply → runOn sees argObj as the ReplayCallbackArg, NOT as an
       InterpreterObject wrapping a primop Value). That is what lets
       Change B's TracingCallbackArg-skip kick in and lets outer's `g 5` fire the
       ReplayCallbackArg's primop directly instead of routing through a
       `<cached-fn>(TracingCallbackArg)` cascade that bypasses the ambient lambda-LO
       mechanism. The earlier Value-level `mkApp + force` path lost
       the ReplayCallbackArg's Object-ness behind two layers of Value wrapping.
       Divergence (= ambient layer mismatch thrown out of the ReplayCallbackArg's
       primop, or an outer-side query failure) is caught and signaled
       as nullopt — the surrounding walker treats this as a miss. */
    std::shared_ptr<Object> resultObj;
    try {
        resultObj = fnObj->queryApply(replayLocal);
    } catch (const std::exception & e) {
        tracingCacheLog(
            "dispatchApplyLive: divergence at queryApply for applyReqHash=%s: %s",
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
            e.what());
        return std::nullopt;
    }
    if (!resultObj) {
        tracingCacheLog(
            "dispatchApplyLive: queryApply returned null for applyReqHash=%s",
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return std::nullopt;
    }
    /* Force via getType so the apply result evaluates to WHNF; that
       triggers outer's f.body running, which drives replayLocal's
       probes. */
    try {
        (void) resultObj->getType();
    } catch (const std::exception & e) {
        tracingCacheLog(
            "dispatchApplyLive: divergence forcing apply-result for applyReqHash=%s: %s",
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
            e.what());
        return std::nullopt;
    }

    auto ambientResult = replayLocal->getChainCursor();
    tracingCacheLog(
        "dispatchApplyLive: applyReqHash=%s AmbientResult=%s",
        applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12),
        ambientResult.to_string(HashFormat::Base16, false).substr(0, 12));

    /* Correctness-first cb-repeated fix: memoise the ReplayCallbackArg at
       BOTH the arg leaf's evolved state hash (invariant across invocations
       in the outer history — kept for chaseLocalArgSidecar-alignment)
       AND at the fn leaf's evolved state hash at THIS invocation (which
       DOES differ per invocation because arg(1)_evolved captures
       the outer history's per-boundary ε folds). Cold's outer probe
       recorded `from = fromStateHashes[0]` which is the FIRST root's cid;
       for `applyResult(getAttr(arg(1), "cb"), arg(N))` that first
       root is arg(1). So walker's dispatch of `getWHNF from=X`
       calls resolveStateHash(X = arg(1)_evolved) — memoising a
       correction here would break other arg(1) resolutions.

       Instead memoise at BOTH the LEAF (arg root) and at the
       applyResult subject's evolved cid, so any downstream lookup
       via those cids finds THIS invocation's ReplayCallbackArg. Compute the
       applyResult subject's evolved cid using fnObj's subject +
       Arg{sidecarDepth} as arg. */
    {
        Subject argSubject{Arg{sidecarDepth}};
        Hash evolvedLeafStateHash = stateHashAt(
            argSubject, sidecarScope, envWalk, envWalk.size());
        auto evolvedLeafStateHashHex = evolvedLeafStateHash.to_string(HashFormat::Base16, false);
        ctx.memo[evolvedLeafStateHashHex] = replayLocal;
        tracingCacheLog(
            "dispatchApplyLive: memoised ReplayCallbackArg at leaf cid %s (history.size=%zu, boundaryContextHash=%s)",
            evolvedLeafStateHashHex.substr(0, 12), envWalk.size(),
            boundaryContextHash.to_string(HashFormat::Base16, false).substr(0, 12));

        if (auto * fnSubj = fnObj->getSubject()) {
            Subject applyResultSubj{ApplyResultSubject{
                .fn = std::make_shared<const Subject>(*fnSubj),
                .arg = std::make_shared<const Subject>(std::move(argSubject)),
            }};
            Hash applyArgAncestryForStateHash = fnObj->getArgAncestry();
            Hash evolvedApplyResultStateHash = stateHashAt(
                applyResultSubj, applyArgAncestryForStateHash, envWalk, envWalk.size());
            auto evolvedApplyResultCidHex =
                evolvedApplyResultStateHash.to_string(HashFormat::Base16, false);
            ctx.memo[evolvedApplyResultCidHex] = replayLocal;
            tracingCacheLog(
                "dispatchApplyLive: memoised ReplayCallbackArg at applyResult cid %s (history.size=%zu)",
                evolvedApplyResultCidHex.substr(0, 12), envWalk.size());
        }
    }
    return ambientResult;
}

/* Outer-direction: derived child id whose producer Request is a
   navigation step (getAttr / getListElem). Resolve parent through the
   producer chain, then perform the live navigation step on it. */
/* Per-arg path navigation with multi-root support. `roots` are the
   live Objects corresponding to the query's `fromStateHashes[]` entries (=
   each entry is a cb_arg's ReplayCallbackArg). The top-level path navigates
   from `roots[0]`; Apply steps reach into `roots` by index via
   their `fnRootIndex` / `argRootIndex` so higher-order applies (=
   fn from one cb_arg, arg from another) work. */
static std::shared_ptr<Object> navigatePath(
    const std::vector<std::shared_ptr<Object>> & roots, const trace::PathExpr & path)
{
    if (roots.empty())
        return nullptr;
    std::shared_ptr<Object> obj = roots[0];
    for (auto & step : path.steps) {
        if (!obj)
            return nullptr;
        if (step.kind == trace::PathStep::Kind::GetAttr) {
            obj = obj->maybeGetAttr(step.name);
        } else if (step.kind == trace::PathStep::Kind::GetListElem) {
            obj = obj->getListElem(step.index);
        } else if (step.kind == trace::PathStep::Kind::Apply) {
            if (!step.fnPath || !step.argPath)
                return nullptr;
            if (step.fnRootIndex >= roots.size() || step.argRootIndex >= roots.size())
                return nullptr;
            /* fn and arg sub-paths each navigate from their own
               root entry. Walker mirrors the writer's pathAndRoots
               builder. */
            std::vector<std::shared_ptr<Object>> fnRoots{roots[step.fnRootIndex]};
            std::vector<std::shared_ptr<Object>> argRoots{roots[step.argRootIndex]};
            auto fnObj = navigatePath(fnRoots, *step.fnPath);
            auto argObj = navigatePath(argRoots, *step.argPath);
            if (!fnObj || !argObj)
                return nullptr;
            try {
                obj = fnObj->queryApply(std::move(argObj));
            } catch (const std::exception & e) {
                tracingCacheLog("navigatePath: queryApply failed: %s", e.what());
                return nullptr;
            }
        } else {
            return nullptr;
        }
    }
    return obj;
}

static trace::PathExpr parsePathFromParams(const nlohmann::json & params)
{
    trace::PathExpr path;
    if (params.contains("path"))
        from_json(params.at("path"), path);
    return path;
}

/* Resolve the query's roots: prefer `fromStateHashes[]` if present (=
   per-arg multi-root), fall back to the legacy single `from` field.
   Returns empty vector on resolution failure for any root. */
static std::vector<std::shared_ptr<Object>> resolveRoots(
    const nlohmann::json & params,
    std::function<std::shared_ptr<Object>(const std::string &)> resolve)
{
    std::vector<std::shared_ptr<Object>> roots;
    if (params.contains("fromStateHashes")) {
        for (auto & cid : params["fromStateHashes"]) {
            std::string cidHex;
            if (cid.is_string())
                cidHex = cid.get<std::string>();
            else if (cid.is_object() && cid.contains("content"))
                cidHex = cid["content"].get<std::string>();
            else
                return {};
            auto obj = resolve(cidHex);
            if (!obj)
                return {};
            roots.push_back(std::move(obj));
        }
        return roots;
    }
    if (params.contains("from")) {
        auto obj = resolve(params["from"].get<std::string>());
        if (!obj)
            return {};
        roots.push_back(std::move(obj));
    }
    return roots;
}

std::shared_ptr<Object> TracingReplayEvaluator::resolveProducerChild(
    const std::string & idStr, const std::string & tag, const nlohmann::json & params, ResolutionContext & ctx)
{
    if (!params.contains("from") && !params.contains("fromStateHashes"))
        return nullptr;

    /* Per-arg multi-root: resolve each fromStateHashes[] entry to a live
       cb_arg ReplayCallbackArg, then navigate. The producer query records the
       path-to-parent in `path`; navigation uses both. */
    auto roots = resolveRoots(params,
        [&](const std::string & cid) { return resolveStateHash(cid, ctx); });
    if (roots.empty())
        return nullptr;
    auto parent = navigatePath(roots, parsePathFromParams(params));
    if (!parent)
        return nullptr;

    std::shared_ptr<Object> child;
    try {
        if (tag == "getAttr") {
            child = parent->maybeGetAttr(params["name"].get<std::string>());
        } else if (tag == "getListElem") {
            child = parent->getListElem(params["index"].get<size_t>());
        } else {
            return nullptr;
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to resolve %s producer for %s: %s", tag, idStr, e.what());
        return nullptr;
    }

    if (child)
        ctx.memo[idStr] = child;
    return child;
}

std::optional<std::string> TracingReplayEvaluator::dispatchAmbientQuery(const nlohmann::json & reqJson, ResolutionContext & ctx)
{
    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    /* Apply Facts are recorded via Request only (no Terminal); the
       dispatcher has nothing to compare a current response against. */
    if (tag == "apply")
        return std::nullopt;

    /* Task #103: outer probes that reach through an applyResult carry
       a `callbackApply` slot. Materialise a fresh ReplayCallbackArg
       backed by the slot's obsSet, then pre-populate ctx.memo at the
       contra-arg's base state hash so the downstream resolveRoots /
       resolveStateHash / navigatePath pipeline picks it up. Fresh fn
       invocation per probe — no memoisation of the resulting Object. */
    if (params.contains("callbackApply")) {
        trace::CallbackApplyRef ref;
        try {
            params.at("callbackApply").get_to(ref);
        } catch (const std::exception &) {
            return std::nullopt;
        }
        Hash obsSetHash{HashAlgorithm::SHA256};
        Hash argAncestry(HashAlgorithm::SHA256);
        try {
            obsSetHash = Hash::parseNonSRIUnprefixed(ref.argObsSet, HashAlgorithm::SHA256);
            if (!ref.argAncestry.empty())
                argAncestry = Hash::parseNonSRIUnprefixed(ref.argAncestry, HashAlgorithm::SHA256);
        } catch (const std::exception &) {
            return std::nullopt;
        }
        auto obsSet = decisionGraph.getObservationSet(obsSetHash);
        if (!obsSet)
            return std::nullopt;
        auto obsSetMap = std::make_shared<std::map<Hash, std::string>>();
        for (const auto & obs : *obsSet)
            obsSetMap->emplace(obs.queryHash, obs.responsePayload);
        Subject argSubject{Arg{ref.argDepth}};
        auto walkFacts = std::make_shared<std::vector<ObservationSet>>();
        auto chainCursor = std::make_shared<Hash>(HashAlgorithm::SHA256);
        Hash zeroContext(HashAlgorithm::SHA256);
        auto replayArg = std::make_shared<ReplayCallbackArg>(
            std::move(argSubject), argAncestry,
            walkFacts, chainCursor, zeroContext,
            decisionGraph, inner->getEvalState().rootFSRoot,
            &inner->getEvalState());
        replayArg->withObsSetResponses(obsSetMap);
        auto argBaseId = stateHashAfter(Subject{Arg{ref.argDepth}}, argAncestry, {});
        ctx.memo[argBaseId.to_string(HashFormat::Base16, false)] = replayArg;
        tracingCacheLog(
            "callbackApply slot: materialised ReplayCallbackArg for argDepth=%d obsSet=%s at baseId=%s",
            ref.argDepth, ref.argObsSet.substr(0, 12),
            argBaseId.to_string(HashFormat::Base16, false).substr(0, 12));
        /* Fall through — the query's own tag (getWHNF/getAttr/…) runs
           the normal resolveRoots+navigatePath pipeline below, which
           now finds the replayArg via ctx.memo. */
    }

    if (!params.contains("from"))
        return std::nullopt;


    /* Every ambient response must be live-validated, just like file
       reads and env vars. Resolve each fromStateHashes[] entry to a live
       Object (single-root falls back to `from`) and navigate by the
       recorded path. The query body (= leaf op like getAttr "x")
       then runs on the navigated child. */
    auto roots = resolveRoots(params,
        [&](const std::string & cid) { return resolveStateHash(cid, ctx); });
    if (roots.empty())
        return std::nullopt;
    auto obj = navigatePath(roots, parsePathFromParams(params));
    if (!obj)
        return std::nullopt;

    nlohmann::json resultJson;
    try {
        if (tag == "getWHNF") {
            resultJson = computeWHNFFromObject(*obj);
        } else if (tag == "getAttr") {
            auto name = params["name"].get<std::string>();
            auto child = obj->maybeGetAttr(name);
            if (!child) {
                resultJson = trace::ResultMaybeType{std::nullopt};
            } else {
                resultJson = trace::ResultMaybeType{std::optional<std::string>{objectTypeToString(child->getType())}};
            }
        } else if (tag == "getListElem") {
            auto index = params["index"].get<size_t>();
            auto child = obj->getListElem(index);
            resultJson = trace::ResultType{objectTypeToString(child->getType())};
        } else if (tag == "getFunctionInfo") {
            auto info = obj->getFunctionInfo();
            if (!info)
                resultJson = trace::ResultFunctionInfo{false, {}, false};
            else
                resultJson = trace::ResultFunctionInfo{true, info->formals, info->ellipsis};
        } else {
            return std::nullopt;
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: dispatch failed for %s: %s", tag, e.what());
        return std::nullopt;
    }
    return jsonToCborString(resultJson);
}

template<typename Q>
std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup(const Q & query, std::shared_ptr<Object> currentProxy)
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    auto walkResult = walk(queryHash, std::move(currentProxy));
    if (!walkResult)
        return std::nullopt;
    tracingCacheLog("replay hit: %s", Q::tag);
    return std::make_pair(
        walkResult->payload,
        TriePosition{
            .resultNodeHash = walkResult->resultNodeHash,
            .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            .factSetHash = walkResult->terminalCur,
        });
}

bool TracingReplayEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & TracingReplayEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & TracingReplayEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

EvalState & TracingReplayEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingReplayEvaluator::evalFile(const RootedPath & path, const std::string & displayPath)
{
    if (auto result = lookup(trace::QueryImport{displayPath})) {
        tracingCacheLog("replay hit: evalFile %s", displayPath);
        auto obj = make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
        /* Root cell for the cached value; mirrors TracingEvaluator's
           recording side. Observations the outer makes on this proxy
           (and navigation children that inherit this cell) absorb into
           the root, so cb apply cells opened with parent=this root
           carry the outer's intervening-observation state via XOR
           state-creep — distinguishing sibling cb invocations. */
        obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
        return obj;
    }
    tracingCacheLog("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    if (auto result = lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
        tracingCacheLog("replay hit: evalExpr");
        auto obj = make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
        obj->withArgCell(ArgCell::make(nullptr, obj.get_ptr()));
        return obj;
    }
    tracingCacheLog("replay miss: evalExpr");
    return inner->evalExpr(expr, basePath);
}

ref<Object> TracingReplayEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    return inner->evalExprLazy(expr, basePath);
}

ref<Object> TracingReplayEvaluator::mkString(const std::string & s) { return inner->mkString(s); }
ref<Object> TracingReplayEvaluator::mkInt(NixInt i) { return inner->mkInt(i); }
ref<Object> TracingReplayEvaluator::mkBool(bool b) { return inner->mkBool(b); }
ref<Object> TracingReplayEvaluator::mkPath(const RootedPath & path) { return inner->mkPath(path); }
ref<Object> TracingReplayEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return inner->mkAttrs(attrs);
}
ref<Object> TracingReplayEvaluator::getInternalPrimOp(const std::string & name)
{
    return inner->getInternalPrimOp(name);
}

ref<Object> TracingReplayEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    /* fn and arg must be cache-boundary proxies whose identity is
       content-defined: OuterObject (outer values reached by the
       inner), TracingObject / TracingReplayObject (cached values
       reached by the outer). No counter fallback — per the
       Principles section, identity outside the CLI is grounded in
       observation, not allocation order. If a non-proxy Object
       reaches here it's a wiring bug that has to be addressed at
       its construction site. */
    auto getId = [](Object & obj) -> std::string {
        if (auto hex = obj.getStateHashHex())
            return *hex;
        throw Error(
            "TracingReplayEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnStateHashStr = getId(*fn);
    auto argStateHashStr = getId(*arg);

    /* Outer-direction applies (= fn is an OuterObject) must NEVER
       be replayed from cache — the outer value's behaviour is the
       *only* thing that can change between cold and warm, so its
       apply-result must always go through live dispatch. The
       registry intercepts and the TracingReplayObject wrapper's
       lookupResult both serve recorded responses; both are wrong
       for outer-direction. Skip both: invoke fn->queryApply(arg)
       directly, return whatever the OuterObject yields.
       OuterObject's own queryFn/applyFn closures handle live
       dispatch + the outer-side validation chain. */
    if (auto * fnAmb = dynamic_cast<OuterObject *>(fn.get_ptr().get())) {
        (void) fnAmb;
        tracingCacheLog(
            "walker apply: outer-direction (fn is OuterObject) — live dispatch, no registry");
        auto result = fn->queryApply(arg.get_ptr());
        if (!result)
            throw Error("TracingReplayEvaluator::apply: outer-direction queryApply returned null");
        return ref<Object>(result);
    }

    /* Inner-direction applies: fn is a recorded/cached entity
       (TracingReplayObject from evalFile, TracingCallbackArg's
       counterparts, or an opaque state hash). Each call constructs a
       fresh wrapper. Sibling cb apply invocations share the same
       (fnId, argSubject) at the boundary by construction (= the arg's
       state hash is the same positional arg across siblings), so a
       cross-invocation registry keyed by the apply Request hash
       would last-write-wins and conflate sibling invocations'
       per-call observation state — exactly the anti-pattern the
       via-Asks doc's boundary-trace-only discipline calls out. */

    /* Build the ApplyResultSubject from fn/arg constituents — mirror
       of TracingEvaluator::apply. Use polymorphic `getSubject()` so
       apply-result wrappers (TracingReplayObject /
       TracingObject) expose their applyResultSubject as `fn` for
       further applies — their state hashes evolve via subject-id own-loop
       instead of being frozen by `PostulatedIdempotentRead{this.state hash}`. Fall
       back to PostulatedIdempotentRead only when no Subject is exposed
       (= atom whose state hash is fully determined at construction). */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnStateHashStr, HashAlgorithm::SHA256);
    auto argSubjectHash = Hash::parseNonSRIUnprefixed(argStateHashStr, HashAlgorithm::SHA256);

    Subject fnSubj = fn->getSubject()
        ? *fn->getSubject()
        : Subject{PostulatedIdempotentRead{fnIdHash}};

    Subject argSubject = arg->getSubject()
        ? *arg->getSubject()
        : Subject{PostulatedIdempotentRead{argSubjectHash}};

    /* Apply boundary's argAncestry combines fn's and arg's inherited scopes
       symmetrically but non-commutatively — mirrors the writer's
       formula in `TracingEvaluator::apply`. */
    Hash applyArgAncestry = combineArgAncestries(fn->getArgAncestry(), arg->getArgAncestry());

    Subject resultSubject{ApplyResultSubject{
        .fn = std::make_shared<const Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const Subject>(std::move(argSubject)),
    }};

    /* Walker mirror of TracingEvaluator::apply's option 2 evolution.
       Uses walker.envWalk (the cumulative committed history), which
       under the 1:1 alignment restructure matches writer.envWalk
       edge-for-edge once all prior cb-applies' chains have been
       dispatched. */
    auto & history = writer.getD1CidasksWalk();
    auto applyArgAncestryStateHash = stateHashAt(resultSubject, applyArgAncestry, history, history.size());
    auto applyArgAncestryStateHashHex = applyArgAncestryStateHash.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "walker apply: fn=%s arg=%s argAncestry=%s -> applyArgAncestryStateHash=%s",
            describe(*apr.fn),
            describe(*apr.arg),
            applyArgAncestry.to_string(HashFormat::Base16, false).substr(0, 12),
            applyArgAncestryStateHashHex.substr(0, 16));
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyArgAncestryStateHashHex,
    };
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, fn, arg]() { return inner->apply(fn, arg); });
    /* Apply-result argAncestry cell. Parent = fn proxy's cell. */
    auto cell = ArgCell::make(effectiveArgCell(*fn), arg.get_ptr());
    obj->withArgCell(std::move(cell));
    obj->withApplyResultSubject(std::move(resultSubject), applyArgAncestry);
    /* Keep the applyContext attachment for the ensureInner-finalisation
       side-channel that other paths still inspect (e.g. tests that
       check applyContext->finalized). Pre-population of observations
       from the Requests pool is no longer needed — evolvedQueryFrom
       reads the evaluator's envWalk instead. */
    if (auto * argAmb = dynamic_cast<OuterObject *>(arg.get_ptr().get())) {
        if (auto ctx = argAmb->getApplyContext())
            obj->withApplyContextOnly(std::move(ctx));
    }
    return obj;
}

} // namespace nix

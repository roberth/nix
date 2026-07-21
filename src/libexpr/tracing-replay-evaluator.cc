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
       callbackApply slot's live invocation) re-route through
       `OuterObject::queryApply → applyFn → OuterApply::run` and
       would each fire a fresh
       `createCallbackCell` on the writer if not suppressed. Each fresh
       boundary inflates `envWalk` with a redundant ε edge
       beyond the genuine cb-apply events the recorder already
       captured. Suppress for the history's duration so writer's
       envWalk stays in 1:1 alignment with walker's
       envWalk. */
    TracingWriter::SuppressApplyBoundary suppressBoundary(writer);

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
        /* Apply-boundary requests without a callbackApply slot have no
           live-fire dispatch path anymore — AmbientAsks was ripped out
           (task #109); the callbackApply slot mechanism carries the
           obsSet CAS reference and warm's dispatchAmbientQuery fires
           fn live via that path (see line ~1156). If we reach this
           branch, the request is a stale apply Fact from before the
           #103 cutover — miss cleanly. */
        if (isAmbient && queryTag == "apply") {
            tracingCacheLog(
                "dispatch: legacy apply Fact req=%s (no callbackApply slot) — miss",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12));
            return Hash(HashAlgorithm::SHA256);
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
           (cb-sibling-discrimination-via-observation): substituting
           a stored response for a wrong-sibling live response
           would route both siblings to the same recorded terminal.
           Only substitute on DISPATCH FAILURE (see the block above),
           not on mismatch. */
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

    std::vector<Observation> rejectedObs;
    auto commitRejected = [&](const std::vector<Hash> &) {
        for (auto & obs : pendingEdgeObservations)
            rejectedObs.push_back(std::move(obs));
        pendingEdgeObservations.clear();
    };

    /* === Fast path (task #106) ===
       Session-cumulative: look up `getAsks(Q, envCur)` and walk that
       specific known trace lockstep, using session-scoped envWalk. On
       hit, envWalk has been extended with this Q's Ask edges and
       envCur advanced to the terminalCur. On miss, roll back any
       partial commits and fall through to the slow path with per-walk
       scoping. */
    std::optional<TracingDecisionGraph::WalkHit> walkHit;
    {
        auto fastPathSavedEnvWalkSize = envWalk.size();
        auto fastPathSavedEnvCur = envCur;
        auto fastPathSavedFingerprints = committedEdgeFingerprints;
        walkHit = decisionGraph.walk(queryHash, dispatch,
            [&](bool committed, const std::vector<Hash> & useful) {
                if (committed) commitEdge();
                else commitRejected(useful);
            },
            envCur);
        if (walkHit) {
            auto payload = decisionGraph.getResultPayload(walkHit->resultHash);
            if (payload) {
                envCur = walkHit->terminalCur;
                tracingCacheStats().hits++;
                tracingCacheLog(
                    "fast path HIT queryHash=%s startCur=%s terminalCur=%s "
                    "(envWalk grew %zu -> %zu)",
                    queryHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    fastPathSavedEnvCur.to_string(HashFormat::Base16, false).substr(0, 12),
                    envCur.to_string(HashFormat::Base16, false).substr(0, 12),
                    fastPathSavedEnvWalkSize, envWalk.size());
                return WalkResult{std::move(*payload), walkHit->resultHash, walkHit->terminalCur};
            }
        }
        /* Fast path missed or Result payload absent. Roll back partial
           commits so the slow path starts with clean session state. */
        envWalk.resize(fastPathSavedEnvWalkSize);
        envCur = fastPathSavedEnvCur;
        committedEdgeFingerprints = std::move(fastPathSavedFingerprints);
        pendingEdgeObservations.clear();
        rejectedObs.clear();
        walkHit.reset();
    }

    /* === Slow path ===
       Per-walk scoping: save session envWalk, reset to empty, do
       parent-anchored + walk-from-∅ attempts, restore session state
       on exit. Slow-path per-Q walk builds its own local envWalk;
       it does not update the session envCur (per-Q state is not the
       session-cumulative point).

       See `doc/design/tracing-eval-cache.md` §Replay strategies
       (slow path) for the reasoning. */
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

    /* Walk with two anchor candidates in order:
       1. Parent TracingReplayObject's terminalCur — the structural-anchor
          lookup position. Child Q's recording was made starting from
          parent's reached factSet, so anchoring the child history there
          matches the recording's frame.
       2. From ∅ — needed when no parent anchor exists (top-level Q
          like evalFile/evalExpr, no TracingReplayObject) and as a backstop
          when the parent-anchored attempt finds no matching Asks chain. */
    Hash parentAnchor = TracingDecisionGraph::emptySetHash();
    if (auto * parentTR = dynamic_cast<TracingReplayObject *>(ctx.currentProxy.get())) {
        parentAnchor = parentTR->getTriePos().factSetHash;
    }
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
                /* Per-edge K > 0 navigation: fold in only observations
                   whose `fromHash` equals subject's current state (=
                   observation was made against subject at cur).
                   ObservationSet-scoped semantics (all obs in one edge
                   check against edge-entry cur) preserved via
                   `edgeAcc`. Under matching-until-divergence this is
                   the same filter cold's writer used when stamping a
                   fold step — no DB roundtrip needed. */
                if (!found) {
                    Hash cur = stateHashAt(*subj, argAncestry, extendedWalkForMatch, 0);
                    for (const auto & edge : extendedWalkForMatch) {
                        if (found) break;
                        Hash edgeAcc(HashAlgorithm::SHA256);
                        for (const auto & obs : edge.observations) {
                            if (obs.fromHash == cur)
                                edgeAcc = TracingDecisionGraph::xorHashes(edgeAcc, obs.elementHash);
                        }
                        cur = TracingDecisionGraph::xorHashes(cur, edgeAcc);
                        if (cur.to_string(HashFormat::Base16, false) == idStr) found = true;
                    }
                }
                /* Observation-permutation navigation, folded to its
                   fixed point. `stateHashConverged` is order- and
                   grouping-independent by construction: walker's
                   convergence value depends only on the SET of
                   observations, not on edge boundaries.

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
        /* "Not in pool" means the id has no producer Request. Such
           ids are OUTER-direction by elimination — outer-arg state
           hashes minted by makeCachedFnPrimOp.impl, e.g. a nested
           OuterObject for the int the callback body passes to
           inner_lambda in cb-higher-order's `g 10`.

           Live-proxy fallback: the `<replay-local-lambda>` primop
           registers the args[0] it receives under the cb-arg arg's
           initial state hash when fired (= registerAmbientResolverProxy
           in replay-callback-arg.cc). If we find a matching
           registration here, the OUTER walker resolves to that live
           proxy and dispatches the env fact live against outer's
           actual value — capability-mediated, not cached.

           Without a registration, fall through to nullptr. The via-
           Asks design forbids serving from the Responses pool for
           OUTER values — silently masks outer-body change (cb-
           higher-order step 3 returning stale 6 when outer changed
           from `g 5` to `g 10`). */
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

/* Resolve an "apply" producer's result by resolving fn + arg and
   invoking fn.queryApply(arg) live. Arg resolution is uniform (via
   resolveStateHash) — the historical localArg-sidecar special case
   is gone: callback-arg observations ride in the CallbackApply
   query's obsSet, not through producer-chain apply resolution. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveApplyId(
    const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx)
{
    auto fnObj = resolveStateHash(params["fn"].get<std::string>(), ctx);
    if (!fnObj) {
        tracingCacheLog("replay: apply %s: cannot resolve fn %s", idStr, params["fn"]);
        return nullptr;
    }
    auto argIdStr = params["arg"].get<std::string>();
    auto argObj = resolveStateHash(argIdStr, ctx);
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


static trace::PathExpr parsePathFromParams(const nlohmann::json & params)
{
    trace::PathExpr path;
    if (params.contains("path"))
        from_json(params.at("path"), path);
    return path;
}

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
       a `callbackApply` slot. Dispatch end-to-end here rather than
       falling through to the state-hash-equality routing pipeline —
       the slot carries every piece of navigation info the walker
       needs. Materialise a fresh ReplayCallbackArg backed by the
       slot's obsSet, navigate to fn structurally from the outer's
       arg (cell[0].liveObject) via the query's `fnPath`, invoke
       fn->queryApply(replayArg), then run the query's op
       (getWHNF/getAttr/…) on the result. No ctx.memo, no
       resolveStateHash dance. */
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
        auto replayArg = std::make_shared<ReplayCallbackArg>(
            std::move(argSubject), argAncestry,
            walkFacts, chainCursor,
            decisionGraph, inner->getEvalState().rootFSRoot,
            &inner->getEvalState());
        replayArg->withObsSetResponses(obsSetMap);

        /* Resolve fn as a live Object from the slot's state-hash
           reference. `ref.fn` is fn's evolved state hash stamped by
           the writer at emission time (`tracing-writer.cc:80`).
           resolveStateHash routes through the cell chain via
           state-hash equality — finds the specific fn whose evolved
           state matches the recording's, not merely whichever fn is
           currently in cell[0]. If it can't find a match, miss
           cleanly rather than falling back to a structural shortcut
           that discriminates by the current invocation's fn instead
           of the recording's. Correctness-first per task #103's
           MVP framing (repeated live outer validation calls, no
           per-cell memoisation). */
        auto fnObj = resolveStateHash(ref.fn, ctx);
        if (!fnObj) {
            tracingCacheLog(
                "callbackApply slot: resolveStateHash(fn=%s) miss — clean fallthrough",
                ref.fn.substr(0, 12));
            return std::nullopt;
        }

        auto pathParsed = parsePathFromParams(params);
        std::shared_ptr<Object> resultObj;
        try {
            /* First step must be Apply; trailing steps handled after. */
            if (pathParsed.steps.empty()
                || pathParsed.steps[0].kind != trace::PathStep::Kind::Apply)
                return std::nullopt;
            resultObj = fnObj->queryApply(replayArg);
        } catch (const std::exception & e) {
            tracingCacheLog("callbackApply slot: dispatch failed at fn apply: %s", e.what());
            return std::nullopt;
        }
        if (!resultObj)
            return std::nullopt;

        /* Apply the query's leaf op to the result Object, using any
           trailing path steps after the Apply for descendant probes
           (e.g. `.getAttr("rr")` before the leaf op). */
        std::shared_ptr<Object> obj = resultObj;
        try {
            for (size_t i = 1; i < pathParsed.steps.size(); ++i) {
                const auto & step = pathParsed.steps[i];
                if (!obj)
                    return std::nullopt;
                if (step.kind == trace::PathStep::Kind::GetAttr)
                    obj = obj->maybeGetAttr(step.name);
                else if (step.kind == trace::PathStep::Kind::GetListElem)
                    obj = obj->getListElem(step.index);
                else
                    return std::nullopt;
            }
            if (!obj)
                return std::nullopt;

            nlohmann::json resultJson;
            if (tag == "getWHNF") {
                resultJson = computeWHNFFromObject(*obj);
            } else if (tag == "getAttr") {
                auto name = params["name"].get<std::string>();
                auto child = obj->maybeGetAttr(name);
                if (!child)
                    resultJson = trace::ResultMaybeType{std::nullopt};
                else
                    resultJson = trace::ResultMaybeType{std::optional<std::string>{objectTypeToString(child->getType())}};
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
            tracingCacheLog(
                "callbackApply slot: end-to-end dispatch HIT tag=%s obsSet=%s argDepth=%d",
                tag.c_str(), ref.argObsSet.substr(0, 12), ref.argDepth);
            return jsonToCborString(resultJson);
        } catch (const std::exception & e) {
            tracingCacheLog("callbackApply slot: dispatch failed at leaf op %s: %s", tag.c_str(), e.what());
            return std::nullopt;
        }
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

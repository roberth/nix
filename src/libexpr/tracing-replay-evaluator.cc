#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/interpreter-object.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/arg-scope.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/replay-local-object.hh"
#include "nix/expr/tracing-cache-stats.hh"
#include "nix/expr/tracing-local-object.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/tracing-cache-log.hh"
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
    , lastQFactsHash(TracingDecisionGraph::emptySetHash())
{
}

std::optional<TracingReplayEvaluator::V13WalkResult>
TracingReplayEvaluator::v13Walk(const Hash & queryHash, std::shared_ptr<Object> currentProxy)
{
    /* The entire walk is VALIDATION of recorded state — any apply
       queries triggered through `fnObj->queryApply(...)` during
       dispatch (resolveApplyId, navigatePath's Apply step,
       dispatchApplyLive) re-route through `AmbientObject::queryApply
       → applyFn → AmbientApply::run` and would each fire a fresh
       `markApplyBoundary` on the writer if not suppressed. Each fresh
       boundary inflates `d1CidasksWalk` with a redundant ε edge
       beyond the genuine cb-apply events the recorder already
       captured. Suppress for the walk's duration so writer's
       d1CidasksWalk stays in 1:1 alignment with walker's
       cidasksWalk. */
    TracingWriter::SuppressApplyBoundary suppressBoundary(writer);

    /* Register callback so suppressed markApplyBoundary calls (=
       inner cb-apply boundaries fired inside dispatchApplyLive's
       cb-fn execution) synthesise a phantom ε obs in walker's
       cidasksWalk. Cold's writer would have inserted these as ε
       edges into d1CidasksWalk; without this walker's walk-index
       falls short of cold's edgeIndex for later flushes referencing
       seed(1) at post-inner-apply positions. */
    auto prevHook = writer.suppressedBoundaryHook;
    writer.suppressedBoundaryHook = [this](const Hash & applyReqHash) {
        /* Dedup by applyReqHash — cold's inner emits ONE ε obs per
           unique cb-apply boundary. Walker's dispatchApplyLive fires
           cb-fn LIVE multiple times (once per outer probe that
           references the apply's chain), which re-triggers the same
           inner markApplyBoundary each firing. Only push ε obs the
           first time per applyReqHash. */
        if (!suppressedBoundaryEpsilonsSeen.insert(applyReqHash).second)
            return;
        /* ε obs = {fromHash=0, elementHash=factHash} where factHash =
           XOR(applyReqHash, AmbientResult). Use applyReqHash as
           placeholder AmbientResult (matches cold's
           "empty-d=2-group" convention). */
        auto factHash = TracingDecisionGraph::xorFactIntoHash(
            Hash(HashAlgorithm::SHA256), applyReqHash, applyReqHash);
        cidasks::Edge edge;
        edge.observations.push_back({Hash(HashAlgorithm::SHA256), factHash});
        cidasksWalk.push_back(std::move(edge));
        tracingCacheLog(
            "walker: suppressed-boundary ε obs pushed, cidasksWalk=%zu",
            cidasksWalk.size());
    };
    struct HookGuard {
        TracingWriter & w;
        std::function<void(const Hash &)> prev;
        ~HookGuard() { w.suppressedBoundaryHook = prev; }
    } hookGuard{writer, prevHook};

    /* Per-walk resolution context. The cumulative cidasks walk
       (= `this->cidasksWalk`) lives on the evaluator so it
       persists across v13Walk calls — required for cell-chain
       scopeStateId computation to land at the writer's `d1EdgeIndex` (=
       cumulative across logResults). */
    ResolutionContext ctx{
        std::move(currentProxy),
        {},
    };
    ctx.currentQueryHash = queryHash;
    /* QCidasksWalks snapshot load: DELETED. Under lockstep
       walker/writer growth, walker's own cidasksWalk carries what's
       needed at each Q's flush moment; the per-Q snapshot table is
       redundant. Preserves the ctx.snapshotWalk vector empty so
       downstream extendedWalkForMatch reduces to walker's cidasksWalk. */

    /* Per-edge buffer: dispatch() appends ambient facts here; the
       walk-loop promotes the buffer to a cumulative cidasksWalk
       edge on commit (via commitEdge) or discards it on reject.
       Without the buffer, rejected-edge facts would pollute
       cidasksWalk and throw off the cell-chain scopeStateId computations. */
    std::vector<cidasks::Observation> pendingEdgeObservations;

    auto commitEdge = [&]() {
        /* 1:1 alignment with writer's d1CidasksWalk: writer inserts each
           cb-apply boundary's ε obs as a SEPARATE d1 edge at its
           `insertionIndex`, not bundled with the real-obs edge that
           triggered it. Walker's dispatch() pushes ε obs (fromHash=0)
           into `pendingEdgeObservations` alongside real obs of the
           same Asks edge — we need to split them at commit time so
           each ε lives in its own edge, matching writer's layout.

           Split: partition pending obs into ε (fromHash=0) and real
           (non-zero fromHash). Commit real obs as the primary edge;
           each ε obs becomes its own subsequent edge. */
        std::vector<cidasks::Observation> realObs;
        std::vector<cidasks::Observation> epsilonObs;
        realObs.reserve(pendingEdgeObservations.size());
        for (auto & obs : pendingEdgeObservations) {
            if (obs.fromHash == Hash(HashAlgorithm::SHA256))
                epsilonObs.push_back(std::move(obs));
            else
                realObs.push_back(std::move(obs));
        }
        pendingEdgeObservations.clear();

        auto tryPush = [&](std::vector<cidasks::Observation> obs) {
            if (obs.empty()) {
                tracingCacheLog("dispatch: edge empty, skip commit");
                return;
            }
            Hash fingerprint(HashAlgorithm::SHA256);
            for (const auto & f : obs)
                fingerprint = TracingDecisionGraph::xorFactIntoHash(
                    fingerprint, f.fromHash, f.elementHash);
            if (committedEdgeFingerprints.insert(fingerprint).second) {
                cidasks::Edge edge;
                edge.observations = std::move(obs);
                cidasksWalk.push_back(std::move(edge));
                tracingCacheLog("dispatch: committed edge, cidasksWalk=%zu (obs=%zu)",
                                cidasksWalk.size(), cidasksWalk.back().observations.size());
            } else {
                tracingCacheLog("dispatch: edge already in cidasksWalk (shared prefix), skip");
            }
        };

        tryPush(std::move(realObs));
        for (auto & obs : epsilonObs)
            tryPush({std::move(obs)});
    };

    auto discardEdge = [&]() {
        pendingEdgeObservations.clear();
    };

    /* Dispatcher: turns a Request hash into the current Response
       hash. Memoised in dispatchCache for stable requests (file
       reads, env vars) where same request always gives same
       response. Ambient queries are NOT memoised because the same
       request hash can dispatch to different responses depending on
       which proxy (cb invocation) the walk is grounded in — sibling
       cb apply invocations of the same fn share a request hash but
       must see their own arg's live value, not a memoised sibling's. */
    auto dispatch = [&](const Hash & requestHash, const TracingDecisionGraph::EdgeContext & edgeCtx) -> Hash {
        auto requestPayload = decisionGraph.getRequestPayload(requestHash);
        if (!requestPayload)
            return Hash(HashAlgorithm::SHA256);
        bool isAmbient = false;
        std::optional<Hash> ambientFromHash;
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
                            ambientFromHash = Hash::parseNonSRIUnprefixed(
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
            if (auto it = dispatchCache.find(requestHash); it != dispatchCache.end())
                return it->second;
        }
        /* Apply-boundary: AmbientResult split by chain presence.
            - No chain at applyReqHash: AmbientResult = applyReqHash
              (= chain root; matches writer's empty-d=2-group path).
            - Chain present: invoke fn live via dispatchApplyLive,
              which forces the result so outer's f drives probes
              against a fresh standin. On divergence, fail dispatch. */
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
            /* DISALLOW-mode LRM fallback (on dispatch failure only,
               not mismatch): under `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`
               the cache must serve every recorded value, so a
               `resolveRoots` failure here is a walker routing bug
               (currentProxy can't reach the sibling's contraArg to
               dispatch a cross-sibling `from`), not a legitimate env
               change. Fall back to LRM which under within-session
               soundness has cold's actual response for this reqhash.
               Only substitute on FAILURE — not on live/stored mismatch
               — so observation-driven divergence (cb-sibling-
               discrimination-via-observation) with distinct reqhashes
               per sibling stays uncorrupted. Gated on DISALLOW so
               normal-mode capability-mediated dispatch stays live-only. */
            static const bool disallowInner =
                getEnv("_NIX_DISALLOW_CACHE_INTERPRET_INNER").value_or("") == "1";
            if (disallowInner && isAmbient) {
                if (auto storedResp = decisionGraph.getLocalResponsePayload(requestHash, Hash(HashAlgorithm::SHA256))) {
                    auto storedH = TracingDecisionGraph::computeResponseHash(*storedResp);
                    tracingCacheLog(
                        "dispatch DISALLOW-mode LRM fallback req=%s -> resp=%s (%s)",
                        requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        storedH.to_string(HashFormat::Base16, false).substr(0, 12),
                        queryDescription);
                    writer.noteEnvObservation(requestHash, storedH);
                    pendingEdgeObservations.push_back({
                        ambientFromHash.value_or(Hash(HashAlgorithm::SHA256)),
                        TracingDecisionGraph::xorFactIntoHash(
                            Hash(HashAlgorithm::SHA256), requestHash, storedH),
                    });
                    return storedH;
                }
            }
            tracingCacheLog(
                "dispatch FAIL req=%s payload=%s (no current response)",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription);
            return Hash(HashAlgorithm::SHA256);
        }
        auto h = TracingDecisionGraph::computeResponseHash(*currentResp);
        /* edgeCtx is threaded through walk() for offline-inspection
           consumers; d=1 dispatch MUST NOT read stored responses to
           substitute for a live response that differs from cold's —
           doing so masks legitimate outer-body change detection (per
           the design's capability-mediated invariant) AND, even
           under `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`, breaks
           observation-driven sibling discrimination
           (cb-sibling-discrimination-via-observation): a wrong-sibling
           live response substituted with the FIRST-WRITER LRM entry
           routes both siblings to the same recorded terminal, giving
           `200` instead of `100 + 1000 = 1100`. Only substitute on
           DISPATCH FAILURE (see the block above), not on mismatch. */
        (void) edgeCtx;
        if (!isAmbient)
            dispatchCache.emplace(requestHash, h);
        /* Dispatched facts are real environment observations; feed
           them into the writer's v13FactSet so any subsequent
           logResult records at the same factSetHash regardless of
           which facts came from interpretation vs cache-hit
           dispatch. */
        writer.noteEnvObservation(requestHash, h);
        /* Buffer ambient facts for this in-flight Asks edge; the
           walk-loop commits them via onEdgeCommitted on success. */
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
        if (isAmbient && ambientFromHash) {
            pendingEdgeObservations.push_back({
                *ambientFromHash,
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, h),
            });
            tracingCacheLog(
                "dispatch ambient: req=%s payload=%s from=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                ambientFromHash->to_string(HashFormat::Base16, false).substr(0, 12),
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

    /* Fast path: leverage the trie's structural sharing.

       For sequential mapAttrs-style replays the next Q's recorded
       factSet is almost always a strict superset of the last Q's.
       Instead of walking from (Q, ∅) and re-dispatching the whole
       chain, ask the RequestSet trie: which Requests does this Q's
       RS contain that we haven't already dispatched, and vice
       versa? That's a trie-diff in O(|delta|·branching) — the
       hash-equal shared subtrees short-circuit instantly.

       Then XOR-extend lastQFactsHash by the fact-element hashes
       for the delta-add (using live dispatch) and undo the
       delta-rm (using cached responses), giving the cur Q's
       recorded chain would have landed at. If Terminals has an
       entry there for Q, hit; otherwise fall back to walk(). */
    auto outgoing = decisionGraph.getAsks(queryHash, TracingDecisionGraph::emptySetHash());
    if (outgoing.size() == 1) {
        const Hash & edgeRsHash = outgoing[0];
        std::vector<Hash> onlyInDispatched;
        std::vector<Hash> onlyInEdge;
        dispatchedTrie.diff(decisionGraph, edgeRsHash, onlyInDispatched, onlyInEdge);

        Hash candidateCur = lastQFactsHash;
        bool dispatchFailed = false;
        TracingDecisionGraph::EdgeContext fastPathCtx{queryHash, lastQFactsHash, edgeRsHash};
        for (const auto & req : onlyInEdge) {
            auto resp = dispatch(req, fastPathCtx);
            if (resp == Hash(HashAlgorithm::SHA256)) {
                dispatchFailed = true;
                break;
            }
            candidateCur = TracingDecisionGraph::xorFactIntoHash(candidateCur, req, resp);
        }
        if (!dispatchFailed) {
            for (const auto & req : onlyInDispatched) {
                auto it = dispatchCache.find(req);
                if (it == dispatchCache.end()) { dispatchFailed = true; break; }
                /* XOR is self-inverse: same op undoes the previous fold-in. */
                candidateCur = TracingDecisionGraph::xorFactIntoHash(candidateCur, req, it->second);
            }
        }
        if (!dispatchFailed) {
            if (auto term = decisionGraph.getTerminal(queryHash, candidateCur)) {
                auto payload = decisionGraph.getResultPayload(*term);
                if (payload) {
                    for (const auto & req : onlyInEdge) {
                        dispatchedTrie.insert(req);
                        dispatchedRequestSet.insert(req);
                    }
                    lastQFactsHash = candidateCur;
                    tracingCacheStats().hits++;
                    commitEdge();
                    return V13WalkResult{std::move(*payload), *term, candidateCur};
                }
            }
        }
        /* Fast-path didn't reach a terminal: drop the buffered facts;
           the full walk below starts fresh. */
        discardEdge();
    }

    /* Fall back to walk(). Two anchor candidates in order:
       1. Parent TracingReplayObject's terminalCur — the structural-anchor lookup
          position. Child Q's recording was made starting from
          parent's reached factSet (= where the parent walk landed),
          so anchoring the child walk there matches the recording's
          frame. This isolates each child Q from sibling Q's
          accumulated state: the prior session-leaky `lastQFactsHash`
          would carry a sibling's terminal into this Q's startCur,
          dragging in observations the recording doesn't expect.
       2. From ∅ — original behavior. Needed when no parent anchor
          exists (top-level Q like evalFile/evalExpr, no TracingReplayObject) and as
          a backstop when the parent-anchored attempt finds no
          matching Asks chain. */
    /* Use `ctx.currentProxy` (not `currentProxy`) — the local was
       moved into ctx above, so it's now empty. cb-sibling-b's chain
       traversal was blocked by this bug: child Q walks were starting
       from ∅ instead of the parent's terminalCur, forcing walker to
       re-walk sibling B's chain from the top under the wrong
       currentProxy context. */
    Hash parentAnchor = TracingDecisionGraph::emptySetHash();
    if (auto * parentTR = dynamic_cast<TracingReplayObject *>(ctx.currentProxy.get())) {
        parentAnchor = parentTR->getTriePos().factSetHash;
    }
    /* Track rejected-edge obs across all attempts. Committed on walk
       MISS so subsequent v13Walk calls' resolveCdiId sees the obs
       walker produced during the failed traversal — those obs carry
       real (req, resp) pairs from cold's recorded responses, and
       future resolves at deeper edgeIndex may need them. Only
       preserve on final miss; on hit, the winning edges are already
       committed and the rejected ones represent wrong branches whose
       obs would contaminate the correct chain. */
    std::vector<cidasks::Observation> rejectedObs;
    auto commitRejected = [&](const std::vector<Hash> &) {
        for (auto & obs : pendingEdgeObservations)
            rejectedObs.push_back(std::move(obs));
        pendingEdgeObservations.clear();
    };
    /* Pass dispatchedRequestSet as startCurRequests so walker at
       parentAnchor knows which requests have already been dispatched
       in prior walks reaching this cur. Without this, walker's
       `useful = rs \ curRequests` computes wrongly, potentially
       re-dispatching already-observed requests and diverging the cur.
       Empty when starting from ∅ (nothing dispatched yet). */
    std::unordered_set<Hash> parentAnchorCurRequests;
    if (parentAnchor != TracingDecisionGraph::emptySetHash())
        parentAnchorCurRequests = dispatchedRequestSet;
    auto walkHit = decisionGraph.walk(queryHash, dispatch,
        [&](bool committed, const std::vector<Hash> & useful) {
            if (committed) commitEdge();
            else commitRejected(useful);
        },
        parentAnchor,
        parentAnchorCurRequests);
    if (!walkHit && parentAnchor != TracingDecisionGraph::emptySetHash()) {
        walkHit = decisionGraph.walk(queryHash, dispatch, [&](bool committed, const std::vector<Hash> & useful) {
            if (committed)
                commitEdge();
            else
                commitRejected(useful);
        });
    }
    /* reverse-outgoing walkImpl fallback: DELETED. Was a targeted
       cb-repeated helper via alternative rs ordering; the un-fold
       Terminal deletion (see walkImpl) also removed the mechanism it
       reached for. */
    /* Snapshot-padding retry on miss. When cold's snapshot for this Q
       exceeds walker's current cidasksWalk (cold/warm flush-pattern
       asymmetry per per-arg-completion.md), scopeStateIdAt-driven
       dispatch response resolution can differ from cold's at flush time
       — the same subject CDI resolves to a different LiveObject under
       walker's undersized cidasksWalk. Retry with cidasksWalk padded
       to snapshot size, ONLY on primary miss so tests whose primary
       walk succeeds (cb-higher-order, cb-higher-order-nested, cb-385)
       stay on the unpadded path. Padding is spliced out post-walk so
       walker's persistent cidasksWalk retains only real committed edges. */
    /* Snapshot-padded retry DELETED (iteration 22 re-attempt). */
    if (!walkHit) {
        /* Walker missed. Rejected-edge obs are NOT committed to
           cidasksWalk: they represent wrong paths whose responses
           cold never recorded, so folding them into seed CDIs shifts
           subject_at_k to values cold never stamped. Per Asks-paradigm
           navigation invariant, CDIs are pure functions of the
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
    /* Cold/warm cidasksWalk-size alignment. Cold's writer pushes to
       `d1CidasksWalk` at two per-logResult points:
       (1) inside `flushPendingAmbient(finalize=true)` in
           tracing-writer.cc, at the trailing
           `d1CidasksWalk.push_back(pendingD1Edge)` when
           `!pendingNewRequests.empty()`,
       (2) at the trailing `splitFlush` in the writer's logResult
           path — the second `d1CidasksWalk.push_back(pendingD1Edge)`
           when again `!pendingNewRequests.empty()`.
       So cold's per-logResult push count is 2 IFF pendingNewRequests
       was non-empty at both points. Walker's per-Asks-edge
       `commitEdge` only mirrors point (2). Push a second synthetic
       edge here to mirror point (1) so walker.cidasksWalk grows in
       lockstep. Empty edges are load-bearing for index alignment;
       subsequent `scopeStateIdAt(...,cidasksWalk,K)` computations
       need matching K to resolve CDIs by structural k rather than
       fold-coincidence linear search across sibling cells. */
    cidasks::Edge synthetic1;
    cidasksWalk.push_back(std::move(synthetic1));
    tracingCacheLog("v13Walk success synthetic push: cidasksWalk=%zu",
                    cidasksWalk.size());
    return V13WalkResult{std::move(*payload), walkHit->resultHash, walkHit->terminalCur};
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
   First check the per-walk memo (ctx.memo) for already-resolved ids.
   Then walk the proxy graph (ctx.currentProxy.parent → …) looking
   for an argScope cell whose id matches — this is the seed-lookup
   case, grounded in the proxy whose method triggered this walk
   rather than in any evaluator-global state.
   Then fall through to producer-Request resolution: find idStr in
   the Requests pool, resolve the parent recursively, dispatch the
   producer's query on the parent. QueryApply payloads invoke the
   live apply against a (frozen) ReplayLocalObject arg. localArg
   sidecars chase to the apply. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveCdiId(const std::string & idStr, ResolutionContext & ctx)
{
    /* Per-walk memo. */
    if (auto it = ctx.memo.find(idStr); it != ctx.memo.end()) {
        tracingCacheLog("resolve %s -> memo hit", idStr.substr(0, 12));
        return it->second;
    }

    /* Producer-first check: apply-result CDIs need to route through
       resolveApplyId (proper per-(fn, arg) live reconstruction)
       instead of cell iteration (which conflates distinct apply-result
       CDIs sharing cell[0]'s subject Merkle). ApplyResultProducers is
       cold's `applyResultCid → (fn, arg)` index; any evolved state of
       an apply hits the same producer pair. */
    {
        Hash idHash0{HashAlgorithm::SHA256};
        bool haveIdHash = false;
        try {
            idHash0 = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
            haveIdHash = true;
        } catch (...) {}
        if (haveIdHash) {
            if (auto producer = decisionGraph.getApplyResultProducer(idHash0)) {
                auto & [fnH, argH] = *producer;
                tracingCacheLog("resolve %s: ApplyResultProducers route fn=%s arg=%s",
                                idStr.substr(0, 12),
                                fnH.to_string(HashFormat::Base16, false).substr(0, 12),
                                argH.to_string(HashFormat::Base16, false).substr(0, 12));
                nlohmann::json params;
                params["fn"] = fnH.to_string(HashFormat::Base16, false);
                params["arg"] = argH.to_string(HashFormat::Base16, false);
                auto result = resolveApplyId(idStr, params, ctx);
                if (result) {
                    ctx.memo[idStr] = result;
                    return result;
                }
            }
        }
    }

    /* Walk the proxy's argScope chain looking for a cell whose
       liveObject's scopeStateId matches idStr at some k under
       walker's own cidasksWalk. The former snapshot-extension pass
       (dedup(ctx.snapshotWalk) onto extendedWalkForMatch) was
       DELETED when QCidasksWalks was removed; walker's cidasksWalk
       is sufficient under lockstep growth. */
    std::vector<cidasks::Edge> extendedWalkForMatch = cidasksWalk;
    auto cell = ctx.currentProxy ? ctx.currentProxy->getProxyArgScope() : nullptr;
    int cellDepth = 0;
    for (; cell; cell = cell->parent, ++cellDepth) {
        if (auto live = cell->liveObject) {
            if (auto * subj = live->getSubject()) {
                /* Use the live proxy's own inherited scope so the
                   walker's scope state id matches what the recorder
                   computed at this proxy at flush. */
                auto scope = live->getInheritedScope();
                bool found = false;
                /* K=0 fast path — Asks-style initial-CDI lookup:
                   subject's initial content-defined identifier
                   (before any observation folds in) is a pure
                   function of (subject, scope). Walker computes it
                   as a key and checks equality against the target
                   — no iteration over K, no scanning for "which
                   walker-state produces target". F19 (2026-07-04)
                   empirical: 55% of cell-chain matches in the
                   cb-* + builtins-cache bounds land at K=0.
                   Structurally an Asks-style navigation: walker's
                   own hashed state (initial CDI) IS the lookup
                   key. */
                {
                    auto initialCdi = cidasks::scopeStateIdAt(*subj, scope, extendedWalkForMatch, 0);
                    if (initialCdi.to_string(HashFormat::Base16, false) == idStr) {
                        found = true;
                    }
                }
                /* Path 3 walker-side trie navigation for K > 0.
                   Walker's current cur is its own hashed state (key);
                   walker looks up (subject, cur, obs.from, obs.elem)
                   in cold-recorded SubjectEvolutionEdges; if edge
                   exists, folds obs.elem into edge accumulator.
                   Edge-scoped semantics (all obs in one edge check
                   against edge-entry cur) preserved.
                   Empirical (iter 61 probe): 137/137 k-iter matches
                   also reached by trie navigation. */
                if (!found) {
                    Hash subjectSelfHash = cidasks::scopeStateIdAt(
                        *subj, Hash(HashAlgorithm::SHA256), {}, 0);
                    Hash cur = cidasks::scopeStateIdAt(*subj, scope, extendedWalkForMatch, 0);
                    for (const auto & edge : extendedWalkForMatch) {
                        if (found) break;
                        Hash edgeAcc(HashAlgorithm::SHA256);
                        for (const auto & obs : edge.observations) {
                            auto next = decisionGraph.getSubjectEvolutionEdge(
                                subjectSelfHash, cur, obs.fromHash, obs.elementHash);
                            if (next)
                                edgeAcc = TracingDecisionGraph::xorHashes(edgeAcc, obs.elementHash);
                        }
                        cur = TracingDecisionGraph::xorHashes(cur, edgeAcc);
                        if (cur.to_string(HashFormat::Base16, false) == idStr) found = true;
                    }
                }
                /* Observation-permutation Asks navigation — the
                   second half of the search→asks replacement.
                   Structurally: walker computes its current state
                   `currentId` (its own hashed key); uses that key
                   to partition the observation pool by state-match
                   (obs.fromHash == currentId, an equality against
                   walker's own hash); folds matches into the state
                   accumulator; advances. Each round is one
                   Asks-style navigation step — IDs flow INTO the
                   partition filter as keys (walker's currentId
                   AND obs.fromHash), never out.

                   Two-part search→asks structure at this callsite:
                   - Walk-order Path 3 (above): follows walker's
                     cidasksWalk edge-by-edge, trie-navigating
                     cold's recorded (subject, cur, obs) → nextCur
                     stamps. Handles matches reachable in cold's
                     recording order.
                   - Observation-permutation navigation (this
                     block): walker partitions its observation pool
                     by walker-computed state-match, iterating
                     rounds until convergence or 32-round safety
                     limit. Handles matches reachable in permuted
                     orders (cb-385's 5-round evolution).

                   Iter 63 attempted to convert this filter to
                   Path-3 trie lookup — regressed cb-sibling-b
                   because trie stamps are per-scopeStateIdAt-call,
                   not per-cumulative-cidasksWalk-state; walker's
                   fold can reach states cold's per-call walks
                   didn't. Local hash-equality filter is
                   walker-side computation, doesn't require cold
                   stamps to have covered the specific state. */
                if (!found && !extendedWalkForMatch.empty()) {
                    std::vector<cidasks::Observation> flat;
                    std::set<std::pair<Hash, Hash>> seen;
                    for (const auto & edge : extendedWalkForMatch)
                        for (const auto & obs : edge.observations) {
                            auto key = std::make_pair(obs.fromHash, obs.elementHash);
                            if (seen.insert(key).second) flat.push_back(obs);
                        }
                    std::vector<cidasks::Edge> hypWalk;
                    for (int iter = 0; iter < 32 && !flat.empty() && !found; ++iter) {
                        auto currentId = cidasks::scopeStateIdAt(*subj, scope, hypWalk, hypWalk.size());
                        cidasks::Edge partition;
                        std::vector<cidasks::Observation> stillRemaining;
                        for (auto & obs : flat) {
                            if (obs.fromHash == currentId) partition.observations.push_back(obs);
                            else stillRemaining.push_back(obs);
                        }
                        if (partition.observations.empty()) break;
                        hypWalk.push_back(std::move(partition));
                        auto id = cidasks::scopeStateIdAt(*subj, scope, hypWalk, hypWalk.size());
                        if (id.to_string(HashFormat::Base16, false) == idStr) found = true;
                        flat = std::move(stillRemaining);
                    }
                }
                if (found) {
                    tracingCacheLog(
                        "resolve %s: cell[%d] subject=%s MATCH",
                        idStr.substr(0, 12), cellDepth, cidasks::describe(*subj));
                    ctx.memo[idStr] = live;
                    return live;
                }
                tracingCacheLog(
                    "resolve %s: cell[%d] subject=%s miss across %zu edges (+collected)",
                    idStr.substr(0, 12), cellDepth,
                    cidasks::describe(*subj), cidasksWalk.size() + 1);
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
           OUTER-direction by elimination: an inner local's argId is
           always sidecar-registered by AmbientResolver::apply (=
           inserting `{kind: "localArg", applyResultId: ...}` at the
           argId), and any derived value has a producer Request. Only
           outer-seed argStateIds minted by makeCachedFnPrimOp.impl — e.g.
           a nested AmbientObject for the int the outer body passes
           to inner_lambda in cb-higher-order's `g 10` — reach here.

           Live-proxy fallback: the `<replay-local-lambda>` primop
           registers the args[0] it receives under the cb-arg seed's
           initial argStateId when fired (= registerAmbientResolverProxy in
           replay-local-object.cc). If we find a matching registration
           here, the OUTER walker resolves to that live proxy and
           dispatches the d=1 fact live against outer's actual value
           — capability-mediated, not cached. This closes the seed-
           resolution gap that otherwise kills cb-higher-order's
           DISALLOW_PARSE warm-replay steps.

           Without a registration, fall through to nullptr. The via-
           Asks design forbids serving from the Responses pool for
           OUTER values ("ambient responses are capability-mediated,
           not cached" — primop doc §Replay semantics); the previous
           fallback materialised an RLO and let its methods read out
           of LocalResponseMap, which was correct for INNER locals but
           wrong here: it served the recorded outer response regardless
           of whether the live outer would produce it, silently masking
           outer-body change (cb-higher-order step 3 returning stale 6
           when outer changed from `g 5` to `g 10`).

           INNER locals are unaffected by this change: their sidecar
           presence routes them via `chaseLocalArgSidecar`, and
           `resolveApplyId` with explicit `isLocalArgId`
           discrimination materialises their RLO. Serving inner
           locals from the reconstructed value tree backed by
           LocalResponseMap is per design (= depth-2 Replay's
           "walker reconstructs the LocalObject as a live Nix Value
           tree from the CAS pool"). The forbidden thing is treating
           an OUTER-direction id as if it were a local. */
        if (auto resolver = inner->getAmbientResolver()) {
            if (auto live = tryResolveAmbientResolverProxy(*resolver, idHash, cidasksWalk, &decisionGraph)) {
                tracingCacheLog(
                    "resolve %s: not in pool — found live-proxy registration",
                    idStr.substr(0, 12));
                ctx.memo[idStr] = live;
                return live;
            }
        }
        tracingCacheLog(
            "resolve %s: not in pool — no provenance (outer-seed by elimination); returning null",
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
        return chaseLocalArgSidecar(idStr, reqJson, ctx);
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
   inner-side TracingLocalObject's content-hash whose facts were emitted
   with from=hex(id) but whose id itself isn't a producer Request.
   Materialise a ReplayLocalObject keyed by it; its methods read
   recorded responses out of LocalResponseMap by qH(query{from=hex(id)}),
   matching what TracingLocalObject wrote during recording. */
/* Local-direction: sidecar inserted by AmbientResolver::apply to mark
   that this id is the local arg of a covariant callback. Chase to the
   apply; the apply branch registers the live argObj under localId in
   ctx.memo, so subsequent dispatches of local-incoming Facts find it
   without re-chasing. */
std::shared_ptr<Object> TracingReplayEvaluator::chaseLocalArgSidecar(
    const std::string & idStr, const nlohmann::json & reqJson, ResolutionContext & ctx)
{
    auto applyResultIdHex = reqJson["applyResultId"].get<std::string>();
    resolveCdiId(applyResultIdHex, ctx);
    if (auto it = ctx.memo.find(idStr); it != ctx.memo.end())
        return it->second;
    /* Direct construction fallback: the recursive resolve may not
       have gone through resolveApplyId (which memoizes at argIdStr).
       If the sidecar carries depth+scope, construct the standin
       ourselves — this is what resolveApplyId does for the isLocalArgId
       case at its arg parameter, but here we're the target of the
       resolve and the resolveApplyId path may not have been reached. */
    if (reqJson.contains("depth") && reqJson.contains("scope")) {
        try {
            auto sidecarDepth = reqJson["depth"].get<int>();
            auto sidecarScope = Hash::parseNonSRIUnprefixed(
                reqJson["scope"].get<std::string>(), HashAlgorithm::SHA256);
            cidasks::Subject rootSubject{cidasks::PositionalSeed{sidecarDepth}};
            /* Fall-through path outerContext: compute seqCtx from
               idStr (which is the apply's chain root = applyReqHash).
               Use seq = perApplyReqDispatchCount[applyReqHash] - 1
               if it was pre-incremented by an in-flight
               dispatchApplyLive, otherwise seq = 0. */
            Hash fallthroughApplyReqHash{HashAlgorithm::SHA256};
            try {
                fallthroughApplyReqHash = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
            } catch (...) {}
            size_t fallthroughSeq = 0;
            if (auto it = ctx.perApplyReqDispatchCount.find(fallthroughApplyReqHash);
                it != ctx.perApplyReqDispatchCount.end() && it->second > 0) {
                fallthroughSeq = it->second - 1;
            }
            Hash fallthroughSeqCtx = hashString(HashAlgorithm::SHA256,
                fallthroughApplyReqHash.to_string(HashFormat::Base16, false)
                + "|" + std::to_string(fallthroughSeq));
            auto rlo = std::make_shared<ReplayLocalObject>(
                std::move(rootSubject), sidecarScope,
                std::make_shared<std::vector<cidasks::Edge>>(),
                std::make_shared<Hash>(HashAlgorithm::SHA256),
                fallthroughSeqCtx, decisionGraph, inner->getEvalState().rootFSRoot,
                &inner->getEvalState());
            rlo->withAmbientAsksValidation();
            try {
                rlo->withChainStart(
                    Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256));
            } catch (const std::exception &) {}
            rlo->withApplyContext(sidecarDepth, sidecarScope);
            tracingCacheLog(
                "resolve %s: chaseLocalArgSidecar direct-standin depth=%d",
                idStr.substr(0, 12), sidecarDepth);
            std::shared_ptr<Object> standin = rlo;
            ctx.memo[idStr] = standin;
            return standin;
        } catch (const std::exception &) {}
    }
    return nullptr;
}

/* Mixed direction: fn is Outer (resolved through the producer chain to
   an AmbientObject); arg may be Local (standin) or Outer (resolved
   through chain). Invokes the apply live against fn and arg to
   materialise the apply result; AmbientObject::queryApply registers the
   result in outerValues. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveApplyId(
    const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx)
{
    auto fnObj = resolveCdiId(params["fn"].get<std::string>(), ctx);
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
           source the cb-arg's structural subject (depth + scope)
           and construct the standin with `PositionalSeed{depth}`
           — matching the recorder's TracingLocalObject subject.

           Using PostulatedIdempotentRead{localId} here is the Fix B
           anti-pattern documented in
           `tracing-eval-cache-per-arg-completion.md`:
           `PostulatedIdempotentRead`'s argStateId is constant in `k`
           (= no own-loop evolution), so once the standin's first
           probe extends the chain, every subsequent probe's
           `stampPerArgFields` reads back `localId` instead of the
           cidasks-evolved argStateId the recorder stamped its facts
           against. The recorded reqHashes then can't be found in
           LocalResponseMap → cb-sibling fails with
           "no recorded response for getType on local". Both
           sibling cb-applies share the same first probe's stamped
           reqHash regardless of subject (= at edgeIndex=0,
           PositionalSeed and PostulatedIdempotentRead both yield `localId`),
           which is why this bug stayed latent until cb-sibling
           landed: it's the first test that needs the standin's
           scopeStateId to *evolve* via subsequent probes for downstream
           discrimination.

           Opt into depth-2 per-probe validation (= each probe
           must appear in some recorded AmbientAsks edge's
           requestSet, or we throw divergence) and root the chain
           at applyReqHash — different cb-applies' chains live in
           disjoint AmbientAsks subtrees; `idStr` IS this apply's
           chain root. */
        auto sidecarPayload = decisionGraph.getRequestPayload(argHash);
        std::shared_ptr<Object> standin;
        if (sidecarPayload) {
            try {
                auto sidecarJson = cborStringToJson(*sidecarPayload);
                if (sidecarJson.contains("depth") && sidecarJson.contains("scope")) {
                    auto sidecarDepth = sidecarJson["depth"].get<int>();
                    auto sidecarScope = Hash::parseNonSRIUnprefixed(
                        sidecarJson["scope"].get<std::string>(), HashAlgorithm::SHA256);
                    cidasks::Subject rootSubject{cidasks::PositionalSeed{sidecarDepth}};
                    Hash fallthroughApplyReqHash2{HashAlgorithm::SHA256};
                    try {
                        fallthroughApplyReqHash2 = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
                    } catch (...) {}
                    size_t fallthroughSeq2 = 0;
                    if (auto it2 = ctx.perApplyReqDispatchCount.find(fallthroughApplyReqHash2);
                        it2 != ctx.perApplyReqDispatchCount.end() && it2->second > 0) {
                        fallthroughSeq2 = it2->second - 1;
                    }
                    Hash fallthroughSeqCtx2 = hashString(HashAlgorithm::SHA256,
                        fallthroughApplyReqHash2.to_string(HashFormat::Base16, false)
                        + "|" + std::to_string(fallthroughSeq2));
                    auto rlo = std::make_shared<ReplayLocalObject>(
                        std::move(rootSubject), sidecarScope,
                        std::make_shared<std::vector<cidasks::Edge>>(),
                        std::make_shared<Hash>(HashAlgorithm::SHA256),
                        fallthroughSeqCtx2, decisionGraph, inner->getEvalState().rootFSRoot,
                        &inner->getEvalState());
                    rlo->withAmbientAsksValidation();
                    try {
                        rlo->withChainStart(
                            Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256));
                    } catch (const std::exception &) {
                        /* idStr should be a valid hex hash here; if not,
                           leave chainCursor at its default
                           (emptySetHash) — the walk will fail safely. */
                    }
                    rlo->withApplyContext(sidecarDepth, sidecarScope);
                    standin = rlo;
                    ctx.memo[argIdStr] = standin;
                }
            } catch (const std::exception &) {
                /* Sidecar malformed — fall through to the PostulatedIdempotentRead
                   fallback below. */
            }
        }
        /* Missing or malformed sidecar = the recorder didn't supply
           the depth/scope needed to reconstruct the cb-arg's
           PositionalSeed Subject. Signal resolution failure so the
           caller falls through to inner re-eval. The previous
           PostulatedIdempotentRead fallback violated principle 8's corollary
           (= observation-driven evolution) and produced a standin
           whose discrimination was frozen at edgeIndex=0. */
        argObj = standin;
    } else {
        argObj = resolveCdiId(argIdStr, ctx);
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
    /* LRM context symmetric with cold's finalize:
       hash(applyReqHash || per-applyReqHash sequence). */
    size_t applySeq = ctx.perApplyReqDispatchCount[applyReqHash]++;
    Hash seqCtx = hashString(HashAlgorithm::SHA256,
        applyReqHash.to_string(HashFormat::Base16, false)
        + "|" + std::to_string(applySeq));
    (void) walkerCur;
    auto fnIdStr = params["fn"].get<std::string>();
    auto fnObj = resolveCdiId(fnIdStr, ctx);
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
            "dispatchApplyLive: arg %s is not a local; no d=2 standin to drive",
            argIdStr.substr(0, 12));
        return std::nullopt;
    }

    /* Cycle break (interim): the live invocation below can still
       trigger walker re-entry through nested cached-fn impls (=
       inside the cb body's `<cached-fn>` on a TLO). Until that path
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

    /* Fresh per-dispatch ReplayLocalObject for the inner-supplied
       value. Per via-Asks Replay (depth-2): the walker reconstructs
       the LocalObject as a live Nix Value tree (= lazily produced
       from CAS atoms), hands it to outer's f, and lets f run
       natively. For lambda LocalObjects, the `<replay-local-lambda>`
       primop the RLO produces consults AmbientAsks at apply-time.
       Per-call discipline: each cb-apply Fact dispatch creates its
       own RLO; no ctx.memo lookup. */
    /* Read the writer's localArg sidecar at argHash. depth+scope are
       required: the structural subject (= PositionalSeed{depth} at
       scope) evolves with observations on cb_arg the same way the
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
        sidecarJson["scope"].get<std::string>(), HashAlgorithm::SHA256);

    cidasks::Subject rootSubject{cidasks::PositionalSeed{sidecarDepth}};
    /* Sibling-discriminating walkFacts seed: inject walker's
       currentProxy's applyContext observations into the RLO's initial
       walk. Without this, RLO's per-arg fields are computed against
       an empty walk — so sibling A's RLO and sibling B's RLO have
       identical CDIs at their initial `.x` / `.f` probes, and LRM's
       first-writer-wins returns whichever sibling recorded first,
       yielding cross-sibling data mixing (cb-sibling-b's int-1000
       result = sibling A's x=1 folded with sibling B's f×1000).
       Injecting the current sibling's applyContext obs makes the
       RLO's CDIs reflect the SPECIFIC sibling context walker is
       operating under. */
    auto seededWalkFacts = std::make_shared<std::vector<cidasks::Edge>>();
    if (auto * proxyTR = dynamic_cast<TracingReplayObject *>(ctx.currentProxy.get())) {
        if (auto proxyCtx = proxyTR->getApplyContext()) {
            for (auto & obs : proxyCtx->observations) {
                cidasks::Edge edge;
                edge.observations.push_back(obs);
                seededWalkFacts->push_back(std::move(edge));
            }
        }
    }
    auto replayLocal = std::make_shared<ReplayLocalObject>(
        std::move(rootSubject), sidecarScope,
        seededWalkFacts,
        std::make_shared<Hash>(HashAlgorithm::SHA256),
        seqCtx, decisionGraph, inner->getEvalState().rootFSRoot,
        &inner->getEvalState());
    replayLocal->withApplyContext(sidecarDepth, sidecarScope);
    replayLocal->withAmbientAsksValidation().withChainStart(applyReqHash);

    /* Invoke outer's f LIVE via the Object-level apply entry. Object-
       level apply preserves the RLO replayLocal as an Object through
       the bridging chain (= AmbientObject::queryApply → applyFn →
       resolver->apply → runOn sees argObj as the RLO, NOT as an
       InterpreterObject wrapping a primop Value). That is what lets
       Change B's TLO-skip kick in and lets outer's `g 5` fire the
       standin's primop directly instead of routing through a
       `<cached-fn>(TLO)` cascade that bypasses the d=2 lambda-LO
       mechanism. The earlier Value-level `mkApp + force` path lost
       the RLO's Object-ness behind two layers of Value wrapping.
       Divergence (= depth-2 mismatch thrown out of the standin's
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
    return ambientResult;
}

/* Outer-direction: derived child id whose producer Request is a
   navigation step (getAttr / getListElem). Resolve parent through the
   producer chain, then perform the live navigation step on it. */
/* Per-arg path navigation with multi-root support. `roots` are the
   live Objects corresponding to the query's `fromCIDs[]` entries (=
   each entry is a cb_arg's standin). The top-level path navigates
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

/* Resolve the query's roots: prefer `fromCIDs[]` if present (=
   per-arg multi-root), fall back to the legacy single `from` field.
   Returns empty vector on resolution failure for any root. */
static std::vector<std::shared_ptr<Object>> resolveRoots(
    const nlohmann::json & params,
    std::function<std::shared_ptr<Object>(const std::string &)> resolve)
{
    std::vector<std::shared_ptr<Object>> roots;
    if (params.contains("fromCIDs")) {
        for (auto & cid : params["fromCIDs"]) {
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
    if (!params.contains("from") && !params.contains("fromCIDs"))
        return nullptr;

    /* Per-arg multi-root: resolve each fromCIDs[] entry to a live
       cb_arg standin, then navigate. The producer query records the
       path-to-parent in `path`; navigation uses both. */
    auto roots = resolveRoots(params,
        [&](const std::string & cid) { return resolveCdiId(cid, ctx); });
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

    if (!params.contains("from"))
        return std::nullopt;


    /* Every ambient response must be live-validated, just like file
       reads and env vars. Resolve each fromCIDs[] entry to a live
       Object (single-root falls back to `from`) and navigate by the
       recorded path. The query body (= leaf op like getAttr "x")
       then runs on the navigated child. */
    auto roots = resolveRoots(params,
        [&](const std::string & cid) { return resolveCdiId(cid, ctx); });
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
    auto v13 = v13Walk(queryHash, std::move(currentProxy));
    if (!v13)
        return std::nullopt;
    tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
    return std::make_pair(
        v13->payload,
        TriePosition{
            .resultNodeHash = v13->resultNodeHash,
            .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            .factSetHash = v13->terminalCur,
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
        obj->withScope(ArgScopeCell::make(nullptr, obj.get_ptr()));
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
        obj->withScope(ArgScopeCell::make(nullptr, obj.get_ptr()));
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
       content-defined: AmbientObject (outer values reached by the
       inner), TracingObject / TracingReplayObject (cached values
       reached by the outer). No counter fallback — per the
       Principles section, identity outside the CLI is grounded in
       observation, not allocation order. If a non-proxy Object
       reaches here it's a wiring bug that has to be addressed at
       its construction site. */
    auto getId = [](Object & obj) -> std::string {
        if (auto hex = obj.getScopeStateIdHex())
            return *hex;
        throw Error(
            "TracingReplayEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    /* Outer-direction applies (= fn is an AmbientObject) must NEVER
       be replayed from cache — the outer value's behaviour is the
       *only* thing that can change between cold and warm, so its
       apply-result must always go through live dispatch. The
       registry intercepts and the TracingReplayObject wrapper's
       lookupResult both serve recorded responses; both are wrong
       for outer-direction. Skip both: invoke fn->queryApply(arg)
       directly, return whatever the AmbientObject yields.
       AmbientObject's own queryFn/applyFn closures handle live
       dispatch + the outer-side validation chain. */
    if (auto * fnAmb = dynamic_cast<AmbientObject *>(fn.get_ptr().get())) {
        (void) fnAmb;
        tracingCacheLog(
            "walker apply: outer-direction (fn is AmbientObject) — live dispatch, no registry");
        auto result = fn->queryApply(arg.get_ptr());
        if (!result)
            throw Error("TracingReplayEvaluator::apply: outer-direction queryApply returned null");
        return ref<Object>(result);
    }

    /* Inner-direction applies: fn is a recorded/cached entity
       (TracingReplayObject from evalFile, TracingLocalObject's
       counterparts, or an opaque argStateId). Each call constructs a
       fresh wrapper. Sibling cb apply invocations share the same
       (fnId, argId) at the boundary by construction (= the arg's
       argStateId is the same positional seed across siblings), so a
       cross-invocation registry keyed by the apply Request hash
       would last-write-wins and conflate sibling invocations'
       per-call observation state — exactly the anti-pattern the
       via-Asks doc's boundary-trace-only discipline calls out. */

    /* Build the ApplyResultSubject from fn/arg constituents — mirror
       of TracingEvaluator::apply. Use polymorphic `getSubject()` so
       apply-result wrappers (TracingReplayObject /
       TracingObject) expose their applyResultSubject as `fn` for
       further applies — their argStateIds evolve via cidasks own-loop
       instead of being frozen by `PostulatedIdempotentRead{this.scopeStateId}`. Fall
       back to PostulatedIdempotentRead only when no Subject is exposed
       (= atom whose argStateId is fully determined at construction). */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnId, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(argId, HashAlgorithm::SHA256);

    cidasks::Subject fnSubj = fn->getSubject()
        ? *fn->getSubject()
        : cidasks::Subject{cidasks::PostulatedIdempotentRead{fnIdHash}};

    cidasks::Subject argSubj = arg->getSubject()
        ? *arg->getSubject()
        : cidasks::Subject{cidasks::PostulatedIdempotentRead{argIdHash}};

    /* Apply boundary's scope combines fn's and arg's inherited scopes
       symmetrically but non-commutatively — mirrors the writer's
       formula in `TracingEvaluator::apply`. */
    Hash applyScope = cidasks::applyScope(fn->getInheritedScope(), arg->getInheritedScope());

    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubj)),
    }};

    /* Walker mirror of TracingEvaluator::apply's option 2 evolution.
       Uses walker.cidasksWalk (the cumulative committed walk), which
       under the 1:1 alignment restructure matches writer.d1CidasksWalk
       edge-for-edge once all prior cb-applies' chains have been
       dispatched. */
    /* Cold-side mirror: writer's d1CidasksWalk grows by one edge at
       markApplyBoundary before applyScopeStateId is computed. Walker
       matches this via a bare synthetic edge (no markApplyBoundary
       side-effects) so its applyScopeStateId lands at the same
       walk-index cold's does. Skip for TLO fn to match cold's
       `!fnIsTlo` condition. */
    bool fnIsTloReplay = dynamic_cast<TracingLocalObject *>(fn.get_ptr().get()) != nullptr;
    if (!fnIsTloReplay) {
        nlohmann::json applyQBoundary = trace::QueryApply{fnId, argId};
        auto applyReqHash = hashString(HashAlgorithm::SHA256, applyQBoundary.dump());
        writer.walkerAppendBoundaryEdge(applyReqHash);
    }
    auto & walk = writer.getD1CidasksWalk();
    auto applyScopeStateId = cidasks::scopeStateIdAt(resultSubject, applyScope, walk, walk.size());
    auto applyScopeStateIdHex = applyScopeStateId.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<cidasks::ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "walker apply: fn=%s arg=%s scope=%s -> applyScopeStateId=%s",
            cidasks::describe(*apr.fn),
            cidasks::describe(*apr.arg),
            applyScope.to_string(HashFormat::Base16, false).substr(0, 12),
            applyScopeStateIdHex.substr(0, 16));
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyScopeStateIdHex,
    };
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, fn, arg]() { return inner->apply(fn, arg); });
    /* Apply-result scope cell. Parent = fn proxy's cell. */
    auto cell = ArgScopeCell::make(effectiveArgScope(*fn), arg.get_ptr());
    obj->withScope(std::move(cell));
    obj->withApplyResultSubject(std::move(resultSubject), applyScope);
    /* Keep the applyContext attachment for the ensureInner-finalisation
       side-channel that other paths still inspect (e.g. tests that
       check applyContext->finalized). Pre-population of observations
       from the Requests pool is no longer needed — evolvedQueryFrom
       reads the evaluator's cidasksWalk instead. */
    if (auto * argAmb = dynamic_cast<AmbientObject *>(arg.get_ptr().get())) {
        if (auto ctx = argAmb->getApplyContext())
            obj->withApplyContextOnly(std::move(ctx));
    }
    return obj;
}

} // namespace nix

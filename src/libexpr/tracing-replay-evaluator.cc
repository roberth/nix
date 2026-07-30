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
TracingReplayEvaluator::walk(
    const Hash & selectorHash,
    std::shared_ptr<Object> currentProxy,
    std::shared_ptr<const ArgCell> cell)
{
    ResolutionContext ctx{
        std::move(currentProxy),
        cell,
        {},
    };
    /* Cell-migration Phase F: walker's per-walk state lives on the
       cell's qState when a cell is provided. When null, fall back to
       a locally-owned QState so no caller is broken by the migration.
       Concurrency invariant: only one walk active at a time; the
       active walk's cell is the state carrier (parent-chain reachable
       from currentProxy). Switching walks = switching active cell. */
    std::shared_ptr<QState> qState = std::make_shared<QState>();
    qState->currentQ = selectorHash;
    if (cell) {
        cell->qState = qState;
        /* #177: back-pointer to the cell so writer-side cell.factSetHash()
           reads and follow-up state-hash lookups can locate the cell. */
        qState->cell = cell;
    }

    auto & responseFor = qState->responseFor;
    auto & committedEdgeFingerprints = qState->committedEdgeFingerprints;
    auto & pendingEdgeObservations = qState->pendingEdgeObservations;

    auto commitEdge = [&]() {
        if (pendingEdgeObservations.empty())
            return;
        Hash fingerprint(HashAlgorithm::SHA256);
        for (const auto & f : pendingEdgeObservations)
            fingerprint = TracingDecisionGraph::xorFactIntoHash(
                fingerprint, f.fromHash, f.elementHash);
        if (committedEdgeFingerprints.insert(fingerprint).second) {
            /* #183: walker-side attribution — route each fact to
               its attributionCell (outer probe → arg's cell), or
               sessionRootCell (env-fact default when null).
               #187: barrier stamp peeks the writer's current value
               (walker's commitEdge doesn't bump — outer probe adds
               go through writer.logOuterObservation which does the
               bump; env facts don't bump anyway). try_emplace on the
               cell map means the first stamp wins, so any race with
               the writer path is idempotent. */
            auto barrier = writer.peekBarrier();
            for (const auto & o : pendingEdgeObservations) {
                auto target = o.attributionCell.lock();
                if (!target) target = writer.sessionRootCell;
                if (target)
                    target->addFact(o.reqHash, o.respHash, barrier);
            }
            tracingCacheLog("dispatch: committed edge (obs=%zu)",
                            pendingEdgeObservations.size());
        } else {
            tracingCacheLog("dispatch: edge already committed (shared prefix), skip");
        }
        pendingEdgeObservations.clear();
    };

    /* Dispatcher: Request hash → Response hash. Memoised in
       responseFor for file reads and env vars — no `from` state,
       response is a pure function of request.

       Outer-value requests skip memo: `from` is pre-response, so
       at the divergent probe two siblings share requestHash while
       their responses differ. After that probe the fold
       advances state hashes, so subsequent probes discriminate
       via requestHash naturally. */
    auto dispatch = [&](const Hash & requestHash, const TracingDecisionGraph::EdgeContext & edgeCtx) -> Hash {
        auto requestPayload = decisionGraph.getRequestPayload(requestHash);
        if (!requestPayload)
            return Hash(HashAlgorithm::SHA256);
        bool isQueryRequest = false;
        bool willMoveStateHash = false;
        std::optional<Hash> outerFromHash;
        std::string queryTag;
        std::string queryDescription;
        try {
            auto reqJson = cborStringToJson(*requestPayload);
            isQueryRequest = reqJson.contains("tag");
            if (isQueryRequest) {
                queryTag = reqJson["tag"].get<std::string>();
                if (auto qSel = trace::nodeFromJson(reqJson, decisionGraph.selectorPool)) {
                    queryDescription = trace::describe(**qSel);
                    willMoveStateHash = trace::willMoveStateHash(**qSel);
                    /* Pre-migration semantics: outer-probe facts flow into cells
                       exclusively via logOuterObservation (from the outer's
                       queryFn attributing to callerCell = arg's own cell). The
                       walker's commitEdge path folds ONLY env facts, never
                       outer probes. Historical fromHashOf was effectively
                       always nullopt (dead-code `!true` guard); post-migration
                       structural cleanup accidentally re-enabled the commitEdge
                       outer-probe fold via `q.parent` matching, which double-
                       folded and (worse) into the wrong cell for applications.
                       Keep outerFromHash always nullopt so no outer probe gets
                       pushed into pendingEdgeObservations. */
                    (void) (*qSel);
                } else {
                    queryDescription = queryTag;
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
        if (!willMoveStateHash) {
            if (auto it = responseFor.find(requestHash); it != responseFor.end())
                return it->second;
        }
        /* Cell-migration Phase B: SelectorApply is now walkable (its
           Terminal is inserted by TracingEvaluator::apply after
           computing the applyResult's WHNF). Dispatch falls through
           to getCurrentResponse → dispatchQueryRequest's SelectorApply
           branch, which resolves fn+arg live and returns the WHNF. */
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
        if (!willMoveStateHash)
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
        /* Buffer facts for this in-flight Asks edge; the
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
        if (isQueryRequest && outerFromHash) {
            /* Attribution: the observation belongs to the cell the walk is
               scoped to (walkCell = the arg's own cell). Under pre-migration
               semantics, SelectorApply's field-name convention (`fn` not `from`)
               kept it out of this branch entirely — commitEdge never folded
               Apply-dispatch facts. Post-migration's `parent` rename brought
               Apply into the branch, but attribution via ctx.currentProxy
               (which is the fn proxy for a top-level apply, whose argCell is
               the shared parent cell) leaked A's terminal-worth of facts into
               siblings' shared cellAnchor. walkCell is the semantically correct
               choice for all step selectors — per Foundational 10 (arguments
               accumulate in their own cell's factset). currentProxy.argCell is
               a backstop for the no-walkCell case (e.g. from evalFile). */
            std::weak_ptr<const ArgCell> attrCell;
            if (ctx.walkCell)
                attrCell = ctx.walkCell;
            else if (ctx.currentProxy)
                attrCell = ctx.currentProxy->getProxyArgCell();
            pendingEdgeObservations.push_back({
                *outerFromHash,
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, h),
                requestHash,
                h,
                std::move(attrCell),
            });
            tracingCacheLog(
                "dispatch outer: req=%s payload=%s from=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                outerFromHash->to_string(HashFormat::Base16, false).substr(0, 12),
                h.to_string(HashFormat::Base16, false).substr(0, 12),
                reqJsonStr,
                respJsonStr);
        } else if (isQueryRequest) {
            tracingCacheLog(
                "dispatch outer (no-from): req=%s payload=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                h.to_string(HashFormat::Base16, false).substr(0, 12),
                reqJsonStr,
                respJsonStr);
        } else {
            /* #183: env facts default to sessionRootCell (attrCell
               left null — commitEdge routes null to sessionRootCell). */
            pendingEdgeObservations.push_back({
                Hash(HashAlgorithm::SHA256),
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, h),
                requestHash,
                h,
                std::weak_ptr<const ArgCell>{},
            });
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

    /* #178: walker Q evolution retires. Q hashes stable per operation;
       recomputeQ becomes identity. Left null so decisionGraph.walk
       skips the call. */
    std::function<Hash(const Hash &)> recomputeQ;

    std::optional<TracingDecisionGraph::WalkHit> walkHit;

    /* Per-walk scope: reset committedEdgeFingerprints to empty for the
       walk, restore on exit so nested walks don't share dedup state. */
    auto savedFingerprints = std::move(committedEdgeFingerprints);
    committedEdgeFingerprints.clear();
    struct WalkScope
    {
        std::unordered_set<Hash> & committedEdgeFingerprints;
        std::unordered_set<Hash> savedFingerprints;
        ~WalkScope()
        {
            committedEdgeFingerprints = std::move(savedFingerprints);
        }
    } walkScope{committedEdgeFingerprints, std::move(savedFingerprints)};

    /* Walk with two anchor candidates in order:
       1. Parent TracingReplayObject's terminalCur — the structural-anchor
          lookup position. Child Q's recording was made starting from
          parent's reached factSet, so anchoring the child history there
          matches the recording's frame.
       2. From ∅ — needed when no parent anchor exists (top-level Q
          like evalFile/evalExpr, no TracingReplayObject) and as a backstop
          when the parent-anchored attempt finds no matching Asks chain. */
    /* #177: anchor at cell.factSetHash() — under the pull model, cold's
       writer keys Terminals at cell.factSetHash(); walker's startCur
       must match. */
    Hash cellAnchor = cell ? cell->factSetHash() : TracingDecisionGraph::emptySetHash();
    walkHit = decisionGraph.walk(selectorHash, dispatch,
        [&](bool committed, const std::vector<Hash> & useful) {
            if (committed) commitEdge();
            else commitRejected(useful);
        },
        cellAnchor,
        recomputeQ);
    /* #187 fallback: if cell-anchored walk misses AND cellAnchor ≠ ∅,
       retry from ∅. Under barrier-based Ask insertion the writer's
       chain starts at ∅; walker at parent.terminalCur may not find
       any Ask edge there. Wrong-hit potential from batched dispatch
       is closed by the barrier design — each per-probe barrier
       validates its response live, so a divergent scenario misses
       at the divergent probe's edge. */
    if (!walkHit && cellAnchor != TracingDecisionGraph::emptySetHash()) {
        tracingCacheLog("walk fallback: retrying from ∅");
        pendingEdgeObservations.clear();
        rejectedObs.clear();
        walkHit = decisionGraph.walk(selectorHash, dispatch,
            [&](bool committed, const std::vector<Hash> & useful) {
                if (committed) commitEdge();
                else commitRejected(useful);
            },
            TracingDecisionGraph::emptySetHash(),
            recomputeQ);
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
        /* Check `tag` first: Query payloads under the flat envelope
           carry a discriminator `tag`, and some Query types also
           happen to have a `name` field (SelectorGetAttr) — without the
           tag check first, they'd fall into the env-var branch and
           produce a wrong response. */
        if (reqJson.contains("tag")) {
            return dispatchQueryRequest(reqJson, ctx);
        } else if (reqJson.contains("absPath")) {
            std::string path = reqJson["absPath"];
            auto currentHash = validationEnv.getFileHash(path);
            nlohmann::json respJson = trace::FileReadResponse{currentHash};
            return jsonToCborString(respJson);
        } else if (reqJson.contains("name")) {
            std::string name = reqJson["name"];
            auto currentVal = validationEnv.getEnv(name);
            nlohmann::json respJson = trace::GetEnvResponse{currentVal};
            return jsonToCborString(respJson);
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to get current response: %s", e.what());
    }
    return std::nullopt;
}

/* Resolve a recorded outer-value id (hex of a Hash) to a live Object.
   First check the per-history memo (ctx.memo) for already-resolved ids.
   Then history the proxy graph (ctx.currentProxy.parent → …) looking
   for an argCell cell whose id matches — this is the arg-lookup
   case, grounded in the proxy whose method triggered this history
   rather than in any evaluator-global state.
   Then fall through to producer-Request resolution: find idStr in
   the Requests pool, resolve the parent recursively, dispatch the
   producer's query on the parent. SelectorApply payloads invoke the
   live apply against a (frozen) ReplayCallbackArg arg. localArg
   sidecars chase to the apply. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveIdentity(const std::string & idStr, ResolutionContext & ctx)
{
    /* Per-history memo. */
    if (auto it = ctx.memo.find(idStr); it != ctx.memo.end()) {
        tracingCacheLog("resolve %s -> memo hit", idStr.substr(0, 12));
        return it->second;
    }

    /* #181: under query-space identity, each Object has a stable
       `getSelectorHashHex()` = the Q hash of the Selector that produced
       it. Cell-chain match is a direct equality check — no K>0 fold,
       no convergence, no subjectId derivation from the subject.
       Q hashes don't evolve. */
    auto cell = ctx.walkCell
                  ? ctx.walkCell
                  : (ctx.currentProxy ? ctx.currentProxy->getProxyArgCell() : nullptr);
    int cellDepth = 0;
    for (; cell; cell = cell->parent, ++cellDepth) {
        auto live = cell->liveObject;
        if (!live) {
            tracingCacheLog("resolve %s: cell[%d] no liveObject", idStr.substr(0, 12), cellDepth);
            continue;
        }
        auto liveHex = live->getSelectorHashHex();
        if (liveHex && *liveHex == idStr) {
            tracingCacheLog("resolve %s: cell[%d] MATCH", idStr.substr(0, 12), cellDepth);
            ctx.memo[idStr] = live;
            return live;
        }
        tracingCacheLog(
            "resolve %s: cell[%d] hex=%s miss",
            idStr.substr(0, 12), cellDepth,
            liveHex ? liveHex->substr(0, 12).c_str() : "(none)");
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
           initial state hash when fired (= registerOuterResolverProxy
           in replay-callback-arg.cc). If we find a matching
           registration here, the OUTER walker resolves to that live
           proxy and dispatches the env fact live against outer's
           actual value — capability-mediated, not cached.

           Without a registration, fall through to nullptr. The via-
           Asks design forbids serving from the Responses pool for
           OUTER values — silently masks outer-body change (cb-
           higher-order step 3 returning stale 6 when outer changed
           from `g 5` to `g 10`). */
        if (auto resolver = inner->getOuterResolver()) {
            if (auto live = tryResolveOuterResolverProxy(*resolver, idHash, &decisionGraph)) {
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

    auto tag = reqJson["tag"].get<std::string>();
    /* Flat envelope: query fields live at top level of reqJson (no "params" wrapper). */
    auto & params = reqJson;

    if (tag == "apply") {
        tracingCacheLog("resolve %s: apply producer", idStr.substr(0, 12));
        return resolveApplyId(idStr, params, ctx);
    }

    if (tag == "callbackApply") {
        /* CBApply as a producer identity — materialise ReplayCallbackArg
           from the referenced obsSet, resolve fn, invoke live, return
           the applyResult Object (for the caller to navigate via
           maybeGetAttr etc.). Mirrors dispatchQueryRequest's CBApply
           branch but returns the Object instead of the serialised WHNF. */
        tracingCacheLog("resolve %s: callbackApply producer", idStr.substr(0, 12));
        std::string fnHex = params["fn"].get<std::string>();
        std::string obsSetHex = params["argObsSet"].get<std::string>();
        Hash obsSetHash{HashAlgorithm::SHA256};
        try {
            obsSetHash = Hash::parseNonSRIUnprefixed(obsSetHex, HashAlgorithm::SHA256);
        } catch (const std::exception &) {
            return nullptr;
        }
        auto obsSet = decisionGraph.getObservationSet(obsSetHash);
        if (!obsSet) {
            tracingCacheLog(
                "resolve %s: callbackApply obsSet=%s not in pool",
                idStr.substr(0, 12), obsSetHex.substr(0, 12).c_str());
            return nullptr;
        }
        auto obsSetMap = std::make_shared<std::map<Hash, std::string>>();
        for (const auto & obs : *obsSet)
            obsSetMap->emplace(obs.selectorHash, obs.responsePayload);
        auto fnObj = resolveIdentity(fnHex, ctx);
        if (!fnObj) {
            tracingCacheLog(
                "resolve %s: callbackApply fn=%s not resolvable",
                idStr.substr(0, 12), fnHex.substr(0, 12).c_str());
            return nullptr;
        }
        auto argProducerSel = decisionGraph.selectorPool.intern(trace::SelectorArg{0});
        auto walkFacts = std::make_shared<std::vector<ObservationSet>>();
        auto replayArg = std::make_shared<ReplayCallbackArg>(
            argProducerSel,
            walkFacts,
            decisionGraph, inner->getEvalState().rootFSRoot,
            &inner->getEvalState());
        replayArg->withObsSetResponses(obsSetMap);
        try {
            auto resultObj = fnObj->queryApply(replayArg);
            if (resultObj)
                ctx.memo[idStr] = resultObj;
            return resultObj;
        } catch (const std::exception & e) {
            tracingCacheLog("resolve %s: callbackApply queryApply threw: %s",
                idStr.substr(0, 12), e.what());
            return nullptr;
        }
    }

    auto qSel = trace::nodeFromJson(reqJson, decisionGraph.selectorPool);
    if (qSel) {
        tracingCacheLog(
            "resolve %s: producer-child %s",
            idStr.substr(0, 12), trace::describe(**qSel).c_str());
    } else {
        tracingCacheLog(
            "resolve %s: producer-child via %s (unparseable)",
            idStr.substr(0, 12), tag.c_str());
        return nullptr;
    }
    return resolveProducerChild(idStr, (*qSel)->node, params, ctx);
}

/* Resolve an "apply" producer's result by resolving fn + arg and
   invoking fn.queryApply(arg) live. Arg resolution is uniform (via
   resolveIdentity) — the historical localArg-sidecar special case
   is gone: callback-arg observations ride in the CallbackApply
   query's obsSet, not through producer-chain apply resolution. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveApplyId(
    const std::string & idStr, const nlohmann::json & params, ResolutionContext & ctx)
{
    auto fnObj = resolveIdentity(params["fn"].get<std::string>(), ctx);
    if (!fnObj) {
        tracingCacheLog("replay: apply %s: cannot resolve fn %s", idStr, params["fn"]);
        return nullptr;
    }
    auto argIdStr = params["arg"].get<std::string>();
    auto argObj = resolveIdentity(argIdStr, ctx);
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


/* `perArgFrame` accessor helpers — the sub-object is the standard
   home for `fromStateHashes` + `path`; the top-level `fromStateHashes`
   variant is only for SelectorApply. */
static const nlohmann::json * perArgFrameOf(const nlohmann::json & params)
{
    if (params.contains("perArgFrame") && params["perArgFrame"].is_object())
        return &params["perArgFrame"];
    return nullptr;
}

static trace::PathExpr parsePathFromParams(const nlohmann::json & params)
{
    trace::PathExpr path;
    if (auto * frame = perArgFrameOf(params); frame && frame->contains("path"))
        from_json(frame->at("path"), path);
    return path;
}

static std::vector<std::shared_ptr<Object>> resolveRoots(
    const nlohmann::json & params,
    std::function<std::shared_ptr<Object>(const std::string &)> resolve)
{
    std::vector<std::shared_ptr<Object>> roots;
    auto tryRoots = [&](const nlohmann::json & arr) -> bool {
        for (auto & cid : arr) {
            std::string cidHex;
            if (cid.is_string())
                cidHex = cid.get<std::string>();
            else if (cid.is_object() && cid.contains("content"))
                cidHex = cid["content"].get<std::string>();
            else if (cid.is_object() && cid.contains("stateHash"))
                cidHex = cid["stateHash"].get<std::string>();
            else
                return false;
            auto obj = resolve(cidHex);
            if (!obj)
                return false;
            roots.push_back(std::move(obj));
        }
        return true;
    };
    if (auto * frame = perArgFrameOf(params); frame && frame->contains("fromStateHashes")
        && (*frame)["fromStateHashes"].is_array()) {
        if (tryRoots((*frame)["fromStateHashes"]))
            return roots;
        return {};
    }
    if (params.contains("fromStateHashes") && params["fromStateHashes"].is_array()) {  // SelectorApply
        if (tryRoots(params["fromStateHashes"]))
            return roots;
        return {};
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
    const std::vector<std::shared_ptr<Object>> & roots, const trace::PathExpr & path,
    TracingWriter * writer = nullptr)
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
            auto fnObj = navigatePath(fnRoots, *step.fnPath, writer);
            auto argObj = navigatePath(argRoots, *step.argPath, writer);
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
    const std::string & idStr, const trace::SelectorNode & qv, const nlohmann::json & params, ResolutionContext & ctx)
{
    if (!params.contains("from") && !params.contains("fromStateHashes")
        && !params.contains("perArgFrame"))
        return nullptr;

    /* Per-arg multi-root: resolve each fromStateHashes[] entry to a live
       cb_arg ReplayCallbackArg, then navigate. The producer query records the
       path-to-parent in `path`; navigation uses both. */
    auto roots = resolveRoots(params,
        [&](const std::string & cid) { return resolveIdentity(cid, ctx); });
    if (roots.empty())
        return nullptr;
    auto parent = navigatePath(roots, parsePathFromParams(params), &writer);
    if (!parent)
        return nullptr;

    auto child = std::visit(
        [&](const auto & q) -> std::shared_ptr<Object> {
            using Q = std::decay_t<decltype(q)>;
            try {
                if constexpr (std::is_same_v<Q, trace::SelectorGetAttr>) {
                    return parent->maybeGetAttr(q.name);
                } else if constexpr (std::is_same_v<Q, trace::SelectorGetListElem>) {
                    return parent->getListElem(q.index);
                } else {
                    return nullptr;
                }
            } catch (const std::exception & e) {
                tracingCacheLog("replay: failed to resolve %s producer for %s: %s",
                    Q::tag, idStr, e.what());
                return nullptr;
            }
        },
        qv);

    if (child)
        ctx.memo[idStr] = child;
    return child;
}

std::optional<std::string> TracingReplayEvaluator::dispatchQueryRequest(const nlohmann::json & reqJson, ResolutionContext & ctx)
{
    auto qSelOpt = trace::nodeFromJson(reqJson, decisionGraph.selectorPool);
    if (!qSelOpt)
        return std::nullopt;
    auto & qv = (*qSelOpt)->node;

    auto resolveParent = [&](ref<const trace::Selector> parentSel) -> std::shared_ptr<Object> {
        return resolveIdentity(parentSel->cachedHash.to_string(HashFormat::Base16, false), ctx);
    };

    return std::visit(
        [&](const auto & q) -> std::optional<std::string> {
            using Q = std::decay_t<decltype(q)>;
            if constexpr (std::is_same_v<Q, trace::SelectorApply>) {
                /* Under the Selector-is-a-sequence model, a SelectorApply
                   request identifies "the value produced by this apply" —
                   the applyResult that some cell holds as its liveObject.
                   Compute the request's hash, resolve via cell chain by
                   producer-hex equality, return WHNF of the resolved
                   liveObject. */
                auto selfHash = TracingDecisionGraph::computeSelectorHash(q);
                auto selfHex = selfHash.to_string(HashFormat::Base16, false);
                auto obj = resolveIdentity(selfHex, ctx);
                if (!obj) {
                    tracingCacheLog(
                        "apply: self resolution miss (self=%s)",
                        selfHex.substr(0, 12).c_str());
                    return std::nullopt;
                }
                try {
                    auto whnf = computeWHNFFromObject(*obj);
                    tracingCacheLog(
                        "apply: HIT self=%s whnf=%s",
                        selfHex.substr(0, 12).c_str(),
                        whnf.type.c_str());
                    return jsonToCborString(nlohmann::json(whnf));
                } catch (const std::exception & e) {
                    tracingCacheLog("apply: computeWHNFFromObject threw: %s", e.what());
                    return std::nullopt;
                }
            } else if constexpr (std::is_same_v<Q, trace::SelectorCallbackApply>) {
                /* Task #110: materialise a ReplayCallbackArg backed by
                   the referenced ObservationSet, resolve fn live via
                   subject-navigation, invoke fn->queryApply(replayArg),
                   return the applyResult's WHNF. */
                std::string fnHex = q.parent->cachedHash.to_string(HashFormat::Base16, false);
                Hash obsSetHash{HashAlgorithm::SHA256};
                try {
                    obsSetHash = Hash::parseNonSRIUnprefixed(q.argObsSet, HashAlgorithm::SHA256);
                } catch (const std::exception &) {
                    return std::nullopt;
                }
                auto obsSet = decisionGraph.getObservationSet(obsSetHash);
                if (!obsSet) {
                    tracingCacheLog(
                        "callbackApply: obsSet=%s not in pool — miss",
                        q.argObsSet.substr(0, 12));
                    return std::nullopt;
                }
                auto obsSetMap = std::make_shared<std::map<Hash, std::string>>();
                for (const auto & obs : *obsSet)
                    obsSetMap->emplace(obs.selectorHash, obs.responsePayload);
                /* #178: perArgFrame retired; resolve fn by state hash. */
                std::shared_ptr<Object> fnObj = resolveIdentity(fnHex, ctx);
                if (!fnObj) {
                    tracingCacheLog(
                        "callbackApply: fn resolution miss (fn=%s)",
                        fnHex.substr(0, 12));
                    return std::nullopt;
                }
                /* Contra-arg identity: hardcoded sentinel matching
                   writer's OuterApply::run and reader's replay-callback-arg.
                   Scoped by this enclosing SelectorCallbackApply. */
                auto argProducerSel = decisionGraph.selectorPool.intern(trace::SelectorArg{0});
                auto walkFacts = std::make_shared<std::vector<ObservationSet>>();
                auto replayArg = std::make_shared<ReplayCallbackArg>(
                    argProducerSel,
                    walkFacts,
                    decisionGraph, inner->getEvalState().rootFSRoot,
                    &inner->getEvalState());
                replayArg->withObsSetResponses(obsSetMap);
                try {
                    auto resultObj = fnObj->queryApply(replayArg);
                    if (!resultObj)
                        return std::nullopt;
                    auto whnf = computeWHNFFromObject(*resultObj);
                    tracingCacheLog(
                        "callbackApply: HIT obsSet=%s whnf=%s",
                        q.argObsSet.substr(0, 12), whnf.type.c_str());
                    return jsonToCborString(nlohmann::json(whnf));
                } catch (const std::exception & e) {
                    tracingCacheLog("callbackApply: fn->queryApply failed: %s", e.what());
                    return std::nullopt;
                }
            } else if constexpr (std::is_same_v<Q, trace::SelectorArg>) {
                /* Contra-arg identity: SelectorArg with a hardcoded
                   depth chosen by writer/reader agreement (its
                   containing SelectorCallbackApply scopes it). Resolve
                   by content-hash equality against liveObjects in the
                   cell chain — the callback firing's TracingCallbackArg
                   / ReplayCallbackArg reports the matching hex. */
                auto selfHash = TracingDecisionGraph::computeSelectorHash(q);
                auto selfHex = selfHash.to_string(HashFormat::Base16, false);
                auto obj = resolveIdentity(selfHex, ctx);
                if (!obj) {
                    tracingCacheLog(
                        "arg: self resolution miss (self=%s)",
                        selfHex.substr(0, 12).c_str());
                    return std::nullopt;
                }
                try {
                    auto whnf = computeWHNFFromObject(*obj);
                    tracingCacheLog(
                        "arg: HIT self=%s whnf=%s",
                        selfHex.substr(0, 12).c_str(),
                        whnf.type.c_str());
                    return jsonToCborString(nlohmann::json(whnf));
                } catch (const std::exception & e) {
                    tracingCacheLog("arg: computeWHNFFromObject threw: %s", e.what());
                    return std::nullopt;
                }
            } else if constexpr (std::is_same_v<Q, trace::SelectorGetAttr>
                                 || std::is_same_v<Q, trace::SelectorGetListElem>
                                 || std::is_same_v<Q, trace::SelectorGetFunctionInfo>) {
                /* Retrieval getters: resolve the parent Object via parent
                   Selector's cachedHash and dispatch the specific probe. */
                auto obj = resolveParent(q.parent);
                if (!obj) return std::nullopt;

                nlohmann::json resultJson;
                try {
                    if constexpr (std::is_same_v<Q, trace::SelectorGetAttr>) {
                        /* Pure retrieval — caller (walker) has projected
                           membership from parent's WHNFAttrs. */
                        auto child = obj->maybeGetAttr(q.name);
                        if (!child) return std::nullopt;
                        resultJson = computeWHNFFromObject(*child);
                    } else if constexpr (std::is_same_v<Q, trace::SelectorGetListElem>) {
                        auto child = obj->getListElem(q.index);
                        resultJson = computeWHNFFromObject(*child);
                    } else if constexpr (std::is_same_v<Q, trace::SelectorGetFunctionInfo>) {
                        auto info = obj->getFunctionInfo();
                        if (!info)
                            resultJson = trace::ResultFunctionInfo{false, {}, false};
                        else
                            resultJson = trace::ResultFunctionInfo{true, info->formals, info->ellipsis};
                    } else {
                        return std::nullopt;
                    }
                } catch (const std::exception & e) {
                    tracingCacheLog("replay: dispatch failed for %s: %s", Q::tag, e.what());
                    return std::nullopt;
                }
                return jsonToCborString(resultJson);
            } else {
                /* SelectorExpr / SelectorImport aren't dispatched through this
                   path (root queries handled elsewhere). */
                return std::nullopt;
            }
        },
        qv);
}

template<typename Q>
std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup(const Q & query, std::shared_ptr<Object> currentProxy, std::shared_ptr<const ArgCell> cell)
{
    auto selectorHash = TracingDecisionGraph::computeSelectorHash(query);
    /* Task #110: pass Q's typed payload so the walker can re-derive
       Q's `from` field as observations dispatch. No subject is passed
       from lookup()'s template path — probes with applyResultSubject
       come through a different code path (TracingReplayObject) which
       calls walk() directly with the appropriate subject.

       Phase F: forward the cell so walker's per-walk state lives on
       cell.qState. Callers with a cell (evalFile/evalExpr root cell,
       apply's applyResult cell) pass it; others pass nullptr. */
    auto walkResult = walk(selectorHash, std::move(currentProxy), std::move(cell));
    if (!walkResult)
        return std::nullopt;
    tracingCacheLog("replay hit: %s", Q::tag);
    return std::make_pair(
        walkResult->payload,
        TriePosition{
            .resultNodeHash = walkResult->resultNodeHash,
            .queryHashStr = selectorHash.to_string(HashFormat::Base16, false),
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
    /* Phase F: create the root cell BEFORE the lookup so walker's
       per-walk state lives on cell.qState. liveObject is back-filled
       after the TracingReplayObject wrapper is constructed. */
    auto rootCell = ArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorImport rootSel{displayPath};
    if (auto result = lookup(rootSel, nullptr, rootCell)) {
        tracingCacheLog("replay hit: evalFile %s", displayPath);
        auto obj = make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
        try {
            auto whnfJson = cborStringToJson(result->first);
            trace::ResultWHNF parsed;
            from_json(whnfJson, parsed);
            obj->withCachedWHNF(std::move(parsed));
        } catch (const std::exception &) { /* fall through */ }
        rootCell->liveObject = obj.get_ptr();
        obj->withArgCell(rootCell);
        /* Bootstrap the pool with this root Selector. */
        obj->withProducer(decisionGraph.selectorPool.intern(rootSel));
        return obj;
    }
    tracingCacheLog("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    /* Phase F: create root cell before lookup; back-fill liveObject
       after wrapping. */
    auto rootCell = ArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorExpr rootSel{expr, basePath.path.abs()};
    if (auto result = lookup(rootSel, nullptr, rootCell)) {
        tracingCacheLog("replay hit: evalExpr");
        auto obj = make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
        try {
            auto whnfJson = cborStringToJson(result->first);
            trace::ResultWHNF parsed;
            from_json(whnfJson, parsed);
            obj->withCachedWHNF(std::move(parsed));
        } catch (const std::exception &) { /* fall through */ }
        rootCell->liveObject = obj.get_ptr();
        obj->withArgCell(rootCell);
        obj->withProducer(decisionGraph.selectorPool.intern(rootSel));
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
        if (auto hex = obj.getSelectorHashHex())
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

    /* Build the apply-result producer — mirror of TE::apply.
       fn's identity hex comes directly from getSelectorHashHex();
       nullopt falls back to fnStateHashStr. arg dropped per #181. */
    auto fnQHex = fn->getSelectorHashHex().value_or(fnStateHashStr);
    /* Look up fn's Selector via getSelector(); fall back to pool by hex. */
    std::optional<ref<const trace::Selector>> fnSelOpt = fn->getSelector();
    if (!fnSelOpt) {
        try {
            auto h = Hash::parseNonSRIUnprefixed(fnStateHashStr, HashAlgorithm::SHA256);
            fnSelOpt = decisionGraph.selectorPool.find(h);
        } catch (...) {}
    }
    if (!fnSelOpt)
        throw Error("TracingReplayEvaluator apply: cannot resolve fn Selector for %s", fnStateHashStr);
    auto applySel = decisionGraph.selectorPool.intern(trace::SelectorApply{*fnSelOpt});
    auto & resultProducer = std::get<trace::SelectorApply>(applySel->node);

    auto qHash = applySel->cachedHash;
    auto qHex = qHash.to_string(HashFormat::Base16, false);
    tracingCacheLog(
        "walker apply: fn=%s -> qHash=%s",
        fnQHex.substr(0, 12),
        qHex.substr(0, 16));

    /* Cell-migration Phase B: pre-invoke SelectorApply's lookup so the
       applyResult wrapper can be constructed with its cached WHNF
       already populated from cold's Terminal — mirrors the writer
       side's eager WHNF computation. On hit, downstream `.foo` probes
       on the wrapper use cachedWHNF for membership without a
       separate walk. On miss, we fall back to the
       lazy-inner-apply TRO (with no cachedWHNF), which will trigger
       inner->apply when forced.

       Pass `fn` as currentProxy so the walker's `resolveIdentity`
       has a cell chain to walk when the SelectorApply dispatch
       resolves fn/arg identities — fn's own cell chain roots the
       resolution up to the outer cache-boundary arg. Without a
       currentProxy the cell chain is empty and resolveIdentity
       falls through to the pool + live-proxy registration path,
       which under DISALLOW_PARSE cascades into inner parsing.

       Apply-result argAncestry cell. Parent = fn proxy's cell. */
    /* #183: mirror TE::apply — reuse arg's existing cell (one cell
       per call). Under #188's consolidation the arg always has a
       cell by this point (seedCell on the primop path, applyCell
       propagation on nested callback paths); panic on any fallback
       so a future regression surfaces immediately instead of
       silently allocating a redundant cell. */
    auto cell = effectiveArgCell(*arg);
    if (!cell)
        throw Error("TracingReplayEvaluator apply: arg had no argCell (fn=%s arg=%s)",
                    fnStateHashStr.substr(0, 12), argStateHashStr.substr(0, 12));
    auto & applySelector = std::get<trace::SelectorApply>(applySel->node);
    auto applySelectorHash = applySel->cachedHash;
    /* Phase F: pass the applyResult cell so walker's per-walk state
       lives on cell.qState — cell chain reachable from parent (fn's
       cell), qState reset for this walk's dispatches. */
    auto applyLookup = lookup(applySelector, fn.get_ptr(), cell);
    std::optional<trace::ResultWHNF> cachedWHNF;
    /* #181: query-space identity — use SelectorApply's Q hash so
       downstream applies see fn->getSelectorHashHex() = this Q hash
       (matches cold's TE::apply where triePos.queryHashStr =
       qh.selectorHash from writer.logResult).
       factSetHash = cell.factSetHash() so downstream getter
       direct-lookups anchor at the caller's cell (matches cold's
       logQueryResult anchor = triePos.factSetHash of the parent
       TracingObject wrapping the apply result). */
    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applySelectorHash.to_string(HashFormat::Base16, false),
        .factSetHash = cell->factSetHash(),
    };
    if (applyLookup) {
        try {
            auto whnfJson = cborStringToJson(applyLookup->first);
            trace::ResultWHNF parsed;
            from_json(whnfJson, parsed);
            cachedWHNF = std::move(parsed);
            triePos = applyLookup->second;
        } catch (const std::exception &) {
            /* Parse failure — fall through to lazy path. */
        }
    }
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, fn, arg]() { return inner->apply(fn, arg); });
    obj->withArgCell(std::move(cell));
    obj->withProducer(applySel);
    if (cachedWHNF)
        obj->withCachedWHNF(std::move(*cachedWHNF));
    return obj;
}

/* Explicit instantiations for public consumers of lookup() (e.g.
   TracingReplayObject::lookupStructuralChild). Add new Selector
   variants here as callers appear. */
template std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup<trace::SelectorGetAttr>(
    const trace::SelectorGetAttr &,
    std::shared_ptr<Object>,
    std::shared_ptr<const ArgCell>);
template std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup<trace::SelectorGetListElem>(
    const trace::SelectorGetListElem &,
    std::shared_ptr<Object>,
    std::shared_ptr<const ArgCell>);

} // namespace nix

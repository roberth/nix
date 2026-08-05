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
    const TracingHash & selectorHash,
    std::shared_ptr<Object> currentProxy,
    std::shared_ptr<ArgCell> cell)
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
        TracingHash fingerprint = TracingHash::zero();
        for (const auto & f : pendingEdgeObservations)
            fingerprint = TracingDecisionGraph::xorHashes(fingerprint, f.elementHash);
        if (committedEdgeFingerprints.insert(fingerprint).second) {
            /* Replay validation must not mutate writer state — recording
               and replay run through the same TracingWriter but their
               cells are semantically distinct. Prior code attributed
               walker-dispatched facts to writer.sessionRootCell (env
               default) or the writer-side attributionCell, seeding
               "same probe observed through both flows" duplicates that
               XOR-cancel in cell.factSetHash and break structural-chain
               deltas. Walker's fingerprint tracking is sufficient for
               dedup and validation; writer cells only reflect actual
               inner-cold interpreter work. */
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
    auto dispatch = [&](const TracingHash & requestHash) -> TracingHash {
        auto req = decisionGraph.getRequest(requestHash);
        if (!req)
            return trace::tracingZeroHash();
        bool isQueryRequest = std::holds_alternative<trace::OuterValueRequest>(*req);
        bool willMoveStateHash = false;
        bool ovrIsSelfIdentifying = false;
        std::string queryDescription = std::visit(overloaded{
            [](const trace::FileReadRequest & r) {
                return "env-file " + r.absPath;
            },
            [](const trace::GetEnvRequest & r) {
                return "env-var " + r.name;
            },
            [&](const trace::OuterValueRequest & r) {
                willMoveStateHash = trace::willMoveStateHash(*r.query);
                /* Only SelectorCallbackApply encodes enough context
                   in its identity (argObsSet) to safely cross-context
                   memoize. Other OVR selectors (GetAttr, GetListElem,
                   Apply, GetFunctionInfo) can share request hashes
                   across contexts with different responses (e.g.
                   `.n` on cachedFib{n=10} vs cachedFib{n=9} — same
                   Selector, different response). */
                ovrIsSelfIdentifying =
                    std::holds_alternative<trace::SelectorCallbackApply>(r.query->node);
                return trace::describe(*r.query);
            },
        }, *req);
        if (!willMoveStateHash) {
            if (auto it = responseFor.find(requestHash); it != responseFor.end())
                return it->second;
        }
        /* Cell-scoped OVR memo consult: for OuterValueRequests, look
           up the request hash in the chain's per-cell outerResponseMemo.
           On hit, return the memoized response hash directly — no
           live dispatch, no payload materialisation. This is what
           enables cold's Nth (N≥3) invocation of the same cached fn
           to skip re-dispatching an OVR that the 2nd walker validation
           already computed. */
        if (ovrIsSelfIdentifying) {
            for (auto c = ctx.walkCell.get(); c; c = c->parent.get()) {
                auto it = c->outerResponseMemo.find(requestHash);
                if (it != c->outerResponseMemo.end()) {
                    tracingCacheLog(
                        "dispatch OVR memo hit: req=%s resp=%s cellDepth=%d",
                        requestHash.toHex().substr(0, 12),
                        it->second.toHex().substr(0, 12),
                        c->depth);
                    return it->second;
                }
            }
        }
        /* Cell-migration Phase B: SelectorApply is now walkable (its
           Terminal is inserted by TracingEvaluator::apply after
           computing the applyResult's WHNF). Dispatch falls through
           to computeLiveResponse → dispatchQueryRequest's SelectorApply
           branch, which resolves fn+arg live and returns the WHNF. */
        auto currentResp = computeLiveResponse(*req, ctx);
        if (!currentResp) {
            tracingCacheLog(
                "dispatch FAIL req=%s payload=%s (no current response)",
                requestHash.toHex().substr(0, 12),
                queryDescription);
            return trace::tracingZeroHash();
        }
        auto h = TracingDecisionGraph::computeResponseHash(*currentResp);
        /* Env dispatch MUST NOT read stored responses to substitute
           for a live response that differs from cold's — doing so
           masks legitimate outer-body change detection (per the
           design's capability-mediated invariant) AND, even under
           `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1`, breaks
           observation-driven sibling discrimination
           (cb-sibling-discrimination-via-observation): substituting
           a stored response for a wrong-sibling live response would
           route both siblings to the same recorded terminal. Only
           substitute on DISPATCH FAILURE (see the block above), not
           on mismatch. */
        if (!willMoveStateHash)
            responseFor.emplace(requestHash, h);
        /* Cell-scoped OVR memo populate: after a fresh live dispatch of
           an OuterValueRequest, remember the response payload on the
           parent of the current walk cell (`ctx.walkCell->parent`).
           That's the shared cell across applies of the same cached fn
           (seedCell.parent = fn.rootCell in the primop-apply case);
           storing there lets subsequent walker calls from other
           per-apply cells find the memo by walking up. Only populate
           on FRESH dispatch — don't re-store when we ourselves hit
           the memo. */
        if (ovrIsSelfIdentifying && ctx.walkCell && ctx.walkCell->parent) {
            ctx.walkCell->parent->outerResponseMemo.try_emplace(requestHash, h);
        }
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
        if (isQueryRequest) {
            /* Outer-probe facts fold into cells exclusively via logOuterObservation
               (from the outer's queryFn attributing to callerCell = arg's own
               cell). The walker's commitEdge path handles env facts only, never
               outer probes — matches pre-migration behavior where fromHashOf's
               dead-code guard kept this equivalent branch dormant.

               The reqJSON/respJSON dumps are constructed inline as macro
               args so they only run when tracingCacheLog is enabled; the
               nlohmann JSON tree + dump_escaped are the dominant cost of
               cold runs otherwise. */
            tracingCacheLog(
                "dispatch outer: req=%s payload=%s resp=%s\n  reqJSON=%s\n  respJSON=%s",
                requestHash.toHex().substr(0, 12),
                queryDescription,
                h.toHex().substr(0, 12),
                std::visit([](const auto & r) { return nlohmann::json(r).dump(); }, *req),
                [&]() -> std::string {
                    try { return cborStringToJson(*currentResp).dump(); }
                    catch (...) { return "(unparseable)"; }
                }());
        } else {
            /* #183: env facts default to sessionRootCell (attrCell
               left null — commitEdge routes null to sessionRootCell). */
            pendingEdgeObservations.push_back({
                TracingDecisionGraph::xorFactIntoHash(
                    trace::tracingZeroHash(), requestHash, h),
                requestHash,
                h,
                std::weak_ptr<ArgCell>{},
            });
            tracingCacheLog(
                "dispatch env: req=%s payload=%s resp=%s",
                requestHash.toHex().substr(0, 12),
                queryDescription,
                h.toHex().substr(0, 12));
        }
        return h;
    };

    std::vector<Observation> rejectedObs;
    auto commitRejected = [&](const std::vector<TracingHash> &) {
        for (auto & obs : pendingEdgeObservations)
            rejectedObs.push_back(std::move(obs));
        pendingEdgeObservations.clear();
    };

    std::optional<TracingDecisionGraph::WalkHit> walkHit;

    /* Per-walk scope: reset committedEdgeFingerprints to empty for the
       walk, restore on exit so nested walks don't share dedup state. */
    auto savedFingerprints = std::move(committedEdgeFingerprints);
    committedEdgeFingerprints.clear();
    struct WalkScope
    {
        std::unordered_set<TracingHash> & committedEdgeFingerprints;
        std::unordered_set<TracingHash> savedFingerprints;
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
    TracingHash cellAnchor = cell ? cell->factSetHash() : TracingDecisionGraph::emptySetHash();
    walkHit = decisionGraph.walk(selectorHash, dispatch,
        [&](bool committed, const std::vector<TracingHash> & useful) {
            if (committed) commitEdge();
            else commitRejected(useful);
        },
        cellAnchor);
    /* Task 237: structural-anchor try. If cell-anchor missed, and the
       walker was invoked with a currentProxy that is a TracingReplayObject,
       its triePos.factSetHash is the parent-Q's terminalCur (per
       WalkResult docs). The writer's structural landing chain
       (tracing-writer.hh insertBarrieredAskChain) inserts Asks under
       (child.selectorHash, parent.terminalCur); trying that startCur
       lets the walker reach the structural chain without a full
       ∅-fallback traversal, and enables task 1b (retiring the ∅-chain
       for getter-parent-getter cases). */
    if (!walkHit && ctx.currentProxy) {
        if (auto tro = std::dynamic_pointer_cast<TracingReplayObject>(ctx.currentProxy)) {
            auto structuralAnchor = tro->getTriePos().factSetHash;
            if (structuralAnchor != cellAnchor
                && structuralAnchor != TracingDecisionGraph::emptySetHash()) {
                tracingCacheLog("walk fallback: retrying at parent-TRO structural anchor %s",
                                structuralAnchor.toHex().substr(0, 12).c_str());
                pendingEdgeObservations.clear();
                rejectedObs.clear();
                walkHit = decisionGraph.walk(selectorHash, dispatch,
                    [&](bool committed, const std::vector<TracingHash> & useful) {
                        if (committed) commitEdge();
                        else commitRejected(useful);
                    },
                    structuralAnchor);
            }
        }
    }
    /* #187 fallback: if cell-anchored + structural-anchor walks missed,
       retry from ∅. Wrong-hit potential from batched dispatch is
       closed by the barrier design — each per-probe barrier validates
       its response live, so a divergent scenario misses at the
       divergent probe's edge. */
    if (!walkHit && cellAnchor != TracingDecisionGraph::emptySetHash()) {
        tracingCacheLog("walk fallback: retrying from ∅");
        pendingEdgeObservations.clear();
        rejectedObs.clear();
        walkHit = decisionGraph.walk(selectorHash, dispatch,
            [&](bool committed, const std::vector<TracingHash> & useful) {
                if (committed) commitEdge();
                else commitRejected(useful);
            },
            TracingDecisionGraph::emptySetHash());
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

std::optional<std::string> TracingReplayEvaluator::computeLiveResponse(const trace::Request & req, ResolutionContext & ctx)
{
    /* Env-request memoization: FileReadRequest and GetEnvRequest are
       pure functions of the request (the env is stable within a
       session). Two probes of the same file / env var during the
       session yield the same response; skip the re-dispatch. Attacks
       the 46% inclusive `getFileHash` slice in the P1a profile —
       the walker re-dispatches the same file-content probe across
       many Q walks per session. Not memoized for OuterValueRequest:
       response there depends on the walker's current cell chain and
       cannot be keyed on the request alone. */
    std::string memoKey;
    std::visit(overloaded{
        [&](const trace::FileReadRequest & r) { memoKey = "F:" + r.absPath; },
        [&](const trace::GetEnvRequest & r) { memoKey = "E:" + r.name; },
        [&](const trace::OuterValueRequest &) { /* not memoized */ },
    }, req);
    if (!memoKey.empty()) {
        if (auto it = envResponseMemo.find(memoKey); it != envResponseMemo.end())
            return it->second;
    }
    try {
        auto result = std::visit(overloaded{
            [&](const trace::FileReadRequest & r) -> std::optional<std::string> {
                auto currentHash = validationEnv.getFileHash(r.absPath);
                return trace::encodeResponsePayload(trace::FileReadResponse{currentHash});
            },
            [&](const trace::GetEnvRequest & r) -> std::optional<std::string> {
                auto currentVal = validationEnv.getEnv(r.name);
                return trace::encodeResponsePayload(trace::GetEnvResponse{currentVal});
            },
            [&](const trace::OuterValueRequest & r) -> std::optional<std::string> {
                return dispatchQueryRequest(trace::toJson(*r.query), ctx);
            },
        }, req);
        if (!memoKey.empty() && result)
            envResponseMemo.emplace(memoKey, *result);
        return result;
    } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
        tracingCacheLog("replay: failed to get current response: %s", e.what());
    }
    return std::nullopt;
}

/* ReuseHit is declared in tracing-replay-evaluator.hh — exposed for
   property tests to pin down reuse behavior in isolation. */

std::optional<size_t> reuseMatchScore(
    const std::vector<TracingDecisionGraph::InlineFact> & cellObs,
    const std::map<TracingHash, std::string> & incoming)
{
    /* Current criterion: cellObs ⊆ incoming (every entry in cellObs
       matches an entry in incoming on both reqHash and response).
       Score = number of matching entries. */
    size_t score = 0;
    for (auto & fact : cellObs) {
        auto it = incoming.find(fact.reqHash);
        if (it == incoming.end() || it->second != fact.responsePayload)
            return std::nullopt;
        ++score;
    }
    return score;
}

/**
 * Look for a live callback application cell in the current chain
 * whose `runningObsSet` is a subset of `incoming` (every fact in
 * the cell agrees with `incoming` on both reqHash and response).
 * Among subset candidates, prefer the largest — the cell whose
 * observations are the most specific match short of exceeding
 * `incoming`.
 *
 * Subset (not intersection-compat) is what isolates siblings: two
 * callbacks with different fn bodies observe their contra-arg
 * differently, so neither's obs is a subset of the other's — no
 * false reuse across sibling firings.
 *
 * Fn identity via `SelectorArg{depth}` is positional and coarser
 * than actual fn identity (two different lambdas at the same
 * syntactic slot hash the same), so filtering by `initialFnHex`
 * is unsafe. Under subset, coincidentally-hex-colliding fns with
 * genuinely different observations correctly fail the subset
 * check.
 *
 * On hit, EXTEND both the cell's runningObsSet and the RCA's
 * obsSetResponses with entries from `incoming` the cell didn't
 * already have. If the cell's `cachedApplyResult` is still alive
 * (weak_ptr resolves), return it — the caller skips `queryApply`.
 * Otherwise return the reused RCA so the caller re-runs `queryApply`
 * against it. On no compat cell, returns an empty ReuseHit.
 */
ReuseHit tryReuseLiveCallbackApplication(
    const std::map<TracingHash, std::string> & incoming,
    std::shared_ptr<ArgCell> startCell)
{
    std::shared_ptr<ArgCell> bestCell;
    std::shared_ptr<ReplayCallbackArg> bestRCA;
    size_t bestScore = 0;
    bool haveBest = false;

    int seen = 0, seenCallback = 0, seenRCA = 0, seenSubset = 0;

    /* Candidate cells: (1) the walk's own chain — startCell → parent →
       ... — for cells that themselves have callback state (rare,
       usually chains are regular scopes); (2) each chain cell's
       `liveCallbackChildren` — callback application cells created by
       prior SCA dispatches whose callerScope was on this chain. This
       is the main channel: chain cells are long-lived (anchored in
       outer's Object graph), and their children lists give reuse a
       stable index of past applications lifetime-bounded by the
       TracingReplayEvaluator's ring. */
    auto checkCandidate = [&](std::shared_ptr<ArgCell> cell) {
        auto cs = cell->getCallbackState();
        if (!cs) return;
        ++seenCallback;
        auto rca = std::dynamic_pointer_cast<ReplayCallbackArg>(cell->liveObject);
        if (!rca) return;
        ++seenRCA;
        auto score = reuseMatchScore(cs->runningObsSet, incoming);
        if (!score) return;
        ++seenSubset;
        if (!haveBest || *score > bestScore) {
            bestCell = cell;
            bestRCA = rca;
            bestScore = *score;
            haveBest = true;
        }
    };
    for (auto cell = startCell; cell; cell = cell->parent) {
        ++seen;
        checkCandidate(cell);
        /* Compact expired weak entries opportunistically while walking.
           Populate-via-`.back()` means every live callback application
           adds an entry; without compaction this grows with unique
           obsSets across the session. */
        auto & children = cell->liveCallbackChildren;
        children.erase(
            std::remove_if(children.begin(), children.end(),
                [](const std::weak_ptr<ArgCell> & w) { return w.expired(); }),
            children.end());
        for (auto & weakChild : children) {
            if (auto child = weakChild.lock())
                checkCandidate(child);
        }
    }

    if (!haveBest) {
        tracingCacheLog(
            "callbackApply: no reusable cell (chainLen=%d cellsWithCb=%d rcaOK=%d subset=%d)",
            seen, seenCallback, seenRCA, seenSubset);
        return {};
    }

    /* Extend cell's runningObsSet + RCA's obsSetResponses with entries
       `incoming` has that the cell doesn't. runningObsSet is a vector —
       build a reqHash set for O(N+M) dedup. Both must stay in sync
       (OuterApply::run maintains them together). */
    std::unordered_set<TracingHash> presentReqs;
    auto * cbState = bestCell->getCallbackState();
    for (auto & fact : cbState->runningObsSet)
        presentReqs.insert(fact.reqHash);
    auto obsMap = bestRCA->getObsSetResponses();
    size_t extended = 0;
    for (auto & [req, resp] : incoming) {
        if (presentReqs.count(req)) continue;
        /* getCallbackState() returns const *; the cell owns the state
           and reuse callers have write intent. const_cast is honest
           here — the field is logically mutable during the live
           callback application. */
        const_cast<CallbackState *>(cbState)->runningObsSet.push_back(
            TracingDecisionGraph::InlineFact{req, resp});
        if (obsMap)
            obsMap->emplace(req, resp);
        ++extended;
    }
    /* If the live application's applyResult is still alive, return it
       and skip queryApply. Otherwise return just the RCA + cell so
       the caller re-invokes queryApply and repopulates. */
    auto locked = cbState->cachedApplyResult.lock();
    tracingCacheLog(
        "callbackApply: REUSE cell subset=%zu extended=%zu cachedApplyResult=%s",
        bestScore, extended, locked ? "hit" : "expired-or-empty");
    return ReuseHit{
        .cachedApplyResult = locked,
        .reusedRCA = bestRCA,
    };
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

    TracingHash idHash = trace::tracingZeroHash();
    try {
        idHash = trace::parseTracingHex(idStr);
    } catch (const std::exception &) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
        return nullptr;
    }

    /* Producer chains reference Selectors by their own cachedHash, so
       the SelectorPool (Selectors table) is authoritative — the Requests
       pool holds OuterValueRequest envelopes now, not raw Selector
       payloads. Serialize the typed Selector back to JSON so downstream
       tag-branching code stays uniform. */
    auto selOpt = decisionGraph.selectorPool.find(idHash);
    if (!selOpt) {
        tracingCacheLog(
            "resolve %s: not in Selectors pool — no provenance; returning null",
            idStr.substr(0, 12));
        return nullptr;
    }
    nlohmann::json reqJson = trace::toJson(**selOpt);
    auto tag = reqJson["tag"].get<std::string>();
    /* Flat envelope: query fields live at top level of reqJson (no "params" wrapper). */
    auto & params = reqJson;

    if (tag == "apply") {
        tracingCacheLog("resolve %s: apply producer", idStr.substr(0, 12));
        return resolveApplyId(idStr, params, ctx);
    }

    if (tag == "callbackApply") {
        /* CBApply as a producer identity: use the already-resolved
           typed Selector from the pool (no JSON re-parse). Delegate
           to the shared helper; memoise the resulting Object under
           idStr so recursive resolveIdentity calls short-circuit. */
        tracingCacheLog("resolve %s: callbackApply producer", idStr.substr(0, 12));
        auto * cba = std::get_if<trace::SelectorCallbackApply>(&(*selOpt)->node);
        if (!cba)
            return nullptr;
        auto resultObj = dispatchLiveCallbackApplication(*cba, ctx);
        if (resultObj)
            ctx.memo[idStr] = resultObj;
        return resultObj;
    }

    auto qSel = trace::resolveFromJson(reqJson, decisionGraph.selectorPool);
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
    auto fnObj = resolveIdentity(params["parent"].get<std::string>(), ctx);
    if (!fnObj) {
        tracingCacheLog("replay: apply %s: cannot resolve fn %s", idStr, params["parent"]);
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
    } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
        tracingCacheLog("replay: apply %s: queryApply threw: %s", idStr, e.what());
        return nullptr;
    }
    if (resultObj)
        ctx.memo[idStr] = resultObj;
    return resultObj;
}


std::shared_ptr<Object> TracingReplayEvaluator::resolveProducerChild(
    const std::string & idStr, const trace::SelectorNode & qv, const nlohmann::json & params, ResolutionContext & ctx)
{
    /* Each step Selector's payload carries its parent's hex. Resolve
       the parent live and let the visit below dispatch by Selector kind. */
    if (!params.contains("parent"))
        return nullptr;
    auto parent = resolveIdentity(params["parent"].get<std::string>(), ctx);
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
            } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
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

std::shared_ptr<Object> TracingReplayEvaluator::dispatchLiveCallbackApplication(
    const trace::SelectorCallbackApply & sca,
    ResolutionContext & ctx)
{
    auto obsSet = decisionGraph.getObservationSet(sca.argObsSet);
    if (!obsSet) {
        tracingCacheLog(
            "callbackApply: obsSet=%s not in pool — miss",
            sca.argObsSet.toHex().substr(0, 12).c_str());
        return nullptr;
    }
    std::map<TracingHash, std::string> incomingObs;
    for (const auto & obs : *obsSet)
        incomingObs.emplace(obs.reqHash, obs.responsePayload);

    std::string fnHex = sca.parent->cachedHash.toHex();
    auto fnObj = resolveIdentity(fnHex, ctx);
    if (!fnObj) {
        tracingCacheLog(
            "callbackApply: fn=%s not resolvable",
            fnHex.substr(0, 12).c_str());
        return nullptr;
    }

    /* Look for a compatible live callback application in the current
       chain (see tryReuseLiveCallbackApplication). Three outcomes:
       - hit + cachedApplyResult alive → return it, skip queryApply.
       - hit but expired → queryApply against reused RCA (extended
         obsSet), then re-populate on the newly-created cell.
       - miss → construct fresh RCA + cell, queryApply, populate. */
    auto startCell = ctx.walkCell
                       ? ctx.walkCell
                       : (ctx.currentProxy ? ctx.currentProxy->getProxyArgCell() : nullptr);
    auto hit = tryReuseLiveCallbackApplication(incomingObs, startCell);
    if (hit.cachedApplyResult)
        return hit.cachedApplyResult;

    std::shared_ptr<Object> replayArg;
    if (hit.reusedRCA) {
        replayArg = hit.reusedRCA;
    } else {
        /* Contra-arg identity: SelectorArg{depth} matching writer's
           firingCell.depth = fn.argCell.depth + 1. */
        auto fnCell = fnObj->getProxyArgCell();
        if (!fnCell)
            panic("callbackApply: resolved fn has no argCell");
        int argDepth = fnCell->depth + 1;
        auto argProducerSel = decisionGraph.selectorPool.intern(trace::SelectorArg{argDepth});
        auto obsSetMap = std::make_shared<std::map<TracingHash, std::string>>(std::move(incomingObs));
        replayArg = std::make_shared<ReplayCallbackArg>(
            argProducerSel,
            decisionGraph, inner->getEvalState().rootFSRoot,
            &inner->getEvalState(),
            nullptr,
            obsSetMap);
    }
    try {
        auto resultObj = fnObj->queryApply(replayArg);
        if (!resultObj)
            return nullptr;
        /* Populate cachedApplyResult on the callback application cell
           OuterApply::run just created — it registered itself as the
           last entry in fnObj.argCell.liveCallbackChildren. This holds
           for both fresh and reuse-hit-expired paths: the reuse-expired
           path's queryApply also re-enters OuterApply::run with the
           reused RCA, creating and registering a new cell whose
           runningObsSet inherits the reused RCA's extended obsSet. */
        if (auto fnArgCell = fnObj->getProxyArgCell()) {
            if (!fnArgCell->liveCallbackChildren.empty()) {
                if (auto cell = fnArgCell->liveCallbackChildren.back().lock()) {
                    if (auto * cs = const_cast<CallbackState *>(cell->getCallbackState()))
                        cs->cachedApplyResult = resultObj;
                }
            }
        }
        /* Anchor lifetime in the recent-callback ring so the weak
           cachedApplyResult stays resolvable across walker calls for
           the last N firings. */
        anchorCallbackFiring(resultObj);
        return resultObj;
    } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw;
        tracingCacheLog("callbackApply: fn->queryApply threw: %s", e.what());
        return nullptr;
    }
}

std::optional<std::string> TracingReplayEvaluator::dispatchQueryRequest(const nlohmann::json & reqJson, ResolutionContext & ctx)
{
    auto qSelOpt = trace::resolveFromJson(reqJson, decisionGraph.selectorPool);
    if (!qSelOpt)
        return std::nullopt;
    auto & qv = (*qSelOpt)->node;

    auto resolveParent = [&](ref<const trace::Selector> parentSel) -> std::shared_ptr<Object> {
        return resolveIdentity(parentSel->cachedHash.toHex(), ctx);
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
                auto selfHex = selfHash.toHex();
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
                } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
                    tracingCacheLog("apply: computeWHNFFromObject threw: %s", e.what());
                    return std::nullopt;
                }
            } else if constexpr (std::is_same_v<Q, trace::SelectorCallbackApply>) {
                /* Task #110: dispatch via the shared helper — reuse-aware
                   materialisation of a live callback application. On
                   success, serialise the applyResult's WHNF for the
                   walker's Fact comparison; on any failure the walker
                   sees a null response and misses cleanly. */
                auto resultObj = dispatchLiveCallbackApplication(q, ctx);
                if (!resultObj)
                    return std::nullopt;
                try {
                    auto whnf = computeWHNFFromObject(*resultObj);
                    return jsonToCborString(nlohmann::json(whnf));
                } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw;
                    tracingCacheLog("callbackApply: applyResult WHNF failed: %s", e.what());
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
                auto selfHex = selfHash.toHex();
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
                } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
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
                } catch (const std::exception & e) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* rca-bail-diagnostic */
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
TracingReplayEvaluator::lookup(const Q & query, std::shared_ptr<Object> currentProxy, std::shared_ptr<ArgCell> cell)
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
            .queryHashStr = selectorHash.toHex(),
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
    auto rootCell = RegularArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorImport rootSel{displayPath};
    auto rootSelInterned = decisionGraph.selectorPool.intern(rootSel);
    std::optional<trace::ResultWHNF> cachedWHNF;
    TriePosition triePos{TracingDecisionGraph::emptySetHash(), rootSelInterned->cachedHash.toHex()};
    if (auto result = lookup(rootSel, nullptr, rootCell)) {
        tracingCacheLog("replay hit: evalFile %s", displayPath);
        try {
            auto whnfJson = cborStringToJson(result->first);
            trace::ResultWHNF parsed;
            from_json(whnfJson, parsed);
            cachedWHNF = std::move(parsed);
        } catch (const std::exception &) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* fall through */ }
        triePos = result->second;
    } else {
        tracingCacheLog("replay miss: evalFile %s (wrapping lazy inner in TRO)", displayPath);
    }
    /* Wrapping stack: always return a TRO. On hit, cachedWHNF is set;
       on miss, the lazy inner activates via ensureInner() when needed.
       This routes subsequent applies through TRO::queryApply's walker
       probe first — enabling replay's dispatch machinery to serve
       cross-apply repeats via its own OVR memoization mechanism
       (which needs a separate extension to actually memoize OVR). */
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, path, displayPath]() { return inner->evalFile(path, displayPath); }, rootCell,
        rootSelInterned, std::move(cachedWHNF), /*cbApplyOrigin=*/false, /*walkerMissed=*/!cachedWHNF.has_value());
    rootCell->liveObject = obj.get_ptr();
    return obj;
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    /* Phase F: create root cell before lookup; back-fill liveObject
       after wrapping. */
    auto rootCell = RegularArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorExpr rootSel{expr, basePath.path.abs()};
    auto rootSelInterned = decisionGraph.selectorPool.intern(rootSel);
    std::optional<trace::ResultWHNF> cachedWHNF;
    TriePosition triePos{TracingDecisionGraph::emptySetHash(), rootSelInterned->cachedHash.toHex()};
    if (auto result = lookup(rootSel, nullptr, rootCell)) {
        tracingCacheLog("replay hit: evalExpr");
        try {
            auto whnfJson = cborStringToJson(result->first);
            trace::ResultWHNF parsed;
            from_json(whnfJson, parsed);
            cachedWHNF = std::move(parsed);
        } catch (const std::exception &) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; /* fall through */ }
        triePos = result->second;
    } else {
        tracingCacheLog("replay miss: evalExpr (wrapping lazy inner in TRO)");
    }
    /* Wrapping stack: see evalFile for rationale. */
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); }, rootCell,
        rootSelInterned, std::move(cachedWHNF), /*cbApplyOrigin=*/false, /*walkerMissed=*/!cachedWHNF.has_value());
    rootCell->liveObject = obj.get_ptr();
    return obj;
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
    /* Frontend behavior: probe the walker for a recorded SelectorApply
       Terminal keyed on fn's Selector. If we find one, wrap the
       cached result in a TRO and return — this is what makes
       within-session second-invocation apply-calls hit the first
       invocation's recording without re-invoking the writer.

       On miss, delegate to fn's queryApply. TO's writer path
       records. TRO's walker path (from a warm evalFile hit) would
       also probe but this path handles the more common
       cold-writer-then-warm-consult sequence within one session. */
    auto fnSelOpt = fn->getSelector();
    if (fnSelOpt) {
        /* Need a cell to key the walker's per-walk state on. Wrap the
           arg if it's raw — writes into wrapArgAsCallbackScope's
           seedCell, which is what effectiveArgCell then reads. */
        std::shared_ptr<Object> probeArg = arg.get_ptr();
        if (!probeArg->getSelectorHashHex()) {
            probeArg = wrapArgAsCallbackScope(
                inner->getEvalState(), fn.get_ptr(), probeArg,
                inner, ref<OuterResolver>(inner->getOuterResolver())).get_ptr();
        }
        auto cell = effectiveArgCell(*probeArg);
        auto applySel = decisionGraph.selectorPool.intern(trace::SelectorApply{*fnSelOpt});
        auto & applySelector = std::get<trace::SelectorApply>(applySel->node);
        auto applyLookup = lookup(applySelector, fn.get_ptr(), cell);
        if (applyLookup) {
            std::optional<trace::ResultWHNF> cachedWHNF;
            try {
                auto whnfJson = cborStringToJson(applyLookup->first);
                trace::ResultWHNF parsed;
                from_json(whnfJson, parsed);
                cachedWHNF = std::move(parsed);
            } catch (const std::exception &) { extern thread_local bool rcaBailFlag; if (rcaBailFlag) throw; }
            if (cachedWHNF) {
                auto capturedFn = fn;
                auto capturedArg = probeArg;
                return make_ref<TracingReplayObject>(
                    *this, applyLookup->second,
                    [capturedFn, capturedArg]() {
                        auto & fnObj = *capturedFn;
                        auto r = fnObj.queryApply(capturedArg);
                        if (!r) panic("TRE::apply cache-hit fallback: queryApply returned null");
                        return ref<Object>(r);
                    },
                    std::move(cell),
                    applySel, std::move(cachedWHNF));
            }
        }
    }
    /* Miss (or fn had no Selector) — delegate to fn's queryApply for
       the writer/wrap path. */
    auto result = fn->queryApply(arg.get_ptr());
    if (!result)
        panic("TracingReplayEvaluator::apply: fn->queryApply returned null");
    return ref<Object>(result);
}

/* Explicit instantiations for public consumers of lookup() (e.g.
   TracingReplayObject::lookupStructuralChild). Add new Selector
   variants here as callers appear. */
template std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup<trace::SelectorGetAttr>(
    const trace::SelectorGetAttr &,
    std::shared_ptr<Object>,
    std::shared_ptr<ArgCell>);
template std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup<trace::SelectorGetListElem>(
    const trace::SelectorGetListElem &,
    std::shared_ptr<Object>,
    std::shared_ptr<ArgCell>);
template std::optional<std::pair<std::string, TriePosition>>
TracingReplayEvaluator::lookup<trace::SelectorApply>(
    const trace::SelectorApply &,
    std::shared_ptr<Object>,
    std::shared_ptr<ArgCell>);

} // namespace nix

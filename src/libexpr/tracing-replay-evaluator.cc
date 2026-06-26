#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/arg-scope.hh"
#include "nix/expr/eval.hh"
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

std::optional<std::pair<std::string, Hash>>
TracingReplayEvaluator::v13Walk(const Hash & queryHash, std::shared_ptr<Object> currentProxy)
{
    /* Per-walk resolution context. The cumulative cidasks walk
       (= `this->cidasksWalk`) lives on the evaluator so it
       persists across v13Walk calls — required for cell-chain
       cdi computation to land at the writer's `d1EdgeIndex` (=
       cumulative across logResults). */
    ResolutionContext ctx{
        std::move(currentProxy),
        {},
    };

    /* Per-edge buffer: dispatch() appends ambient facts here; the
       walk-loop promotes the buffer to a cumulative cidasksWalk
       edge on commit (via commitEdge) or discards it on reject.
       Without the buffer, rejected-edge facts would pollute
       cidasksWalk and throw off the cell-chain cdi computations. */
    std::vector<cidasks::Observation> pendingEdgeObservations;
    /* Apply Requests in this edge's dispatch — captured for
       materialisation at post-commit, when walker.cidasksWalk has
       grown to match writer.d1CidasksWalk at record time. See
       applyWrapperRegistry comment in the header. */
    std::vector<std::pair<Hash, nlohmann::json>> pendingApplyMaterialisations;

    auto commitEdge = [&]() {
        if (pendingEdgeObservations.empty()) return;
        /* Dedup by the edge's element-hash fingerprint (= XOR-fold
           of its fact element hashes) so re-traversing a shared
           Asks prefix in a later v13Walk doesn't double-append.
           XOR is a true set algebra here (Component F): same set
           of facts → same fingerprint regardless of order. */
        Hash fingerprint(HashAlgorithm::SHA256);
        for (const auto & f : pendingEdgeObservations)
            fingerprint = TracingDecisionGraph::xorFactIntoHash(
                fingerprint, f.fromHash, f.elementHash);
        if (committedEdgeFingerprints.insert(fingerprint).second) {
            cidasks::Edge edge;
            edge.observations = std::move(pendingEdgeObservations);
            cidasksWalk.push_back(std::move(edge));
            tracingCacheLog("dispatch: committed edge, cidasksWalk=%zu", cidasksWalk.size());
        } else {
            tracingCacheLog("dispatch: edge already in cidasksWalk (shared prefix), skip");
        }
        pendingEdgeObservations.clear();
        /* Materialise any apply-result wrappers for this edge's
           apply Requests. cidasksWalk has just grown by one, so
           evaluator.apply now sees the same walk index the writer
           saw after its markApplyBoundary for the same apply. The
           wrapper is registered in applyWrapperRegistry so later
           resolveCdiId / primop-impl invocations short-circuit
           to it. */
        for (auto & [reqHash, reqJson] : pendingApplyMaterialisations) {
            if (applyWrapperRegistry.count(reqHash))
                continue;
            try {
                auto & params = reqJson["params"];
                auto fnIdStr = params["fn"].get<std::string>();
                auto argIdStr = params["arg"].get<std::string>();
                auto fnObj = resolveCdiId(fnIdStr, ctx);
                auto argObj = resolveCdiId(argIdStr, ctx);
                if (!fnObj || !argObj) {
                    tracingCacheLog(
                        "materialise apply at ε: reqHash=%s — fn/arg resolution failed",
                        reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
                    continue;
                }
                /* Calling apply() here both constructs the wrapper at
                   the right cidasksWalk state AND registers it via
                   applyWrapperRegistry inside apply() itself. */
                auto wrapper = apply(ref<Object>(fnObj), ref<Object>(argObj));
                tracingCacheLog(
                    "materialise apply at ε: reqHash=%s registered",
                    reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
                /* Walk the recorded d2 chain anchored at this
                   boundary. Currently scaffolding only — logs
                   the recorded chain structure. When live
                   validation lands, a `false` return becomes a
                   d1 walker miss for this Q. */
                walkD2ChainAtBoundary(reqHash, wrapper.get_ptr(), ctx);
            } catch (const std::exception & e) {
                tracingCacheLog(
                    "materialise apply at ε: reqHash=%s threw: %s",
                    reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                    e.what());
            }
        }
        pendingApplyMaterialisations.clear();
    };

    auto discardEdge = [&]() {
        pendingEdgeObservations.clear();
        pendingApplyMaterialisations.clear();
    };

    /* Dispatcher: turns a Request hash into the current Response
       hash. Memoised in dispatchCache for stable requests (file
       reads, env vars) where same request always gives same
       response. Ambient queries are NOT memoised because the same
       request hash can dispatch to different responses depending on
       which proxy (cb invocation) the walk is grounded in — sibling
       cb apply invocations of the same fn share a request hash but
       must see their own arg's live value, not a memoised sibling's. */
    auto dispatch = [&](const Hash & requestHash) -> Hash {
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
        /* Apply-boundary marker: short-circuit before getCurrentResponse
           since dispatchAmbientQuery returns nullopt for tag="apply".
           The synthetic response is `Hash(0)` — symmetric with
           TracingWriter::markApplyBoundary. The pendingEdgeObservations
           push happens below so commitEdge fires for this Asks edge
           and walker.cidasksWalk grows. We also capture the apply
           Request payload so commitEdge can materialise the
           apply-result wrapper at the post-commit walk index — that
           moment matches writer.d1CidasksWalk at the corresponding
           markApplyBoundary, so applyCdi computed on both sides
           agrees. */
        if (isAmbient && queryTag == "apply") {
            auto applyRespHash = Hash(HashAlgorithm::SHA256);
            pendingEdgeObservations.push_back({
                Hash(HashAlgorithm::SHA256),
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, applyRespHash),
            });
            try {
                pendingApplyMaterialisations.push_back({requestHash, cborStringToJson(*requestPayload)});
            } catch (...) {
                /* Payload parse already succeeded once above when we
                   set queryTag. Defensive: if it suddenly fails here,
                   skip materialisation; the wrapper will fall back
                   to the late-construction path. */
            }
            tracingCacheLog(
                "dispatch apply boundary: req=%s payload=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription);
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
        if (isAmbient && ambientFromHash) {
            pendingEdgeObservations.push_back({
                *ambientFromHash,
                TracingDecisionGraph::xorFactIntoHash(
                    Hash(HashAlgorithm::SHA256), requestHash, h),
            });
            tracingCacheLog(
                "dispatch ambient: req=%s payload=%s from=%s resp=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                ambientFromHash->to_string(HashFormat::Base16, false).substr(0, 12),
                h.to_string(HashFormat::Base16, false).substr(0, 12));
        } else if (isAmbient) {
            tracingCacheLog(
                "dispatch ambient (no-from): req=%s payload=%s resp=%s",
                requestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                queryDescription,
                h.to_string(HashFormat::Base16, false).substr(0, 12));
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
        for (const auto & req : onlyInEdge) {
            auto resp = dispatch(req);
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
                    return std::make_pair(std::move(*payload), *term);
                }
            }
        }
        /* Fast-path didn't reach a terminal: drop the buffered facts;
           the full walk below starts fresh. */
        discardEdge();
    }

    /* Fall back to walk(). Two attempts in order:
       1. From `lastQFactsHash` — the cumulative dispatched
          position. This skips already-traversed shared prefix
          and resumes from there, crucial for sibling
          discrimination (cb-sibling): starting from ∅ would
          stop at the first reachable Terminal (= prior sibling),
          but starting from lastQFactsHash continues the chain
          past prior siblings' terminals to this Q's recorded
          position.
       2. From ∅ — the original behavior. Needed for Q's whose
          recorded chain doesn't extend from lastQFactsHash
          (= e.g., cb-385's deep-indep `b` fact, recorded at a
          cur that's a *prefix* of where lastQFactsHash sits). */
    auto walkHit = decisionGraph.walk(queryHash, dispatch,
        [&](bool committed, const std::vector<Hash> &) {
            if (committed) commitEdge();
            else discardEdge();
        },
        lastQFactsHash,
        dispatchedRequestSet);
    if (!walkHit) {
        walkHit = decisionGraph.walk(queryHash, dispatch,
            [&](bool committed, const std::vector<Hash> &) {
                if (committed) commitEdge();
                else discardEdge();
            });
    }
    if (!walkHit) {
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    auto payload = decisionGraph.getResultPayload(*walkHit);
    if (!payload) {
        tracingCacheStats().misses++;
        return std::nullopt;
    }
    tracingCacheStats().hits++;
    return std::make_pair(std::move(*payload), *walkHit);
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

    /* Walk the proxy's argScope chain looking for a cell whose
       liveObject's content id matches idStr. The id was stamped
       at some writer-side `d1CidasksWalk` index N at flush time,
       but the lookup carries only the cdi value — not the index.
       So try every edge boundary 0..cidasksWalk.size() against
       this subject's contentIdAt and accept the first match.
       cidasksWalk is cumulative across v13Walk calls (= mirror of
       writer's d1CidasksWalk), so the matching index always falls
       within range provided the walker has processed at least N
       prior Asks-edge commits — which it has by the time this
       lookup runs, since writer's flush K only stamps facts that
       reference cdis from flushes 0..K-1 (= already in walker's
       cidasksWalk by the time Q_K's dispatch reaches them). */
    auto cell = ctx.currentProxy ? ctx.currentProxy->getProxyArgScope() : nullptr;
    int cellDepth = 0;
    for (; cell; cell = cell->parent, ++cellDepth) {
        if (auto live = cell->liveObject) {
            if (auto * subj = live->getSubject()) {
                /* Use the live proxy's own inherited scope so the
                   walker's content id matches what the recorder
                   computed at this proxy at flush. */
                auto scope = live->getInheritedScope();
                bool matched = false;
                for (size_t k = 0; k <= cidasksWalk.size() && !matched; ++k) {
                    auto cdi = cidasks::contentIdAt(*subj, scope, cidasksWalk, k);
                    auto cdiHex = cdi.to_string(HashFormat::Base16, false);
                    if (cdiHex == idStr) {
                        tracingCacheLog(
                            "resolve %s: cell[%d] subject=%s MATCH at edge=%zu",
                            idStr.substr(0, 12), cellDepth,
                            cidasks::describe(*subj), k);
                        ctx.memo[idStr] = live;
                        return live;
                    }
                }
                tracingCacheLog(
                    "resolve %s: cell[%d] subject=%s miss across %zu edges",
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
        tracingCacheLog("resolve %s: not in pool -> materialise local standin", idStr.substr(0, 12));
        return materialiseLocalStandin(idHash, idStr, ctx);
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
std::shared_ptr<Object> TracingReplayEvaluator::materialiseLocalStandin(
    const Hash & idHash, const std::string & idStr, ResolutionContext & ctx)
{
    /* Pass the inner's EvalState so `defeatCache` can construct
       the depth-2 primop. Any live EvalState works for primop
       allocation; we pick the inner's since it's the one this
       evaluator already references. */
    auto standin = std::make_shared<ReplayLocalObject>(
        idHash, decisionGraph, inner->getEvalState().rootFSRoot, &inner->getEvalState());
    ctx.memo[idStr] = standin;
    return standin;
}

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
        /* The cb apply's local arg: opt this standin into depth-2
           per-probe validation (= each subsequent probe on it must
           appear in some recorded AmbientAsks edge's requestSet,
           or we throw divergence). Standins materialised by
           resolveCdiId for non-cb-apply ids stay without
           validation — their facts live at depth-1, not in
           AmbientAsks. */
        auto standin = materialiseLocalStandin(argHash, argIdStr, ctx);
        if (auto * replayLocal = dynamic_cast<ReplayLocalObject *>(standin.get()))
            replayLocal->withAmbientAsksValidation();
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

bool TracingReplayEvaluator::walkD2ChainAtBoundary(
    const Hash & applyRequestHash,
    std::shared_ptr<Object> applyResult,
    ResolutionContext & ctx)
{
    /* The writer's flushPendingAmbient persists each cb-apply's d2
       chain starting at the empty factSet (= cumulativeFactSet
       initialised to emptySetHash, then each fact appends one
       AmbientAsks edge). Walker starts the same way and follows
       the chain until either it reaches a factSet with no
       outgoing edges (= terminal) or a recorded edge whose live
       dispatch produces a response hash that doesn't match the
       transition (= divergence → stale). */
    auto cur = TracingDecisionGraph::emptySetHash();
    size_t step = 0;
    while (true) {
        auto outgoing = decisionGraph.getAmbientAsks(cur);
        if (outgoing.empty()) {
            tracingCacheLog(
                "d2 walk: reqHash=%s step=%zu terminal at cur=%s",
                applyRequestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                step,
                cur.to_string(HashFormat::Base16, false).substr(0, 12));
            return true;
        }
        /* Scaffolding stage: enumerate the chain so we can see the
           recorded structure. Live validation will land
           incrementally — for now, we just pick the first edge
           (= no branching expected since each cb-apply's d2 chain
           was recorded linearly) and advance to its toFactSet
           without dispatch. This means the d1 walker still trusts
           the recorded apply-result; the staleness gap stays open
           until validation lands. */
        auto [rsHash, toFactSet] = outgoing.front();
        auto rsMembers = decisionGraph.getRequestSet(rsHash);
        tracingCacheLog(
            "d2 walk: reqHash=%s step=%zu cur=%s -> toFactSet=%s rs-size=%zu",
            applyRequestHash.to_string(HashFormat::Base16, false).substr(0, 12),
            step,
            cur.to_string(HashFormat::Base16, false).substr(0, 12),
            toFactSet.to_string(HashFormat::Base16, false).substr(0, 12),
            rsMembers ? rsMembers->size() : 0);
        if (rsMembers) {
            for (auto & reqHash : *rsMembers) {
                auto payload = decisionGraph.getRequestPayload(reqHash);
                if (!payload) continue;
                try {
                    auto reqJson = cborStringToJson(*payload);
                    std::string desc = reqJson.value("query", std::string("(no-query-tag)"));
                    if (reqJson.contains("params") && reqJson["params"].is_object()) {
                        auto & params = reqJson["params"];
                        if (params.contains("name"))
                            desc += " name=\"" + params["name"].get<std::string>() + "\"";
                        if (params.contains("from"))
                            desc += " from=" + params["from"].get<std::string>().substr(0, 12);
                    }
                    tracingCacheLog(
                        "d2 walk:   probe req=%s payload=%s",
                        reqHash.to_string(HashFormat::Base16, false).substr(0, 12),
                        desc);
                } catch (...) {
                    tracingCacheLog(
                        "d2 walk:   probe req=%s (payload parse failed)",
                        reqHash.to_string(HashFormat::Base16, false).substr(0, 12));
                }
            }
        }
        cur = toFactSet;
        ++step;
        /* Hard cap on chain depth to guard against unforeseen cycles
           in the recorded structure. cb-apply d2 chains in practice
           are short (= a handful of probes per apply). */
        if (step > 64) {
            tracingCacheLog(
                "d2 walk: reqHash=%s aborting at step=%zu (depth cap)",
                applyRequestHash.to_string(HashFormat::Base16, false).substr(0, 12),
                step);
            return true;
        }
    }
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
        if (tag == "getType") {
            resultJson = trace::ResultType{objectTypeToString(obj->getType())};
        } else if (tag == "getAttr") {
            auto name = params["name"].get<std::string>();
            auto child = obj->maybeGetAttr(name);
            if (!child) {
                resultJson = trace::ResultMaybeType{std::nullopt};
            } else {
                resultJson = trace::ResultMaybeType{std::optional<std::string>{objectTypeToString(child->getType())}};
            }
        } else if (tag == "getString") {
            resultJson = trace::ResultString{obj->getStringIgnoreContext()};
        } else if (tag == "getStringWithContext") {
            auto [str, ctx] = obj->getStringWithContext();
            std::vector<std::string> ctxStrings;
            for (auto & c : ctx)
                ctxStrings.push_back(c.to_string());
            resultJson = trace::ResultStringWithContext{str, std::move(ctxStrings)};
        } else if (tag == "getAttrNames") {
            resultJson = trace::ResultListOfStrings{obj->getAttrNames()};
        } else if (tag == "getBool") {
            resultJson = trace::ResultBool{obj->getBool()};
        } else if (tag == "getInt") {
            resultJson = trace::ResultInt{obj->getInt().value};
        } else if (tag == "getFloat") {
            resultJson = trace::ResultFloat{obj->getFloat()};
        } else if (tag == "getListSize") {
            resultJson = trace::ResultListSize{obj->getListSize()};
        } else if (tag == "getListElem") {
            auto index = params["index"].get<size_t>();
            auto child = obj->getListElem(index);
            resultJson = trace::ResultType{objectTypeToString(child->getType())};
        } else if (tag == "getPath") {
            resultJson = trace::ResultPath{obj->getPath().path.abs()};
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
    const auto & [payload, resultHash] = *v13;
    tracingCacheLog("replay hit (v13 walk): %s", Q::tag);
    return std::make_pair(
        payload,
        TriePosition{
            .resultNodeHash = resultHash,
            .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
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
        if (auto hex = obj.getCdiHex())
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
       counterparts, or an opaque CDI). The wrapper is safe to
       register because the inner's behaviour is frozen at record
       time; identical Q lookups against the wrapper produce the
       same Terminal. */
    auto applyReqHash = TracingDecisionGraph::computeQueryHash(trace::QueryApply{fnId, argId});
    if (auto it = applyWrapperRegistry.find(applyReqHash); it != applyWrapperRegistry.end()) {
        tracingCacheLog(
            "walker apply: registry hit reqHash=%s",
            applyReqHash.to_string(HashFormat::Base16, false).substr(0, 12));
        return ref<Object>(it->second);
    }

    /* Build the ApplyResultSubject from fn/arg constituents — mirror
       of TracingEvaluator::apply. Fall back to OpaqueContent where no
       structural Subject is exposed. Scope comes from the arg's
       inheritedScope (= callScope, set on AmbientObject by the
       <cached-fn> PrimOp impl). */
    auto fnIdHash = Hash::parseNonSRIUnprefixed(fnId, HashAlgorithm::SHA256);
    auto argIdHash = Hash::parseNonSRIUnprefixed(argId, HashAlgorithm::SHA256);

    cidasks::Subject fnSubj;
    if (auto * fnAmb = dynamic_cast<AmbientObject *>(fn.get_ptr().get())) {
        if (auto * s = fnAmb->getSubject())
            fnSubj = *s;
        else
            fnSubj = cidasks::Subject{cidasks::OpaqueContentSubject{fnIdHash}};
    } else {
        fnSubj = cidasks::Subject{cidasks::OpaqueContentSubject{fnIdHash}};
    }

    cidasks::Subject argSubj;
    Hash applyScope(HashAlgorithm::SHA256);
    if (auto * argAmb = dynamic_cast<AmbientObject *>(arg.get_ptr().get())) {
        if (auto * s = argAmb->getSubject())
            argSubj = *s;
        else
            argSubj = cidasks::Subject{cidasks::OpaqueContentSubject{argIdHash}};
        applyScope = argAmb->getInheritedScope();
    } else {
        argSubj = cidasks::Subject{cidasks::OpaqueContentSubject{argIdHash}};
    }

    cidasks::Subject resultSubject{cidasks::ApplyResultSubject{
        .fn = std::make_shared<const cidasks::Subject>(std::move(fnSubj)),
        .arg = std::make_shared<const cidasks::Subject>(std::move(argSubj)),
    }};

    auto & walk = cidasksWalk;
    auto applyCdi = cidasks::contentIdAt(resultSubject, applyScope, walk, walk.size());
    auto applyCdiHex = applyCdi.to_string(HashFormat::Base16, false);
    {
        const auto & apr = std::get<cidasks::ApplyResultSubject>(resultSubject.data);
        tracingCacheLog(
            "walker apply: fn=%s arg=%s scope=%s walk=%zu -> applyCdi=%s",
            cidasks::describe(*apr.fn),
            cidasks::describe(*apr.arg),
            applyScope.to_string(HashFormat::Base16, false).substr(0, 12),
            walk.size(),
            applyCdiHex.substr(0, 16));
    }

    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = applyCdiHex,
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
    /* Register the freshly-constructed wrapper so subsequent
       evaluator.apply calls with the same (fn, arg) — including
       those triggered indirectly via resolveCdiId or primop impls
       running later in the dispatch chain — short-circuit to this
       wrapper rather than computing a fresh applyCdi against a
       later walker.cidasksWalk state. */
    applyWrapperRegistry[applyReqHash] = obj.get_ptr();
    return obj;
}

} // namespace nix

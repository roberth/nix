#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/arg-scope.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/replay-local-object.hh"
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
    /* Per-walk resolution context: holds the proxy whose method
       triggered this walk (for proxy-graph grounded ambient id
       resolution) and a memo of ids resolved during this walk. */
    ResolutionContext ctx{std::move(currentProxy), {}};

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
        try {
            auto reqJson = cborStringToJson(*requestPayload);
            isAmbient = reqJson.contains("query");
        } catch (...) {}
        if (!isAmbient) {
            if (auto it = dispatchCache.find(requestHash); it != dispatchCache.end())
                return it->second;
        }
        auto currentResp = getCurrentResponse(*requestPayload, ctx);
        if (!currentResp)
            return Hash(HashAlgorithm::SHA256);
        auto h = TracingDecisionGraph::computeResponseHash(*currentResp);
        if (!isAmbient)
            dispatchCache.emplace(requestHash, h);
        /* Dispatched facts are real environment observations; feed
           them into the writer's v13FactSet so any subsequent
           logResult records at the same factSetHash regardless of
           which facts came from interpretation vs cache-hit
           dispatch. */
        writer.noteEnvObservation(requestHash, h);
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
                    for (const auto & req : onlyInEdge)
                        dispatchedTrie.insert(req);
                    lastQFactsHash = candidateCur;
                    return std::make_pair(std::move(*payload), *term);
                }
            }
        }
    }

    /* Fall back to a regular walk from ∅. */
    auto walkHit = decisionGraph.walk(queryHash, dispatch);
    if (!walkHit)
        return std::nullopt;
    auto payload = decisionGraph.getResultPayload(*walkHit);
    if (!payload)
        return std::nullopt;
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
std::shared_ptr<Object> TracingReplayEvaluator::resolveAmbientId(const std::string & idStr, ResolutionContext & ctx)
{
    /* Per-walk memo. */
    if (auto it = ctx.memo.find(idStr); it != ctx.memo.end())
        return it->second;

    /* Cell-chain lookup: starting at currentProxy's argScope, walk
       the cell.parent chain looking for one whose contentId matches.
       State creep is folded into contentId() automatically (XOR-fold
       of ancestor cells' intrinsics). */
    auto cell = ctx.currentProxy ? ctx.currentProxy->getProxyArgScope() : nullptr;
    for (; cell; cell = cell->parent) {
        if (cell->contentId().to_string(HashFormat::Base16, false) == idStr) {
            ctx.memo[idStr] = cell->liveObject;
            return cell->liveObject;
        }
    }

    Hash idHash{HashAlgorithm::SHA256};
    try {
        idHash = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        return nullptr;
    }

    auto reqPayload = decisionGraph.getRequestPayload(idHash);
    if (!reqPayload) {
        /* Unknown id in the Requests pool — most commonly an
           inner-side TracingLocalObject's content-hash whose facts
           were emitted with from=hex(id) but whose id itself isn't
           a producer Request. Materialise a ReplayLocalObject keyed
           by it; its methods read recorded responses out of the
           Responses pool by qH(query{from=hex(id)}), matching what
           TracingLocalObject wrote during recording. */
        auto standin = std::make_shared<ReplayLocalObject>(
            idHash, decisionGraph, inner->getEvalState().rootFSRoot);
        ctx.memo[idStr] = standin;
        return standin;
    }

    nlohmann::json reqJson;
    try {
        reqJson = cborStringToJson(*reqPayload);
    } catch (const std::exception &) {
        return nullptr;
    }

    /* Local-arg sidecar (inserted by resolver.apply): chase to the
       apply and re-invoke it. The apply branch registers the live
       argObj under localId in ctx.memo, so subsequent dispatches of
       local-incoming Facts find it without re-chasing. */
    if (reqJson.contains("kind") && reqJson["kind"] == "localArg") {
        auto applyResultIdHex = reqJson["applyResultId"].get<std::string>();
        resolveAmbientId(applyResultIdHex, ctx);
        if (auto it = ctx.memo.find(idStr); it != ctx.memo.end())
            return it->second;
        return nullptr;
    }

    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    /* Apply-result ids ARE live-resolvable: invoke the apply
       against the resolved fn and a ReplayLocalObject for the
       (frozen) recorded local arg. */
    if (tag == "apply") {
        auto fnObj = resolveAmbientId(params["fn"].get<std::string>(), ctx);
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
        auto argReqPayload = decisionGraph.getRequestPayload(argHash);
        bool isLocalArg = false;
        if (argReqPayload) {
            try {
                auto j = cborStringToJson(*argReqPayload);
                if (j.contains("kind") && j["kind"] == "localArg")
                    isLocalArg = true;
            } catch (const std::exception &) {
                isLocalArg = true;
            }
        } else {
            isLocalArg = true;
        }
        if (isLocalArg)
            argObj = std::make_shared<ReplayLocalObject>(argHash, decisionGraph, inner->getEvalState().rootFSRoot);
        else
            argObj = resolveAmbientId(argIdStr, ctx);
        if (!argObj)
            return nullptr;
        ctx.memo[argIdStr] = argObj;
        auto * ambient = dynamic_cast<AmbientObject *>(fnObj.get());
        if (!ambient) {
            tracingCacheLog("replay: apply %s: fn resolved to non-AmbientObject", idStr);
            return nullptr;
        }
        std::shared_ptr<Object> resultObj;
        try {
            resultObj = ambient->queryApply(argObj);
        } catch (const std::exception & e) {
            tracingCacheLog("replay: apply %s: queryApply threw: %s", idStr, e.what());
            return nullptr;
        }
        if (resultObj)
            ctx.memo[idStr] = resultObj;
        return resultObj;
    }

    if (!params.contains("from"))
        return nullptr;

    auto parent = resolveAmbientId(params["from"].get<std::string>(), ctx);
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
       reads and env vars. resolveAmbientId for any tag (including
       apply, via live `queryApply` invocation) returns a live
       Object we can re-query. Serving recorded responses here would
       hide outer-side changes from the validation chain and let the
       cache return stale results. */
    auto obj = resolveAmbientId(params["from"].get<std::string>(), ctx);
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
        /* Top-level entry point: no parent in the proxy graph, no
           argScope (no apply has happened). */
        obj->withScope(nullptr);
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
        obj->withScope(nullptr);
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
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            if (auto qh = to->getQueryHashStr())
                return *qh;
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return ro->getTriePos().queryHashStr;
        if (auto * ao = dynamic_cast<AmbientObject *>(&obj))
            return ao->getId().to_string(HashFormat::Base16, false);
        if (auto * tlo = dynamic_cast<TracingLocalObject *>(&obj))
            return tlo->getId().to_string(HashFormat::Base16, false);
        throw Error(
            "TracingReplayEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    /* The recording side doesn't write a Q_apply Terminal -- a
       fresh app thunk has no result type. Synthesize the
       TriePosition from (fnId, argId) directly and always wrap the
       result in TracingReplayObject. Child queries on the apply
       result still walk independently; they fall through to inner
       only when their own walks miss.

       The previous pre-bind into a per-evaluator ambientState is
       gone — the live arg / fn live on the result proxy's argScope
       cell, and resolveAmbientId walks the proxy graph from
       whichever proxy is being forced. Per-call resolution naturally
       isolates concurrent cache invocations. */
    auto queryHash = TracingDecisionGraph::computeQueryHash(trace::QueryApply{fnId, argId});
    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel
        .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
    };
    auto obj = make_ref<TracingReplayObject>(
        *this, triePos, [this, fn, arg]() { return inner->apply(fn, arg); });
    /* Apply result: open a new intrinsic cell for this apply's
       argument. Cell parent = the fn proxy's argScope cell. */
    auto cell = ArgScopeCell::make(effectiveArgScope(*fn), arg.get_ptr());
    obj->withScope(std::move(cell));
    return obj;
}

} // namespace nix

#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/ambient-object.hh"
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
TracingReplayEvaluator::v13Walk(const Hash & queryHash)
{
    /* Dispatcher: turns a Request hash into the current Response
       hash, memoised in dispatchCache so the file read + CBOR
       encode + SHA-256 happens at most once per request per
       process. */
    auto dispatch = [&](const Hash & requestHash) -> Hash {
        if (auto it = dispatchCache.find(requestHash); it != dispatchCache.end())
            return it->second;
        auto requestPayload = decisionGraph.getRequestPayload(requestHash);
        if (!requestPayload)
            return Hash(HashAlgorithm::SHA256);
        auto currentResp = getCurrentResponse(*requestPayload);
        if (!currentResp)
            return Hash(HashAlgorithm::SHA256);
        auto h = TracingDecisionGraph::computeResponseHash(*currentResp);
        dispatchCache.emplace(requestHash, h);
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
                    /* Commit the side effects: the delta-add requests
                       are now part of our cumulative dispatched set;
                       cur has moved to candidateCur. */
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

std::optional<std::string> TracingReplayEvaluator::getCurrentResponse(const std::string & requestCbor)
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
        } else if (reqJson.contains("query") && ambientState) {
            return dispatchAmbientQuery(reqJson);
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to get current response: %s", e.what());
    }
    return std::nullopt;
}

/* Step C: resolve a recorded ambient id (hex of a Hash) to a live
   Object by recursive lookup against the Requests pool. Seed ids
   are pre-bound at apply() setup; derived ids find their producer
   Request in the pool, resolve the parent recursively, then
   dispatch the producer's query on the parent. */
std::shared_ptr<Object> TracingReplayEvaluator::resolveAmbientId(const std::string & idStr)
{
    if (!ambientState)
        return nullptr;

    auto it = ambientState->idToObject.find(idStr);
    if (it != ambientState->idToObject.end())
        return it->second;

    Hash idHash{HashAlgorithm::SHA256};
    try {
        idHash = Hash::parseNonSRIUnprefixed(idStr, HashAlgorithm::SHA256);
    } catch (const std::exception &) {
        return nullptr;
    }

    auto reqPayload = decisionGraph.getRequestPayload(idHash);
    if (!reqPayload) {
        tracingCacheLog("replay: ambient id %s has no producer Request in pool", idStr);
        return nullptr;
    }

    nlohmann::json reqJson;
    try {
        reqJson = cborStringToJson(*reqPayload);
    } catch (const std::exception &) {
        return nullptr;
    }
    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    /* QueryApply needs Step D's dispatcher (invoke the outer apply,
       register the result Object). Under Step C alone any
       apply-produced child id is unresolvable; downstream Facts
       that reference it will miss and the walk falls through. */
    if (tag == "apply")
        return nullptr;

    if (!params.contains("from"))
        return nullptr;

    auto parent = resolveAmbientId(params["from"].get<std::string>());
    if (!parent)
        return nullptr;

    std::shared_ptr<Object> child;
    try {
        if (tag == "getAttr") {
            child = parent->maybeGetAttr(params["name"].get<std::string>());
        } else if (tag == "getListElem") {
            child = parent->getListElem(params["index"].get<size_t>());
        } else {
            /* Non-child-producing Requests don't produce ids that
               downstream Facts could reference. */
            return nullptr;
        }
    } catch (const std::exception & e) {
        tracingCacheLog("replay: failed to resolve %s producer for %s: %s", tag, idStr, e.what());
        return nullptr;
    }

    if (child)
        ambientState->idToObject[idStr] = child;
    return child;
}

std::optional<std::string> TracingReplayEvaluator::dispatchAmbientQuery(const nlohmann::json & reqJson)
{
    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    /* QueryApply is Step D's responsibility; defer. */
    if (tag == "apply")
        return std::nullopt;

    if (!params.contains("from"))
        return std::nullopt;

    auto obj = resolveAmbientId(params["from"].get<std::string>());
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
TracingReplayEvaluator::lookup(const Q & query)
{
    auto queryHash = TracingDecisionGraph::computeQueryHash(query);
    auto v13 = v13Walk(queryHash);
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
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
    }
    tracingCacheLog("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    if (auto result = lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
        tracingCacheLog("replay hit: evalExpr");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
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
    auto getId = [](Object & obj) -> std::optional<std::string> {
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            return to->getQueryHashStr();
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return std::optional{ro->getTriePos().queryHashStr};
        return std::nullopt;
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);
    if (!fnId)
        fnId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(fn).value());
    if (!argId)
        argId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(arg).value());

    /* Step C: pre-bind ambient seed ids into idToObject. When this
       apply originates from the cached-fn PrimOp impl, fn/arg are
       AmbientObjects whose Hash id is the seed allocated by the
       resolver (registerOuterSeed). The recorded factSet's ambient
       Facts have `from=hex(seed_hash)`. Pre-binding lets
       resolveAmbientId find live Objects for those seeds without
       walking the producer chain. Derived ids fall through to the
       recursive resolution against the Requests pool. */
    AmbientReplayState state;
    if (auto * ambient = dynamic_cast<AmbientObject *>(arg.get_ptr().get()))
        state.idToObject[ambient->getId().to_string(HashFormat::Base16, false)] = arg.get_ptr();
    if (auto * ambient = dynamic_cast<AmbientObject *>(fn.get_ptr().get()))
        state.idToObject[ambient->getId().to_string(HashFormat::Base16, false)] = fn.get_ptr();
    ambientState = std::move(state);

    auto result = lookup(trace::QueryApply{*fnId, *argId});

    if (result) {
        tracingCacheLog("replay hit: apply");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, fn, arg]() { return inner->apply(fn, arg); });
    }
    tracingCacheLog("replay miss: apply");
    return inner->apply(fn, arg);
}

} // namespace nix

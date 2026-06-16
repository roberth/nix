#include "nix/expr/tracing-replay-evaluator.hh"
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
{
}

std::optional<std::pair<std::string, Hash>>
TracingReplayEvaluator::v13Walk(const Hash & queryHash)
{
    auto walkHit = decisionGraph.walk(queryHash, [&](const Hash & requestHash) -> Hash {
        auto requestPayload = decisionGraph.getRequestPayload(requestHash);
        if (!requestPayload)
            return Hash(HashAlgorithm::SHA256);
        auto currentResp = getCurrentResponse(*requestPayload);
        if (!currentResp)
            return Hash(HashAlgorithm::SHA256);
        return TracingDecisionGraph::computeResponseHash(*currentResp);
    });
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

std::optional<std::string> TracingReplayEvaluator::dispatchAmbientQuery(const nlohmann::json & reqJson)
{
    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    std::string fromId;
    if (params.contains("from"))
        fromId = params["from"].get<std::string>();
    else if (tag == "apply")
        return std::nullopt;
    else
        return std::nullopt;

    auto it = ambientState->idToObject.find(fromId);
    if (it == ambientState->idToObject.end()) {
        std::shared_ptr<Object> obj;
        if (!ambientState->pendingChildren.empty()) {
            obj = ambientState->pendingChildren.front();
            ambientState->pendingChildren.erase(ambientState->pendingChildren.begin());
        } else if (!ambientState->unresolvedRoots.empty()) {
            obj = ambientState->unresolvedRoots.front();
            ambientState->unresolvedRoots.erase(ambientState->unresolvedRoots.begin());
        } else {
            tracingCacheLog("replay: unknown ambient id %s", fromId);
            return std::nullopt;
        }
        ambientState->idToObject[fromId] = obj;
        it = ambientState->idToObject.find(fromId);
    }

    auto & obj = it->second;
    nlohmann::json resultJson;
    if (tag == "getType") {
        resultJson = trace::ResultType{objectTypeToString(obj->getType())};
    } else if (tag == "getAttr") {
        auto name = params["name"].get<std::string>();
        auto child = obj->maybeGetAttr(name);
        if (!child) {
            resultJson = trace::ResultMaybeType{std::nullopt};
        } else {
            ambientState->pendingChildren.push_back(child);
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
        ambientState->pendingChildren.push_back(child);
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

    AmbientReplayState state;
    if (!getId(*arg))
        state.unresolvedRoots.push_back(arg.get_ptr());
    if (!getId(*fn))
        state.unresolvedRoots.push_back(fn.get_ptr());
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

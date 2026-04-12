#include "nix/expr/tracing-replay-evaluator.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-index.hh"
#include "nix/expr/environment.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

#include <nlohmann/json.hpp>

namespace nix {

TracingReplayEvaluator::TracingReplayEvaluator(
    ref<Evaluator> inner,
    TracingIndex & tracingIndex,
    Environment & validationEnv,
    TracingWriter & writer)
    : inner(inner)
    , tracingIndex(tracingIndex)
    , writer(writer)
    , validationEnv(validationEnv)
{
}

bool TracingReplayEvaluator::validateDependencies(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    auto deps = tracingIndex.selectDependencies(queryNodeHash);
    // Multiple recordings may share the same query node prefix,
    // producing deps from different sessions. Group by request
    // and validate: at least one result per unique request must match.
    if (!validateDepsAnyMatch(deps))
        return false;

    validatedNodes.insert(queryNodeHash);
    return true;
}

bool TracingReplayEvaluator::validateToValidatedNode(const NodeHash & queryNodeHash)
{
    if (validatedNodes.count(queryNodeHash))
        return true;

    bool reachedValidated = false;
    auto deps = tracingIndex.selectDependenciesUntilValidated(queryNodeHash, validatedNodes, reachedValidated);

    if (!validateDeps(deps))
        return false;

    validatedNodes.insert(queryNodeHash);
    return true;
}

void TracingReplayEvaluator::markValidated(const NodeHash & nodeHash)
{
    validatedNodes.insert(nodeHash);
}

bool TracingReplayEvaluator::isValidated(const NodeHash & nodeHash) const
{
    return validatedNodes.count(nodeHash) > 0;
}

bool TracingReplayEvaluator::validateDepsAnyMatch(const std::vector<std::pair<QueryNode, ResultNode>> & deps)
{
    // Group deps by query payload. Multiple recordings from the same
    // trie prefix produce duplicate entries for the same file/env.
    // For each unique request, at least one result must validate.
    std::map<QueryHash, bool> requestValidated;

    for (const auto & [qNode, rNode] : deps) {
        if (validatedNodes.count(rNode.nodeHash))
            continue;

        auto it = requestValidated.find(qNode.queryHash);
        if (it != requestValidated.end() && it->second)
            continue; // already have a valid result for this request

        if (validateDeps({{qNode, rNode}})) {
            requestValidated[qNode.queryHash] = true;
        } else {
            if (requestValidated.find(qNode.queryHash) == requestValidated.end())
                requestValidated[qNode.queryHash] = false;
        }
    }

    for (auto & [req, valid] : requestValidated) {
        if (!valid)
            return false;
    }
    return true;
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

static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs: return "set";
    case nList: return "list";
    case nString: return "string";
    case nPath: return "path";
    case nInt: return "int";
    case nFloat: return "float";
    case nBool: return "bool";
    case nNull: return "null";
    case nFunction: return "lambda";
    case nThunk: return "thunk";
    case nExternal: return "external";
    case nFailed: return "failed";
    }
    return "unknown";
}

std::optional<std::string> TracingReplayEvaluator::dispatchAmbientQuery(const nlohmann::json & reqJson)
{
    auto tag = reqJson["query"].get<std::string>();
    auto & params = reqJson["params"];

    // Extract target Object from the id mapping
    std::string fromId;
    if (params.contains("from"))
        fromId = params["from"].get<std::string>();
    else if (tag == "apply")
        return std::nullopt; // Apply replay not yet implemented
    else
        return std::nullopt;

    auto it = ambientState->idToObject.find(fromId);
    if (it == ambientState->idToObject.end()) {
        // Lazy unification: map the unknown id to the next available Object.
        // First try pending children (from previous getAttr/getListElem),
        // then unresolved roots (the apply operands).
        std::shared_ptr<Object> obj;
        if (!ambientState->pendingChildren.empty()) {
            obj = ambientState->pendingChildren.front();
            ambientState->pendingChildren.erase(ambientState->pendingChildren.begin());
        } else if (!ambientState->unresolvedRoots.empty()) {
            obj = ambientState->unresolvedRoots.front();
            ambientState->unresolvedRoots.erase(ambientState->unresolvedRoots.begin());
        } else {
            tracingCacheLog("replay: unknown ambient id %s, no pending objects", fromId);
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
        tracingCacheLog("replay: unsupported ambient query tag: %s", tag);
        return std::nullopt;
    }

    return jsonToCborString(resultJson);
}

bool TracingReplayEvaluator::validateDeps(const std::vector<std::pair<QueryNode, ResultNode>> & deps)
{
    for (const auto & [qNode, rNode] : deps) {
        if (validatedNodes.count(rNode.nodeHash))
            continue;

        try {
            // Get the query payload (request) to know what to re-execute
            auto payloadOpt = tracingIndex.getQueryPayload(qNode.queryHash);
            if (!payloadOpt) {
                tracingCacheLog("replay: missing query payload for dependency");
                return false;
            }
            auto reqJson = cborStringToJson(*payloadOpt);

            // The result payload is the recorded response — compare as raw bytes
            if (reqJson.contains("absPath")) {
                std::string path = reqJson["absPath"];
                auto currentHash = validationEnv.getFileHash(path);

                nlohmann::json currentRespJson = trace::FileReadResponse{currentHash};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (rNode.payload != currentCbor) {
                    tracingCacheLog("replay invalidated: file %s changed", path);
                    return false;
                }
            } else if (reqJson.contains("name")) {
                std::string name = reqJson["name"];
                auto currentVal = validationEnv.getEnv(name);

                nlohmann::json currentRespJson = trace::GetEnvResponse{currentVal};
                auto currentCbor = jsonToCborString(currentRespJson);
                if (rNode.payload != currentCbor) {
                    tracingCacheLog("replay invalidated: env %s changed", name);
                    return false;
                }
            } else {
                // Unknown query type (e.g. ambient interaction) — can't
                // re-execute to validate. Conservative: invalidate.
                tracingCacheLog("replay invalidated: unvalidatable dependency");
                return false;
            }
        } catch (const std::exception & e) {
            tracingCacheLog("replay: failed to parse dependency: %s", e.what());
            return false;
        }
        validatedNodes.insert(rNode.nodeHash);
    }
    return true;
}

template<typename Q>
std::optional<std::pair<std::string, TriePosition>> TracingReplayEvaluator::lookup(const Q & query)
{
    auto queryHash = TracingIndex::computeQueryHash(query);
    auto shortcuts = tracingIndex.selectShortcuts(queryHash);

    for (const auto & shortcut : shortcuts) {
        if (!validateDependencies(shortcut.nodeHash))
            continue;

        auto queryNode = tracingIndex.getQuery(shortcut.nodeHash);
        if (!queryNode)
            continue;

        // Walk forward: Query → (depth>0 Query/Result)* → Result(depth=0)
        auto resultNode = tracingIndex.findResult(shortcut.nodeHash,
            [&](const std::string & queryPayload, const std::string & resultPayload) {
                auto currentResponse = getCurrentResponse(queryPayload);
                return currentResponse && resultPayload == *currentResponse;
            });

        if (!resultNode)
            continue;

        validatedNodes.insert(resultNode->nodeHash);
        temporalCursor = resultNode->nodeHash;
        tracingCacheLog("replay hit: %s", Q::tag);
        return std::make_pair(
            resultNode->payload,
            TriePosition{
                .resultNodeHash = resultNode->nodeHash,
                .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
            });
    }

    return std::nullopt;
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

ref<Object> TracingReplayEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    if (auto result = lookup(trace::QueryImport{displayPath})) {
        tracingCacheLog("replay hit: evalFile %s", displayPath);
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, path, displayPath]() { return inner->evalFile(path, displayPath); });
    }

    tracingCacheLog("replay miss: evalFile %s", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> TracingReplayEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    if (auto result = lookup(trace::QueryExpr{expr, basePath.path.abs()})) {
        tracingCacheLog("replay hit: evalExpr");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, expr, basePath]() { return inner->evalExpr(expr, basePath); });
    }

    tracingCacheLog("replay miss: evalExpr");
    return inner->evalExpr(expr, basePath);
}

ref<Object> TracingReplayEvaluator::evalExprLazy(const std::string & expr, const SourcePath & basePath)
{
    return inner->evalExprLazy(expr, basePath);
}

ref<Object> TracingReplayEvaluator::mkString(const std::string & s)
{
    return inner->mkString(s);
}

ref<Object> TracingReplayEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return inner->mkAttrs(attrs);
}

ref<Object> TracingReplayEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    // Try to get identity from TracingObject/TracingReplayObject
    auto getId = [](Object & obj) -> std::optional<std::string> {
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            return to->getQueryHashStr();
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return std::optional{ro->getTriePos().queryHashStr};
        return std::nullopt;
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    // Get or allocate virtual root ids. The registry on TracingWriter
    // ensures that on a miss, when the recording evaluator sees the
    // same Object, it gets the same id — no counter drift.
    if (!fnId)
        fnId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(fn).value());
    if (!argId)
        argId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(arg).value());

    // Set up ambient replay state for validating ambient interactions.
    // Objects without trie identity (virtual roots) are registered as
    // unresolved roots — lazily mapped to recorded ambient ids during
    // the walk.
    AmbientReplayState state;
    if (!getId(*arg))
        state.unresolvedRoots.push_back(arg.get_ptr());
    if (!getId(*fn))
        state.unresolvedRoots.push_back(fn.get_ptr());
    ambientState = std::move(state);

    auto result = lookup(trace::QueryApply{*fnId, *argId});
    // Don't clear ambientState — child queries on the result
    // TracingReplayObject may encounter ambient events that need
    // the same id→Object mapping. The state persists until the
    // next apply() call sets up a new one.

    if (result) {
        tracingCacheLog("replay hit: apply");
        return make_ref<TracingReplayObject>(
            *this, result->second, [this, fn, arg]() { return inner->apply(fn, arg); });
    }
    tracingCacheLog("replay miss: apply");

    return inner->apply(fn, arg);
}

} // namespace nix

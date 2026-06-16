#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <map>
#include <vector>

namespace nix {

class Environment;

/**
 * Evaluator that replays cached results from the v13 decision graph.
 * On cache miss, defers to the inner evaluator.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingDecisionGraph & decisionGraph;
    TracingWriter & writer;
    Environment & validationEnv;

    /**
     * Ambient replay state for resolving ambient interaction events
     * during apply() replay. Maps recorded ambient ids (the "from"
     * field in query payloads) to current Objects.
     */
    struct AmbientReplayState
    {
        std::map<std::string, std::shared_ptr<Object>> idToObject;
        std::vector<std::shared_ptr<Object>> unresolvedRoots;
        std::vector<std::shared_ptr<Object>> pendingChildren;
    };

    std::optional<AmbientReplayState> ambientState;

    std::optional<std::string> dispatchAmbientQuery(const nlohmann::json & reqJson);

    template<typename Q>
    std::optional<std::pair<std::string, TriePosition>> lookup(const Q & query);

public:
    TracingReplayEvaluator(
        ref<Evaluator> inner,
        Environment & validationEnv,
        TracingWriter & writer,
        TracingDecisionGraph & decisionGraph);

    /**
     * Compute the current response for a recorded request (file hash,
     * env var, or ambient interaction) by executing against the
     * current validation environment.
     */
    std::optional<std::string> getCurrentResponse(const std::string & requestCbor);

    /**
     * v13 walk lookup. Returns (resultPayload, resultHash) on hit,
     * nullopt on miss.
     */
    std::optional<std::pair<std::string, Hash>> v13Walk(const Hash & queryHash);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    EvalState & getEvalState() override;

    ref<Object> evalFile(const RootedPath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const RootedPath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkInt(NixInt i) override;
    ref<Object> mkBool(bool b) override;
    ref<Object> mkPath(const RootedPath & path) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> getInternalPrimOp(const std::string & name) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
};

} // namespace nix

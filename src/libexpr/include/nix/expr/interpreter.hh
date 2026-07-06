#pragma once
/**
 * @file
 * Interpreter implementation of the Evaluator interface.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/eval.hh"

namespace nix {

/**
 * Evaluator implementation that wraps EvalState.
 */
class Interpreter : public Evaluator
{
    ref<EvalState> evalState;

public:
    /// Shared resolver for ambient interactions (set by builtins.cache).
    std::shared_ptr<struct OuterResolver> ambientResolver;

    std::shared_ptr<struct OuterResolver> getAmbientResolver() override
    {
        return ambientResolver;
    }

    explicit Interpreter(ref<EvalState> evalState);

    bool isReadOnly() const override;

    Store & getStore() override;

    const fetchers::Settings & getFetchSettings() override;

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

    EvalState & getEvalState() override;
};

} // namespace nix
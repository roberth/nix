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
    explicit Interpreter(ref<EvalState> evalState);

    bool isReadOnly() const override;

    Store & getStore() override;

    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;

    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;

    ref<Object> mkString(const std::string & s) override;

    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
};

} // namespace nix
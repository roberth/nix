#pragma once
/**
 * @file
 * ExprProxy - base class for pseudo-Exprs that delegate to external sources.
 * ExprFromObject - Expr that evaluates by pulling from an Object.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/nixexpr.hh"

#include <memory>

namespace nix {

/**
 * Base class for pseudo-Exprs that delegate evaluation to external sources.
 *
 * Provides default implementations for show() and bindVars() since these
 * proxy expressions don't participate in normal parsing/binding.
 */
struct ExprProxy : Expr, gc
{
    void show(const SymbolTable & symbols, std::ostream & str) const override
    {
        str << "<proxy>";
    }

    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override
    {
        // No variables to bind - we pull from external sources, not the environment
    }
};

/**
 * An Expr that wraps an Object and produces a Value when evaluated.
 *
 * This enables using results from any Evaluator (including replay evaluators)
 * within normal Nix evaluation. The Object is queried lazily - for attrsets,
 * child attributes become thunks that wrap child Objects.
 *
 * No Value objects are shared between evaluators - fresh Values are constructed
 * by querying the Object interface.
 */
struct ExprFromObject : ExprProxy
{
    std::shared_ptr<Object> obj;

    explicit ExprFromObject(std::shared_ptr<Object> obj)
        : obj(std::move(obj))
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
};

} // namespace nix

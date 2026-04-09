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

    /**
     * Optional inner Evaluator for function call support.
     * When set and the Object is a function, the PrimOp created by
     * eval() captures this to route applications back through the
     * inner evaluator via Evaluator::apply().
     */
    std::shared_ptr<Evaluator> innerEvaluator;

    explicit ExprFromObject(std::shared_ptr<Object> obj, std::shared_ptr<Evaluator> innerEvaluator = nullptr)
        : obj(std::move(obj))
        , innerEvaluator(std::move(innerEvaluator))
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
};

/**
 * Lazy attribute thunk: defers maybeGetAttr until forced.
 */
struct ExprFromObjectAttr : ExprProxy
{
    std::shared_ptr<Object> parentObj;
    std::string name;
    std::shared_ptr<Evaluator> innerEvaluator;

    ExprFromObjectAttr(
        std::shared_ptr<Object> parentObj, std::string name, std::shared_ptr<Evaluator> innerEvaluator)
        : parentObj(std::move(parentObj))
        , name(std::move(name))
        , innerEvaluator(std::move(innerEvaluator))
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
};

} // namespace nix

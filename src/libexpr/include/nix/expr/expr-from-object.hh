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
    void show(const SymbolTable & symbols, std::ostream & str) const override;
    void bindVars(EvalState & es, const std::shared_ptr<const StaticEnv> & env) override;
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
     * Inner evaluator for function call routing.
     *
     * When set, the Object is a function defined inside the cache
     * boundary. eval() creates a `<cached-fn>` PrimOp that routes
     * calls through this evaluator. ambientResolver MUST also be set.
     *
     * When null, functions are either absent or ambient (from the
     * outer evaluator). Ambient functions get an `<ambient-fn>`
     * PrimOp that dispatches via AmbientObject::queryApply().
     */
    std::shared_ptr<Evaluator> innerEvaluator;

    /**
     * Bidirectional bridge between outer EvalState and inner evaluator.
     * Propagated to child ExprFromObjects so nested function results
     * can dispatch calls regardless of which PrimOp variant created them.
     *
     * Created via makeAmbientResolver(outerState, innerEvaluator).
     */
    std::shared_ptr<struct AmbientResolver> ambientResolver;

    explicit ExprFromObject(
        std::shared_ptr<Object> obj,
        std::shared_ptr<Evaluator> innerEvaluator = nullptr,
        std::shared_ptr<AmbientResolver> ambientResolver = nullptr)
        : obj(std::move(obj))
        , innerEvaluator(std::move(innerEvaluator))
        , ambientResolver(std::move(ambientResolver))
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
    std::shared_ptr<struct AmbientResolver> ambientResolver;

    ExprFromObjectAttr(
        std::shared_ptr<Object> parentObj,
        std::string name,
        std::shared_ptr<Evaluator> innerEvaluator,
        std::shared_ptr<AmbientResolver> ambientResolver = nullptr)
        : parentObj(std::move(parentObj))
        , name(std::move(name))
        , innerEvaluator(std::move(innerEvaluator))
        , ambientResolver(std::move(ambientResolver))
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
};

/**
 * Create a shared AmbientResolver for use with ExprFromObject.
 * The resolver is shared across all function calls within a single
 * builtins.cache invocation.
 */
std::shared_ptr<AmbientResolver> makeAmbientResolver(EvalState * outerState, std::shared_ptr<Evaluator> innerEvaluator);

} // namespace nix

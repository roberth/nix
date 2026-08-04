#pragma once
/**
 * @file
 * ExprProxy - base class for pseudo-Exprs that delegate to external sources.
 * ExprFromObject - Expr that evaluates by pulling from an Object.
 */

#include "nix/expr/observation-set.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/nixexpr.hh"
#include "nix/expr/trace-types.hh"

#include <memory>
#include <vector>

namespace nix {

/**
 * Dispatch a Selector as a query on `obj` and return the (WHNF, child)
 * pair. Same routine `OuterObject`'s queryFn uses internally to execute
 * a probe on the wrapped outer Object. Exported so the higher-order
 * callback primop can reuse it for arg-obsSet accumulation.
 */
OuterQueryResult dispatchOuterQuery(std::shared_ptr<Object> obj, const trace::SelectorNode & q);

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
     * calls through this evaluator. outerResolver MUST also be set.
     *
     * When null, functions are either absent or outer-owned (from
     * the outer evaluator). Outer functions get an `<outer-fn>`
     * PrimOp that dispatches via OuterObject::queryApply().
     */
    std::shared_ptr<Evaluator> innerEvaluator;

    /**
     * Bidirectional bridge between outer EvalState and inner evaluator.
     * Propagated to child ExprFromObjects so nested function results
     * can dispatch calls regardless of which PrimOp variant created them.
     *
     * Created via makeOuterResolver(outerState, innerEvaluator).
     */
    std::shared_ptr<struct OuterResolver> outerResolver;

    explicit ExprFromObject(
        std::shared_ptr<Object> obj,
        std::shared_ptr<Evaluator> innerEvaluator = nullptr,
        std::shared_ptr<OuterResolver> outerResolver = nullptr)
        : obj(std::move(obj))
        , innerEvaluator(std::move(innerEvaluator))
        , outerResolver(std::move(outerResolver))
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
    std::shared_ptr<struct OuterResolver> outerResolver;

    ExprFromObjectAttr(
        std::shared_ptr<Object> parentObj,
        std::string name,
        std::shared_ptr<Evaluator> innerEvaluator,
        std::shared_ptr<OuterResolver> outerResolver = nullptr)
        : parentObj(std::move(parentObj))
        , name(std::move(name))
        , innerEvaluator(std::move(innerEvaluator))
        , outerResolver(std::move(outerResolver))
    {
    }

    void eval(EvalState & state, Env & env, Value & v) override;
};

/**
 * Create a shared OuterResolver for use with ExprFromObject.
 * The resolver is shared across all function calls within a single
 * builtins.cache invocation.
 *
 * @param innerWriter When non-null, enables Step E's incoming-Fact
 *   recording via TracingCallbackArg during covariant callbacks.
 *   Callers without a writer (or who don't need replay hits on the
 *   apply) can pass nullptr; resolver.apply then skips the wrap and
 *   bridges argObj directly.
 */
class TracingWriter;
std::shared_ptr<OuterResolver> makeOuterResolver(
    EvalState * outerState,
    std::shared_ptr<Evaluator> innerEvaluator,
    TracingWriter * innerWriter = nullptr);

/** PrimOp wrapping a cache-boundary function so apply routes through
    `innerEval->apply` after opening a cached-fn cell chain. Used by
    `TObject::maybeMaterialiseAsFunctionValue` and its replay-side
    counterpart. */
PrimOp * makeCachedFnPrimOp(
    std::shared_ptr<Object> fnObj,
    std::shared_ptr<Evaluator> innerEval,
    std::shared_ptr<OuterResolver> resolver);

/** PrimOp wrapping an outer-side function so apply dispatches through
    `fnObj->queryApply`. Used by
    `OuterObject::maybeMaterialiseAsFunctionValue` and as the
    generic fallback in `ExprFromObject::eval`'s nFunction case. */
PrimOp * makeOuterFnPrimOp(
    std::shared_ptr<Object> fnObj,
    std::shared_ptr<OuterResolver> resolver);

} // namespace nix

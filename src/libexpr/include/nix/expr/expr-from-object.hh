#pragma once
/**
 * @file
 * ExprProxy - base class for pseudo-Exprs that delegate to external sources.
 * ExprFromObject - Expr that evaluates by pulling from an Object.
 */

#include "nix/expr/subject-id.hh"
#include "nix/expr/evaluator.hh"
#include "nix/expr/nixexpr.hh"

#include <memory>
#include <vector>

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
 *
 * @param innerWriter When non-null, enables Step E's incoming-Fact
 *   recording via TracingCallbackArg during covariant callbacks.
 *   Callers without a writer (or who don't need replay hits on the
 *   apply) can pass nullptr; resolver.apply then skips the wrap and
 *   bridges argObj directly.
 */
class TracingWriter;
std::shared_ptr<AmbientResolver> makeAmbientResolver(
    EvalState * outerState,
    std::shared_ptr<Evaluator> innerEvaluator,
    TracingWriter * innerWriter = nullptr);

/** Set the resolver's cached-call argAncestry — used by subject-id to make
    sibling cached calls' state hashes distinct via inheritance.
    Should be unique per cached call (e.g. hash of import path). */
void setAmbientResolverCallScope(AmbientResolver & resolver, Hash callArgAncestry);

/** Get the resolver's current callArgAncestry for RAII save/restore around
    per-cb-invocation argAncestry overrides. */
Hash getAmbientResolverCallScope(const AmbientResolver & resolver);

/** Register a live outer-direction proxy under a subject-id `subject` +
    `argAncestry` in the resolver's outer-values map. Used by the
    `<replay-local-lambda>` primop at warm replay to publish the
    live arg it received (args[0]) under the cb-arg seed's
    structural identity, so the OUTER walker can resolve env facts
    whose `from` references the seed's state hash — at ANY walk-edge
    index, since the env fact's `from` is the seed's subject-id-evolved
    state hash at flush time and the walker doesn't know that index a
    priori. At cold these queries' answers came from the queryFn
    closure that captured the live outer arg; at warm this
    registration is the equivalent live channel. Single-entry
    contract (= overwrite-on-conflict) keyed by `(subject, argAncestry)`
    structural-equality. */
void registerAmbientResolverProxy(
    AmbientResolver & resolver,
    Subject subject,
    Hash argAncestry,
    std::shared_ptr<Object> obj);

/** Try to resolve a registered live-proxy from the resolver by
    matching its registered `(subject, argAncestry)` against the given
    `idHash` at any edge boundary of `envWalk`. Returns nullptr
    if no registration matches at any edge. Used by
    `TracingReplayEvaluator::resolveStateHash` as a fallback after
    cell-chain and Requests-pool resolution fail, before the
    "outer-seed by elimination" miss path. Iterating every edge
    boundary is necessary because the env fact's `from` is the
    seed's state hash at the writer's flush-time `envWalk` index
    (= post-observations evolution), which differs from the
    initial state hash we registered under. */
std::shared_ptr<Object> tryResolveAmbientResolverProxy(
    AmbientResolver & resolver,
    const Hash & idHash,
    const std::vector<Edge> & envWalk,
    TracingDecisionGraph * dg = nullptr);

} // namespace nix

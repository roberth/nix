#pragma once
/**
 * @file
 * CoarseEvalCache - Evaluator implementation using the coarse-grained eval cache.
 */

#include "nix/expr/evaluator.hh"
#include "nix/util/ref.hh"

namespace nix {

namespace eval_cache {
class EvalCache;
}

/**
 * Evaluator implementation that uses a coarse-grained cache key to remember
 * a Nix value.
 *
 * `CoarseEvalCache` is typically used with a flake lock as its cache key, and
 * the flake outputs of the root flake as the cached value.
 *
 * Since the Evaluator interface serves as a general entrypoint, but coarse
 * caching requires custom setup and circumstances, most operations that
 * implement Evaluator here just delegate to the inner Evaluator; the same
 * that evaluates for cache misses. The inner Evaluator may itself be a
 * tracing stack (TracingReplayEvaluator → TracingEvaluator → Interpreter)
 * when `tracing-eval-cache` is enabled — wrapping rather than replacing,
 * so both the coarse disk cache and the per-query trie cache compose.
 */
// Implementation note: EvalState is aware of coarse eval caches, so little
// bookkeeping is needed here. If we want to keep the coarse cache, we could
// consider moving some of its implementation into this class.
class CoarseEvalCache : public Evaluator
{
    ref<Evaluator> inner;

public:
    explicit CoarseEvalCache(ref<Evaluator> inner);

    /**
     * Get the root Object from an EvalCache.
     * This creates an Object interface for navigating the evaluation results,
     * which may trigger evaluation if the values aren't already cached.
     */
    ref<Object> getRoot(ref<eval_cache::EvalCache> evalCache);

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
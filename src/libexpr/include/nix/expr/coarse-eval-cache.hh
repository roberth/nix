#pragma once
/**
 * @file
 * CoarseEvalCache - Evaluator implementation using the coarse-grained eval cache.
 */

#include "nix/expr/evaluator.hh"
#include "nix/expr/interpreter.hh"
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
 * Most operations delegate to an inner Interpreter. The unique capability
 * is getRoot(), which returns cached cursor objects.
 *
 * Implementation note: EvalState is aware of coarse eval caches, so little
 * bookkeeping is needed here. If we want to keep the coarse cache, we could
 * consider moving some of its implementation into this class.
 */
class CoarseEvalCache : public Evaluator
{
    ref<Interpreter> inner;

public:
    explicit CoarseEvalCache(ref<Interpreter> inner);

    /**
     * Get the root Object from an EvalCache.
     * This creates an Object interface for navigating the evaluation results,
     * which may trigger evaluation if the values aren't already cached.
     */
    ref<Object> getRoot(ref<eval_cache::EvalCache> evalCache);

    bool isReadOnly() const override;

    Store & getStore() override;

    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;

    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
};

} // namespace nix
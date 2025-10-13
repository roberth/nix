#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/coarse-eval-cache-cursor-object.hh"
#include "nix/expr/eval-cache.hh"

namespace nix {

CoarseEvalCache::CoarseEvalCache(ref<Interpreter> inner)
    : inner(inner)
{
}

ref<Object> CoarseEvalCache::getRoot(ref<eval_cache::EvalCache> evalCache)
{
    auto cursor = evalCache->getRoot();
    return make_ref<CoarseEvalCacheCursorObject>(cursor);
}

bool CoarseEvalCache::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & CoarseEvalCache::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & CoarseEvalCache::getFetchSettings()
{
    return inner->getFetchSettings();
}

} // namespace nix

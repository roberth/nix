#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/coarse-eval-cache-cursor-object.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/expr/environment/system.hh"

namespace nix {

CoarseEvalCache::CoarseEvalCache(ref<EvalState> state)
    : state(state)
{
}

ref<Object> CoarseEvalCache::getRoot(ref<eval_cache::EvalCache> evalCache)
{
    auto cursor = evalCache->getRoot();
    return make_ref<CoarseEvalCacheCursorObject>(cursor);
}

bool CoarseEvalCache::isReadOnly() const
{
    return state->settings.readOnlyMode;
}

Store & CoarseEvalCache::getStore()
{
    return *state->systemEnvironment->store;
}

const fetchers::Settings & CoarseEvalCache::getFetchSettings()
{
    return state->fetchSettings;
}

ref<Object> CoarseEvalCache::evalFile(const SourcePath & path, const std::string & displayPath)
{
    throw Error("CoarseEvalCache::evalFile is not supported; use getRoot() for flakes");
}

ref<Object> CoarseEvalCache::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    throw Error("CoarseEvalCache::evalExpr is not supported; use getRoot() for flakes");
}

} // namespace nix
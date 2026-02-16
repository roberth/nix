#include "nix/expr/coarse-eval-cache.hh"
#include "nix/expr/coarse-eval-cache-cursor-object.hh"
#include "nix/expr/eval-cache.hh"
#include "nix/util/logging.hh"

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

// CoarseEvalCache only supports initialization through flakes (via getRoot).
// For evalFile/evalExpr, fall back to direct evaluation without caching.

ref<Object> CoarseEvalCache::evalFile(const SourcePath & path, const std::string & displayPath)
{
    debug("CoarseEvalCache::evalFile falling back to direct evaluation for '%s'", displayPath);
    return inner->evalFile(path, displayPath);
}

ref<Object> CoarseEvalCache::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    debug("CoarseEvalCache::evalExpr falling back to direct evaluation");
    return inner->evalExpr(expr, basePath);
}

ref<Object> CoarseEvalCache::mkString(const std::string & s)
{
    return inner->mkString(s);
}

ref<Object> CoarseEvalCache::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return inner->mkAttrs(attrs);
}

ref<Object> CoarseEvalCache::apply(ref<Object> fn, ref<Object> arg)
{
    return inner->apply(fn, arg);
}

} // namespace nix
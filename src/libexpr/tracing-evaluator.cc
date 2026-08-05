#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
#include "nix/expr/tracing-callback-arg.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-replay-object.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/expr/trace-file.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/environment.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/sync.hh"
#include "nix/expr/tracing-cache-log.hh"
#include "nix/util/hash.hh"
#include "nix/util/logging.hh"
#include "nix/expr/object-type.hh"

namespace nix {

TracingEvaluator::TracingEvaluator(TracingWriter & writer, ref<Evaluator> inner, TracingDatabase * db)
    : writer(writer)
    , inner(inner)
    , db(db)
{
}

void TracingEvaluator::ensurePreloaded()
{
    if (preloaded)
        return;
    preloaded = true;

    if (!db)
        return;

    auto & evalState = inner->getEvalState();

    auto latestTrace = db->latestTraceFile();
    if (!latestTrace)
        return;

    auto filePaths = db->getTracedFilePaths(*latestTrace);
    if (filePaths.empty())
        return;

    // Get the tracing source accessor from the environment
    auto accessor = evalState.environment->fsRoot();
    auto tracingAccessor = dynamic_cast<TracingSourceAccessor *>(&*accessor);
    if (!tracingAccessor)
        return;

    // Read files in parallel (I/O bound)
    struct PreloadedFile
    {
        CanonPath path;
        SpeculativeReadResult result;
    };

    Sync<std::vector<PreloadedFile>> preloadedFiles;

    ThreadPool pool;
    for (const auto & pathStr : filePaths) {
        pool.enqueue([&, pathStr]() {
            try {
                auto canonPath = CanonPath(pathStr);
                auto result = tracingAccessor->readSpeculatively(canonPath);
                preloadedFiles.lock()->push_back(
                    PreloadedFile{
                        .path = std::move(canonPath),
                        .result = std::move(result),
                    });
            } catch (...) {
                // Ignore read errors during preload
            }
        });
    }

    try {
        pool.process();
    } catch (...) {
        // Ignore pool errors during preload
    }

    // Parse sequentially (EvalState parsing is not thread-safe)
    for (auto & file : *preloadedFiles.lock()) {
        try {
            auto sourcePath = SourcePath{accessor, file.path};
            /* lazy-paths: parseExprFromString takes RootedPath. Preloaded
               .nix files come through the (system) rootFS accessor, so
               root them at `rootFSRoot`. */
            auto basePath = RootedPath{evalState.rootFSRoot, sourcePath.parent().path};
            auto expr = evalState.parseExprFromString(std::move(file.result.contents), basePath);
            evalState.insertPreloadedParsedFile(sourcePath, expr, std::move(file.result.emitTrace));
        } catch (...) {
            // Ignore parse errors during preload
        }
    }
}

bool TracingEvaluator::isReadOnly() const
{
    return inner->isReadOnly();
}

Store & TracingEvaluator::getStore()
{
    return inner->getStore();
}

const fetchers::Settings & TracingEvaluator::getFetchSettings()
{
    return inner->getFetchSettings();
}

EvalState & TracingEvaluator::getEvalState()
{
    return inner->getEvalState();
}

namespace {

/** `TracingEvaluator` is constructed only by `prim_cache`, so every
    instance is by definition the recording layer of a `builtins.cache`
    boundary. Reaching one of its root-value methods means the cache
    layer is about to construct (or has just constructed via a replay
    miss → delegation) a fresh root value — either as a top-level
    `evalFile`/`evalExpr` miss in the replay evaluator above, or as a
    `TracingReplayObject::ensureInner` escape that targets this
    evaluator directly via its `getInner` callback.

    `_NIX_DISALLOW_CACHE_INTERPRET_INNER=1` asserts that neither path
    is allowed — the cache replay must serve every recorded value's
    behaviour from the cache alone. */
void guardCacheRecording(std::string_view method, std::string_view target)
{
    static const bool disallow =
        getEnv("_NIX_DISALLOW_CACHE_INTERPRET_INNER").value_or("") == "1";
    if (!disallow)
        return;
    throw Error(
        "tracing eval cache: the recording layer of a `builtins.cache` "
        "boundary was asked to %s `%s` (_NIX_DISALLOW_CACHE_INTERPRET_INNER=1). "
        "A recorded value's behaviour could not be served from the cache "
        "alone — the recording is either keyed at a query the walker "
        "cannot reach, or absent entirely.",
        method, target);
}

} // namespace

ref<Object> TracingEvaluator::evalFile(const RootedPath & path, const std::string & displayPath)
{
    guardCacheRecording("evalFile", displayPath);
    ensurePreloaded();
    tracingCacheLog("tracing: evalFile %s", displayPath);
    /* Cell-migration Phase C: root cell owns SelectorImport's qState.
       #177: parent = sessionRootCell so env facts (folded there) are
       visible via factSetHash() on this cell and its descendants. */
    auto rootCell = RegularArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorImport rootSel{displayPath};
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, rootSel);
    auto result = inner->evalFile(path, displayPath);
    auto whnf = computeWHNFFromObject(*result, inner->getEvalState());
    auto triePos = writer.logResult(v, whnf, qh, rootCell);
    /* Bootstrap the SelectorPool: intern this evalFile's root Selector
       so descendants that build SelectorApplyStep{parent=this} etc.
       can resolve their parent via fromVariant. */
    auto obj = TracingObject::create(result, writer, v, triePos, rootCell,
        inner, ref<OuterResolver>(inner->getOuterResolver()),
        writer.getDecisionGraph().selectorPool.intern(rootSel),
        std::move(whnf));
    rootCell->liveObject = obj.get_ptr();
    return obj;
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExpr", expr);
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    /* #177: parent = sessionRootCell so env facts inherit. */
    auto rootCell = RegularArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorExpr rootSel{expr, basePath.path.abs()};
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, rootSel);
    auto result = inner->evalExpr(expr, basePath);
    auto whnf = computeWHNFFromObject(*result, inner->getEvalState());
    auto triePos = writer.logResult(v, whnf, qh, rootCell);
    /* Bootstrap the SelectorPool with this evalExpr's root Selector. */
    auto obj = TracingObject::create(result, writer, v, triePos, rootCell,
        inner, ref<OuterResolver>(inner->getOuterResolver()),
        writer.getDecisionGraph().selectorPool.intern(rootSel),
        std::move(whnf));
    rootCell->liveObject = obj.get_ptr();
    return obj;
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExprLazy", expr);
    ensurePreloaded();
    auto rootCell = RegularArgCell::make(writer.sessionRootCell, nullptr);
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, trace::SelectorExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    auto obj = TracingObject::create(result, writer, v, std::nullopt, rootCell,
        inner, ref<OuterResolver>(inner->getOuterResolver()));
    rootCell->liveObject = obj.get_ptr();
    return obj;
}

/* Leaf synthesisers delegate straight to inner. Wrapping them in
   TracingObject was a no-op passthrough — argCell/producer unset, the
   synthesised triePos hash never interned as a Selector, every getter
   falling back through the `findByHex` miss branch. Matches
   TracingReplayEvaluator's mk* and CoarseEvalCache's mk*. */
ref<Object> TracingEvaluator::mkString(const std::string & s) { return inner->mkString(s); }
ref<Object> TracingEvaluator::mkInt(NixInt i) { return inner->mkInt(i); }
ref<Object> TracingEvaluator::mkBool(bool b) { return inner->mkBool(b); }
ref<Object> TracingEvaluator::mkPath(const RootedPath & path) { return inner->mkPath(path); }
ref<Object> TracingEvaluator::getInternalPrimOp(const std::string & name) { return inner->getInternalPrimOp(name); }
ref<Object> TracingEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs) { return inner->mkAttrs(attrs); }

ref<Object> TracingEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    /* TE::apply is a thin frontend for the fn's own queryApply.
       Tracing behaviour (arg-wrapping, SelectorApply recording,
       result wrapping) lives in TracingObject::queryApply where it
       belongs — untraced fn types (InterpreterObject, etc.) route
       through their own queryApply and don't record. */
    auto result = fn->queryApply(arg.get_ptr());
    if (!result)
        panic("TracingEvaluator::apply: fn->queryApply returned null");
    return ref<Object>(result);
}

} // namespace nix

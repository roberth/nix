#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/outer-object.hh"
#include "nix/expr/expr-from-object.hh"
#include "nix/expr/tracing-callback-apply-result.hh"
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
    auto rootCell = ArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorImport rootSel{displayPath};
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, rootSel);
    auto result = inner->evalFile(path, displayPath);
    auto whnf = computeWHNFFromObject(*result);
    auto triePos = writer.logResult(v, whnf, qh, rootCell);
    auto obj = TracingObject::create(result, writer, v, triePos);
    const_cast<ArgCell &>(*rootCell).liveObject = obj.get_ptr();
    obj->withArgCell(std::move(rootCell));
    /* Bootstrap the SelectorPool: intern this evalFile's root Selector
       so descendants that build SelectorApplyStep{parent=this} etc.
       can resolve their parent via fromVariant. */
    if (auto * dg = writer.getDecisionGraph())
        obj->withProducer(dg->selectorPool.intern(rootSel));
    obj->withCachedWHNF(std::move(whnf));
    return obj;
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExpr", expr);
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    /* #177: parent = sessionRootCell so env facts inherit. */
    auto rootCell = ArgCell::make(writer.sessionRootCell, nullptr);
    trace::SelectorExpr rootSel{expr, basePath.path.abs()};
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, rootSel);
    auto result = inner->evalExpr(expr, basePath);
    auto whnf = computeWHNFFromObject(*result);
    auto triePos = writer.logResult(v, whnf, qh, rootCell);
    auto obj = TracingObject::create(result, writer, v, triePos);
    const_cast<ArgCell &>(*rootCell).liveObject = obj.get_ptr();
    obj->withArgCell(std::move(rootCell));
    /* Bootstrap the SelectorPool with this evalExpr's root Selector. */
    if (auto * dg = writer.getDecisionGraph())
        obj->withProducer(dg->selectorPool.intern(rootSel));
    obj->withCachedWHNF(std::move(whnf));
    return obj;
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    guardCacheRecording("evalExprLazy", expr);
    ensurePreloaded();
    auto rootCell = ArgCell::make(writer.sessionRootCell, nullptr);
    auto [v, qh] = writer.logRootSelectorOnCell(rootCell, trace::SelectorExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    auto obj = TracingObject::create(result, writer, v);
    const_cast<ArgCell &>(*rootCell).liveObject = obj.get_ptr();
    obj->withArgCell(std::move(rootCell));
    return obj;
}

ref<Object> TracingEvaluator::mkString(const std::string & s)
{
    auto result = inner->mkString(s);
    // Deterministic identity from content — no trie entry needed.
    auto hash = hashString(HashAlgorithm::SHA256, "mkString:" + s);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkInt(NixInt i)
{
    auto result = inner->mkInt(i);
    auto hash = hashString(HashAlgorithm::SHA256, "mkInt:" + std::to_string(i.value));
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkBool(bool b)
{
    auto result = inner->mkBool(b);
    auto hash = hashString(HashAlgorithm::SHA256, b ? "mkBool:true" : "mkBool:false");
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkPath(const RootedPath & path)
{
    auto result = inner->mkPath(path);
    /* Identity from the SourceRoot's unpinnedId + canon path. The
       unpinnedId strips revision-output attrs from the URL, so the
       same logical source at two different revs produces the same
       identity — exactly the property the trie needs to replay across
       upgrades of an input. SourceRoots without an unpinnedId (e.g.
       internal-helper accessors) fall back to a per-instance address;
       those are typically process-scoped and don't need cross-run
       replay anyway. */
    std::string content = "mkPath:";
    if (path.root->unpinnedId)
        content += *path.root->unpinnedId;
    else
        content += fmt("addr:%p", (void *) &*path.root);
    content += ":" + path.path.abs();
    auto hash = hashString(HashAlgorithm::SHA256, content);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::getInternalPrimOp(const std::string & name)
{
    auto result = inner->getInternalPrimOp(name);
    auto hash = hashString(HashAlgorithm::SHA256, "internalPrimOp:" + name);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    auto result = inner->mkAttrs(attrs);
    // Deterministic identity from attr names + child identities.
    std::string content = "mkAttrs:";
    for (auto & [name, obj] : attrs) {
        content += name + "=";
        if (auto hex = obj->getSelectorHashHex())
            content += *hex;
        content += ",";
    }
    auto hash = hashString(HashAlgorithm::SHA256, content);
    auto hashStr = hash.to_string(HashFormat::Base16, false);
    auto triePos = TriePosition{.resultNodeHash = hash, .queryHashStr = hashStr};
    auto v = writer.getSink().allocValue();
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    /* fn and arg must be cache-boundary proxies whose identity is
       content-defined. No counter fallback — see the parallel
       comment in TracingReplayEvaluator::apply. */
    auto getId = [](Object & obj) -> std::string {
        if (auto hex = obj.getSelectorHashHex())
            return *hex;
        throw Error(
            "TracingEvaluator::apply: fn/arg lacks a content-defined "
            "identity (type %s). Wrap it as a cache-boundary proxy at its "
            "construction site.", typeid(obj).name());
    };

    auto fnStateHashStr = getId(*fn);
    auto argStateHashStr = getId(*arg);

    tracingCacheLog("tracing: apply fnStateHash=%s argStateHash=%s", fnStateHashStr, argStateHashStr);

    /* cb-apply: record an explicit ε edge for this apply.
       createCallbackCell closes the preceding observations as one
       Asks edge (β1) and then records a synthetic single-observation
       Asks edge (ε) carrying just the apply Request — both sides
       advance their cumulative subject-id history by one for ε, so the
       apply-result's state hash is computed at a history step the walker
       can reach via the recorded chain. */
    /* Intern SelectorApply for fn. Symmetric to TRE::apply — under the
       producer-propagation fixes (`44e212e07`) fn always has a producer
       Selector by construction; the audit committed as `fa2197831`
       confirmed the fallback paths were dead. */
    auto fnSelOpt = fn->getSelector();
    if (!fnSelOpt)
        unreachable();
    auto fnSel = *fnSelOpt;
    auto applySel = writer.getDecisionGraph()->selectorPool.intern(trace::SelectorApply{fnSel});
    nlohmann::json applyQ = trace::toJson(*applySel);

    /* If fn is a TracingCallbackArg (= inner-supplied lambda the
       outer is now applying — the cb-higher-order case), capture the
       enclosing cell's applyId before proceeding so that observations
       recorded on the apply-result (via TracingCallbackApplyResult
       below) route to the enclosing CallbackCell's runningObsSet
       rather than to a fresh cell.

       Skip `createCallbackCell` entirely for the TracingCallbackArg-fn
       path: pushing a fresh empty cell there would produce spurious
       state that the enclosing cell already covers, since the nested
       apply is itself an observation on the enclosing cell's
       contra-arg. */
    bool fnIsTlo = dynamic_cast<TracingCallbackArg *>(fn.get_ptr().get()) != nullptr;

    /* Build the apply-result producer Selector.

       fn's identity hex comes directly from `getSelectorHashHex()`:
        - OuterObject / TracingObject apply-result / TracingCallbackArg /
          TCallbackApplyResult all return the content hash of their stored
          producer Selector.
        - TracingObject non-apply-result (fresh from evalFile, nav child)
          returns triePos.queryHashStr — the parent Selector's Q hash.

       Nullopt (raw Object without state) falls back to the incoming
       fnStateHashStr the caller already computed for logging.

       arg identity is dropped from the payload per #181; discrimination
       flows through the arg's own cell/facts. */
    auto fnQHex = fn->getSelectorHashHex().value_or(fnStateHashStr);
    auto & resultProducer = std::get<trace::SelectorApply>(applySel->node);

    /* #183: one cell per call, tracking the arg. Reuse the arg's
       existing cell (created at the first opportunity, e.g., seedCell
       in makeCachedFnPrimOp.impl) if available; otherwise create.
       Resolved early so createCallbackCell can populate its
       callbackState in the same step. */
    /* Under #188's consolidation the arg always has a cell by this
       point (seedCell on the primop path, applyCell propagation on
       nested callback paths). Panic on any fallback so a future
       regression surfaces immediately instead of silently allocating
       a redundant cell. */
    auto cell = effectiveArgCell(*arg);
    if (!cell)
        throw Error("TracingEvaluator::apply: arg had no argCell (fn=%s arg=%s)",
                    fnStateHashStr.substr(0, 12), argStateHashStr.substr(0, 12));

    if (fnIsTlo) {
        /* Nested cb-apply (fn is a TracingCallbackArg): the recursive
           apply itself is an observation on the enclosing cell's
           contra-arg. Carried through the enclosing cell's
           runningObsSet — no dedicated fact record at the writer
           level here. */
    } else {
        tracingCacheLog("createCallbackCell callsite=TracingEvaluator::apply fn=%s arg=%s",
                        fnStateHashStr.substr(0, 12), argStateHashStr.substr(0, 12));
        writer.createCallbackCell(applyQ);
        /* #184: populate cell->callbackState so
           emitCallbackApplyForApplyResult's primary (cell-based) path
           finds it — mirrors what OuterApply::run does for its localCell. */
        cell->callbackState = std::make_shared<CallbackState>();
        cell->callbackState->fnStateHashHex = fnStateHashStr;
    }

    auto qHash = applySel->cachedHash;
    auto qHex = qHash.to_string(HashFormat::Base16, false);
    tracingCacheLog(
        "writer apply: fn=%s -> qHash=%s",
        fnQHex.substr(0, 12),
        qHex.substr(0, 16));

    /* Cell-migration Phase B: apply records SelectorApply as a proper
       Selector with its own Terminal, keyed on `cell` (resolved above).
       Observations during inner->apply attribute to this cell via
       queryFn's attributionCell; cell.factSetHash() reflects them for
       the SelectorApply Terminal cur — distinct calls have distinct
       cells → distinct Terminals. */
    auto & applySelector = std::get<trace::SelectorApply>(applySel->node);
    auto [v, qh] = writer.logSelectorOnCell(cell, applySelector);

    /* #178: siblingScope XOR retires. Sibling cached calls
       discriminate structurally via per-cell factset isolation. */
    auto result = inner->apply(fn, arg);

    /* For the TracingCallbackArg-fn case (cb-higher-order's recursive
       cb-apply): wrap the result in a TracingCallbackApplyResult so
       subsequent method calls (`getType`, `getInt`, etc.) route their
       observations into the enclosing CallbackCell's runningObsSet
       rather than into env main-trie Terminals. See
       tracing-callback-apply-result.hh for the recording flow.

       Phase B note: the ActiveSelector we pushed via
       logSelectorOnCell above still needs to pop. TLO path
       doesn't compute/log a SelectorApply Terminal (its result is
       routed via the callback cell), so we pop by discarding qh
       without a matching logResult — that would leave the stack
       inconsistent. Instead, log a trivial "function"-typed WHNF
       so the frame closes cleanly with a Terminal-of-record; the
       TLO path's downstream doesn't consult this Terminal. */
    if (fnIsTlo) {
        trace::ResultWHNF placeholderWhnf;
        placeholderWhnf.type = "lambda";
        placeholderWhnf.payload = trace::WHNFFunction{};
        writer.logResult(v, placeholderWhnf, qh, cell);
        auto laro = std::make_shared<TracingCallbackApplyResult>(
            result, writer, applySel);
        laro->withArgCell(cell);
        /* #184: the enclosing callback firing's cell is fn's cell — fn
           is a TracingCallbackArg whose argCell IS the OuterApply::run
           localCell (with callbackState populated). Route recordD2
           observations there directly instead of via applyId lookup. */
        laro->withCallbackCell(effectiveArgCell(*fn));
        return ref<Object>(laro);
    }

    /* Non-TLO path: compute WHNF from the (already-forced-by-inner)
       result, emit QCA against the applyResult (moved here from
       TracingObject::whnf so it still fires when the wrapper's
       cachedWHNF is pre-populated below), logResult inserts
       SelectorApply's Terminal, wrapper is constructed with
       cachedWHNF ready. */
    auto whnfResult = computeWHNFFromObject(*result);
    writer.emitCallbackApplyForApplyResult(cell, applySel, whnfResult);
    auto tp = writer.logResult(v, whnfResult, qh, cell);

    TriePosition triePos = tp
        ? *tp
        : TriePosition{
              .resultNodeHash = Hash{HashAlgorithm::SHA256},
              /* #181: use the SelectorApply Q hash (matches TRE::apply's
                 walker path); getSelectorHashHex() reads this back as fn's
                 identity for downstream applies. */
              .queryHashStr = qh.selectorHash
                  ? qh.selectorHash->to_string(HashFormat::Base16, false)
                  : qHex,
          };
    auto obj = TracingObject::create(result, writer, v, triePos);
    obj->withArgCell(std::move(cell));
    obj->withProducer(applySel);
    obj->withCachedWHNF(std::move(whnfResult));
    return obj;
}

} // namespace nix

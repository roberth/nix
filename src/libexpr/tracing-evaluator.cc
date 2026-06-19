#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/ambient-object.hh"
#include "nix/expr/tracing-decision-graph.hh"
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

ref<Object> TracingEvaluator::evalFile(const RootedPath & path, const std::string & displayPath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalFile %s", displayPath);
    auto [v, qh] = writer.logRootQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const RootedPath & basePath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const RootedPath & basePath)
{
    ensurePreloaded();
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    return TracingObject::create(result, writer, v);
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
        if (auto * to = dynamic_cast<TracingObject *>(&*obj)) {
            if (auto qh = to->getQueryHashStr())
                content += *qh;
        } else if (auto * ro = dynamic_cast<TracingReplayObject *>(&*obj)) {
            content += ro->getTriePos().queryHashStr;
        }
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
    // Get identity from TracingObject, TracingReplayObject, or
    // AmbientObject. AmbientObject is the case the `<cached-fn>` PrimOp
    // creates when wrapping the outer's args — without recognising it
    // here, argId falls through to virtual:N (per-writer counter
    // starting at 0), which collides across separate cache primop
    // calls and makes every subsequent call's child-Q lookup land at
    // the first call's recorded Terminal.
    auto getId = [](Object & obj) -> std::optional<std::string> {
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            return to->getQueryHashStr();
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return std::optional{ro->getTriePos().queryHashStr};
        if (auto * ao = dynamic_cast<AmbientObject *>(&obj))
            return ao->getId().to_string(HashFormat::Base16, false);
        return std::nullopt;
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    // Get or allocate virtual root identities for objects without trie
    // identity — values created by the Nix evaluator directly (e.g. `{}`
    // literals) that never passed through the Evaluator interface.
    if (!fnId)
        fnId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(fn).value());
    if (!argId)
        argId = "virtual:" + std::to_string(writer.getOrAllocVirtualRoot(arg).value());

    tracingCacheLog("tracing: apply fnId=%s argId=%s", fnId ? *fnId : "none", argId ? *argId : "none");
    /* Don't record a Q_apply Terminal: a fresh app thunk has no
       result type. Compute the TriePosition structurally so
       downstream queries on this apply result still chain off
       Q_apply via queryHashStr (Merkle parent for v13's queryHash). */
    auto queryHash = TracingDecisionGraph::computeQueryHash(trace::QueryApply{*fnId, *argId});
    auto v = writer.getSink().logQuery(trace::QueryApply{*fnId, *argId});
    auto result = inner->apply(fn, arg);
    TriePosition triePos{
        .resultNodeHash = Hash{HashAlgorithm::SHA256}, // sentinel; v13 doesn't key off this
        .queryHashStr = queryHash.to_string(HashFormat::Base16, false),
    };
    return TracingObject::create(result, writer, v, triePos);
}

} // namespace nix

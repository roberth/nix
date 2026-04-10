#include "nix/expr/tracing-evaluator.hh"
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

namespace nix {

static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs:
        return "set";
    case nList:
        return "list";
    case nString:
        return "string";
    case nPath:
        return "path";
    case nInt:
        return "int";
    case nFloat:
        return "float";
    case nBool:
        return "bool";
    case nNull:
        return "null";
    case nFunction:
        return "lambda";
    case nThunk:
        return "thunk";
    case nExternal:
        return "external";
    case nFailed:
        return "failed";
    }
    return "unknown";
}

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
            auto expr = evalState.parseExprFromString(std::move(file.result.contents), sourcePath.parent());
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

ref<Object> TracingEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalFile %s", displayPath);
    auto [v, qh] = writer.logRootQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    ensurePreloaded();
    tracingCacheLog("tracing: evalExpr %s", expr);
    auto [v, qh] = writer.logRootQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    return TracingObject::create(result, writer, v, triePos);
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const SourcePath & basePath)
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
    // Get identity from TracingObject or TracingReplayObject.
    auto getId = [](Object & obj) -> std::optional<std::string> {
        if (auto * to = dynamic_cast<TracingObject *>(&obj))
            return to->getQueryHashStr();
        if (auto * ro = dynamic_cast<TracingReplayObject *>(&obj))
            return std::optional{ro->getTriePos().queryHashStr};
        return std::nullopt;
    };

    auto fnId = getId(*fn);
    auto argId = getId(*arg);

    // Assign virtual root identities to objects without trie identity.
    // These are values created by the Nix evaluator directly (e.g. `{}`
    // literals) that never passed through the Evaluator interface.
    if (!fnId)
        fnId = "virtual:" + std::to_string(writer.allocVirtualRoot());
    if (!argId)
        argId = "virtual:" + std::to_string(writer.allocVirtualRoot());

    tracingCacheLog("tracing: apply");
    auto [v, qh] = writer.logRootQuery(trace::QueryApply{*fnId, *argId});
    auto result = inner->apply(fn, arg);
    auto type = result->getTypeLazy();
    auto triePos = writer.logResult(v, trace::ResultType{objectTypeToString(type)}, qh);
    return TracingObject::create(result, writer, v, triePos);
}

} // namespace nix

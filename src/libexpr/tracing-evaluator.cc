#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-database.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/eval.hh"
#include "nix/expr/environment.hh"
#include "nix/util/thread-pool.hh"
#include "nix/util/sync.hh"

#include <iostream>

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
    }
}

TracingEvaluator::TracingEvaluator(TraceFile & traceFile, ref<Evaluator> inner, TracingDatabase & db)
    : traceFile(traceFile)
    , inner(inner)
{
    // Preload files from the previous trace
    {
        auto evalState = inner->getEvalState();
        if (!evalState)
            return;

        auto latestTrace = db.latestTraceFile();
        if (!latestTrace)
            return;

        auto filePaths = db.getTracedFilePaths(*latestTrace);
        if (filePaths.empty())
            return;

        // Get the tracing source accessor from the environment
        auto accessor = evalState->environment->fsRoot();
        auto tracingAccessor = dynamic_cast<TracingSourceAccessor *>(&*accessor);
        if (!tracingAccessor)
            return;

        // Read files in parallel (I/O bound)
        struct PreloadedFile
        {
            CanonPath path;
            SpeculativeReadResult result;
        };

        Sync<std::vector<PreloadedFile>> preloaded;

        ThreadPool pool;
        for (const auto & pathStr : filePaths) {
            pool.enqueue([&, pathStr]() {
                try {
                    auto canonPath = CanonPath(pathStr);
                    auto result = tracingAccessor->readSpeculatively(canonPath);
                    preloaded.lock()->push_back(
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
        for (auto & file : *preloaded.lock()) {
            try {
                auto sourcePath = SourcePath{accessor, file.path};
                // parseExprFromString expects basePath (parent directory), not file path
                auto expr = evalState->parseExprFromString(std::move(file.result.contents), sourcePath.parent());
                evalState->insertPreloadedParsedFile(sourcePath, expr, std::move(file.result.emitTrace));
            } catch (...) {
                // Ignore parse errors during preload
            }
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

ref<Object> TracingEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    auto v = traceFile.logQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    traceFile.logResult(v, trace::ResultType{objectTypeToString(type)});
    return TracingObject::create(result, traceFile, v);
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    auto v = traceFile.logQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    traceFile.logResult(v, trace::ResultType{objectTypeToString(type)});
    return TracingObject::create(result, traceFile, v);
}

} // namespace nix

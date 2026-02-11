#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-database.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/trace-types.hh"

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

TracingEvaluator::TracingEvaluator(TraceFile & traceFile, ref<Evaluator> inner)
    : traceFile(traceFile)
    , inner(inner)
{
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

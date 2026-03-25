#include "nix/expr/tracing-evaluator.hh"
#include "nix/expr/tracing-object.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

static std::string objectTypeToString(ObjectType type)
{
    switch (type) {
    case nAttrs: return "set";
    case nList: return "list";
    case nString: return "string";
    case nPath: return "path";
    case nInt: return "int";
    case nFloat: return "float";
    case nBool: return "bool";
    case nNull: return "null";
    case nFunction: return "lambda";
    case nThunk: return "thunk";
    case nExternal: return "external";
    case nFailed: return "failed";
    }
    return "unknown";
}

TracingEvaluator::TracingEvaluator(TraceSink & sink, ref<Evaluator> inner)
    : sink(sink)
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

EvalState & TracingEvaluator::getEvalState()
{
    return inner->getEvalState();
}

ref<Object> TracingEvaluator::evalFile(const SourcePath & path, const std::string & displayPath)
{
    auto v = sink.logQuery(trace::QueryImport{displayPath});
    auto result = inner->evalFile(path, displayPath);
    auto type = result->getType();
    sink.logResult(v, trace::ResultType{objectTypeToString(type)});
    return TracingObject::create(result, sink, v);
}

ref<Object> TracingEvaluator::evalExpr(const std::string & expr, const SourcePath & basePath)
{
    auto v = sink.logQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExpr(expr, basePath);
    auto type = result->getType();
    sink.logResult(v, trace::ResultType{objectTypeToString(type)});
    return TracingObject::create(result, sink, v);
}

ref<Object> TracingEvaluator::evalExprLazy(const std::string & expr, const SourcePath & basePath)
{
    auto v = sink.logQuery(trace::QueryExpr{expr, basePath.path.abs()});
    auto result = inner->evalExprLazy(expr, basePath);
    // Lazy: don't force type yet, just wrap
    return TracingObject::create(result, sink, v);
}

ref<Object> TracingEvaluator::mkString(const std::string & s)
{
    // Construction operations don't need tracing — they produce known values
    return inner->mkString(s);
}

ref<Object> TracingEvaluator::mkAttrs(const std::map<std::string, ref<Object>> & attrs)
{
    return inner->mkAttrs(attrs);
}

ref<Object> TracingEvaluator::apply(ref<Object> fn, ref<Object> arg)
{
    // Function application: the result is traced via the returned Object
    auto result = inner->apply(fn, arg);
    auto v = sink.allocValue();
    return TracingObject::create(result, sink, v);
}

} // namespace nix

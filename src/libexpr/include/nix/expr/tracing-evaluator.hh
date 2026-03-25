#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/util/ref.hh"

namespace nix {

class TracingDatabase;

/**
 * Evaluator decorator that logs all queries and results to a TraceSink.
 * Wraps returned Objects in TracingObject to continue tracing through
 * attribute access and value extraction.
 */
class TracingEvaluator : public Evaluator
{
    TraceSink & sink;
    ref<Evaluator> inner;

public:
    /**
     * @param sink The trace sink to log to
     * @param inner The wrapped evaluator
     * @param db Optional tracing database for preloading from previous traces
     */
    TracingEvaluator(TraceSink & sink, ref<Evaluator> inner, TracingDatabase * db = nullptr);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;
    EvalState & getEvalState() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> evalExprLazy(const std::string & expr, const SourcePath & basePath) override;
    ref<Object> mkString(const std::string & s) override;
    ref<Object> mkAttrs(const std::map<std::string, ref<Object>> & attrs) override;
    ref<Object> apply(ref<Object> fn, ref<Object> arg) override;
};

} // namespace nix

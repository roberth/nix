#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/util/ref.hh"

namespace nix {

class Store;
class TraceFile;
class TracingDatabase;

namespace fetchers {
struct Settings;
}

class TracingEvaluator : public Evaluator
{
    TraceFile & traceFile;
    ref<Evaluator> inner;

public:
    /**
     * Create a tracing evaluator.
     * @param traceFile The trace file to log to
     * @param inner The wrapped evaluator
     * @param db Tracing database for preloading from previous traces
     */
    TracingEvaluator(TraceFile & traceFile, ref<Evaluator> inner, TracingDatabase & db);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
};

} // namespace nix

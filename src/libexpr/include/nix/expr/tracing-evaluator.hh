#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/util/ref.hh"

namespace nix {

class Store;
class TraceFile;

namespace fetchers {
struct Settings;
}

class TracingEvaluator : public Evaluator
{
    TraceFile & traceFile;
    ref<Evaluator> inner;

public:
    TracingEvaluator(TraceFile & traceFile, ref<Evaluator> inner);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
};

} // namespace nix

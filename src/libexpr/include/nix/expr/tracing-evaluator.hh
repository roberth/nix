#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

#include <memory>

namespace nix {

class Store;
class TraceFile;
class TracingDatabase;
class TracingIndex;

namespace fetchers {
struct Settings;
}

class TracingEvaluator : public Evaluator
{
    TracingWriter & writer;
    ref<Evaluator> inner;
    TracingDatabase & db;
    bool preloaded = false;

    void ensurePreloaded();

public:
    /**
     * Create a tracing evaluator.
     * @param writer The tracing writer (shared with TracingEnvironment)
     * @param inner The wrapped evaluator
     * @param db Tracing database for preloading from previous traces
     */
    TracingEvaluator(TracingWriter & writer, ref<Evaluator> inner, TracingDatabase & db);

    bool isReadOnly() const override;
    Store & getStore() override;
    const fetchers::Settings & getFetchSettings() override;

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;
};

} // namespace nix

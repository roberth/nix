#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/tracing-writer.hh"
#include "nix/util/ref.hh"

namespace nix {

class TracingDatabase;

/**
 * Evaluator decorator that logs all queries and results via TracingWriter.
 * Wraps returned Objects in TracingObject to continue tracing through
 * attribute access and value extraction.
 */
class TracingEvaluator : public Evaluator
{
    TracingWriter & writer;
    ref<Evaluator> inner;
    TracingDatabase * db;
    bool preloaded = false;

    void ensurePreloaded();

public:
    /**
     * @param writer The tracing writer (logs to both JSON and optionally trie)
     * @param inner The wrapped evaluator
     * @param db Optional tracing database for preloading from previous traces
     */
    TracingEvaluator(TracingWriter & writer, ref<Evaluator> inner, TracingDatabase * db = nullptr);

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

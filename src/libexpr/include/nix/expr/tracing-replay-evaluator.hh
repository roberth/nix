#pragma once

#include "nix/expr/evaluator.hh"
#include "nix/expr/file-hash-cache.hh"
#include "nix/expr/trace-types.hh"
#include "nix/util/ref.hh"

#include <vector>

namespace nix {

class TracingDatabase;

/**
 * Evaluator that replays cached results from a previous trace.
 *
 * On each evalFile/evalExpr call, checks if the operation matches
 * a cached entry in the loaded trace. If so, returns a
 * TracingReplayObject that serves results from cache. Otherwise,
 * defers to the inner evaluator.
 *
 * Environment interactions (file reads, env vars) between queries
 * are validated before returning cached results.
 */
class TracingReplayEvaluator : public Evaluator
{
    ref<Evaluator> inner;
    TracingDatabase & db;
    FileHashCache hashCache;

    std::vector<trace::TraceEntry> trace;
    std::optional<trace::QueryIndex> index;
    size_t cursor = 0;
    bool invalidated = false;

    /**
     * Validate all environment interactions (file reads, env lookups)
     * between the cursor and the target position. Returns true if all
     * validations pass; false if any fail (caller should invalidate).
     */
    bool validateEnvTo(size_t targetPos);

public:
    TracingReplayEvaluator(ref<Evaluator> inner, TracingDatabase & db);

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

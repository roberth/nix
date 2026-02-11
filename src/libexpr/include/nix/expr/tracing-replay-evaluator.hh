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
 * Maintains a cursor into the loaded trace. On each call, checks if
 * the operation matches the trace at the cursor position. If not,
 * defers to the inner evaluator.
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

    ref<Object> evalFile(const SourcePath & path, const std::string & displayPath) override;
    ref<Object> evalExpr(const std::string & expr, const SourcePath & basePath) override;

    EvalState * getEvalState() override;
};

} // namespace nix

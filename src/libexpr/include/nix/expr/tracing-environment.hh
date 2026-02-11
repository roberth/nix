#pragma once
/**
 * @file
 * TracingEnvironment - Environment wrapper that records traces.
 */

#include "nix/expr/environment.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/util/ref.hh"

namespace nix {

class TraceFile;

/**
 * Environment wrapper that records all operations as traces.
 *
 * Wraps another Environment and logs all I/O operations for later replay.
 */
class TracingEnvironment : public Environment
{
    ref<Environment> inner;
    TraceFile & traceFile;
    ref<TracingSourceAccessor> tracingAccessor;

public:
    TracingEnvironment(ref<Environment> inner, TraceFile & traceFile);

    ref<SourceAccessor> fsRoot() override;
    std::optional<std::string> getEnv(const std::string & name) override;

    TraceFile * getTraceFile() override
    {
        return &traceFile;
    }
};

} // namespace nix

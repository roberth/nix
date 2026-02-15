#pragma once
/**
 * @file
 * TracingEnvironment - Environment wrapper that records traces.
 */

#include "nix/expr/environment.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/util/ref.hh"

namespace nix {

class TracingWriter;

/**
 * Environment wrapper that records all operations as traces.
 *
 * Wraps another Environment and logs all I/O operations for later replay.
 * Uses TracingWriter to record to both JSON trace and trie index.
 */
class TracingEnvironment : public Environment
{
    ref<Environment> inner;
    TracingWriter & writer;
    ref<TracingSourceAccessor> tracingAccessor;

public:
    TracingEnvironment(ref<Environment> inner, TracingWriter & writer);

    ref<SourceAccessor> fsRoot() override;
    std::optional<std::string> getEnv(const std::string & name) override;

    TraceFile * getTraceFile() override;
};

} // namespace nix

#pragma once
/**
 * @file
 * TracingEnvironment - Environment wrapper that records I/O traces.
 */

#include "nix/expr/environment.hh"
#include "nix/expr/trace-sink.hh"
#include "nix/util/ref.hh"

namespace nix {

class TracingSourceAccessor;

/**
 * Environment wrapper that records all I/O operations as traces.
 *
 * File reads are traced via TracingSourceAccessor (SHA256 content hashes).
 * Environment variable lookups are traced as GetEnvRequest/Response pairs.
 */
class TracingEnvironment : public Environment
{
    ref<Environment> inner;
    TraceSink & sink;
    ref<TracingSourceAccessor> tracingAccessor;

public:
    TracingEnvironment(ref<Environment> inner, TraceSink & sink);

    ref<SourceAccessor> fsRoot() override;
    std::optional<std::string> getEnv(const std::string & name) override;
    TraceSink * getTraceSink() override { return &sink; }
};

} // namespace nix

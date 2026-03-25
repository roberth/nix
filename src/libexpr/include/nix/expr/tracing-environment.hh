#pragma once
/**
 * @file
 * TracingEnvironment - Environment wrapper that records I/O traces.
 */

#include "nix/expr/environment.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/util/ref.hh"

namespace nix {

class TracingWriter;

/**
 * Environment wrapper that records all I/O operations as traces.
 *
 * File reads are traced via TracingSourceAccessor (SHA256 content hashes).
 * Environment variable lookups are traced as GetEnvRequest/Response pairs.
 * Uses TracingWriter to record to both JSON trace and optionally trie index.
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
    TraceSink * getTraceSink() override;
};

} // namespace nix

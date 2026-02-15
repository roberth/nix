#include "nix/expr/tracing-environment.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-writer.hh"

namespace nix {

TracingEnvironment::TracingEnvironment(ref<Environment> inner, TracingWriter & writer)
    : inner(inner)
    , writer(writer)
    , tracingAccessor(
          make_ref<TracingSourceAccessor>(inner->fsRoot(), [&](const trace::Response<trace::FileReadRequest> & resp) {
              // Log via writer to both JSON trace and trie index
              writer.logResponse(resp);
          }))
{
}

ref<SourceAccessor> TracingEnvironment::fsRoot()
{
    return tracingAccessor;
}

std::optional<std::string> TracingEnvironment::getEnv(const std::string & name)
{
    auto result = inner->getEnv(name);

    trace::Response<trace::GetEnvRequest> resp{
        .request = {.name = name},
        .response = {.value = result},
    };
    writer.logResponse(resp);

    return result;
}

TraceFile * TracingEnvironment::getTraceFile()
{
    return &writer.getTraceFile();
}

} // namespace nix

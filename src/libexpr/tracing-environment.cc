#include "nix/expr/tracing-environment.hh"
#include "nix/expr/tracing-source-accessor.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

TracingEnvironment::TracingEnvironment(ref<Environment> inner, TraceSink & sink)
    : inner(inner)
    , sink(sink)
    , tracingAccessor(make_ref<TracingSourceAccessor>(inner->fsRoot(), sink))
{
}

ref<SourceAccessor> TracingEnvironment::fsRoot()
{
    return tracingAccessor;
}

std::optional<std::string> TracingEnvironment::getEnv(const std::string & name)
{
    auto result = inner->getEnv(name);

    sink.logEnvResponse(trace::Response<trace::GetEnvRequest>{
        .request = {.name = name},
        .response = {.value = result},
    });

    return result;
}

} // namespace nix

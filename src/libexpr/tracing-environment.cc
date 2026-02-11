#include "nix/expr/tracing-environment.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-database.hh"

namespace nix {

TracingEnvironment::TracingEnvironment(ref<Environment> inner, TraceFile & traceFile)
    : inner(inner)
    , traceFile(traceFile)
    , tracingAccessor(
          make_ref<TracingSourceAccessor>(inner->fsRoot(), [&](const nlohmann::json & entry) { traceFile.log(entry); }))
{
}

ref<SourceAccessor> TracingEnvironment::fsRoot()
{
    return tracingAccessor;
}

std::optional<std::string> TracingEnvironment::getEnv(const std::string & name)
{
    auto result = inner->getEnv(name);

    trace::Response<trace::GetEnvRequest> t{
        .request = {.name = name},
        .response = {.value = result},
    };
    traceFile.log(t);

    return result;
}

} // namespace nix

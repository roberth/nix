#include "nix/expr/tracing-environment.hh"
#include "nix/expr/trace-types.hh"
#include "nix/expr/tracing-writer.hh"

namespace nix {

TracingEnvironment::TracingEnvironment(ref<Environment> inner, TracingWriter & writer)
    : inner(inner)
    , writer(writer)
    , writerAlive(std::make_shared<bool>(true))
    , tracingAccessor(
          make_ref<TracingSourceAccessor>(
              inner->fsRoot(),
              [alive = this->writerAlive, &writer](const trace::Response<trace::FileReadRequest> & resp) {
                  if (!*alive)
                      throw Error("TracingSourceAccessor: logFn called after TracingWriter destroyed");
                  writer.logResponse(resp);
              }))
{
}

TracingEnvironment::~TracingEnvironment()
{
    *writerAlive = false;
    tracingAccessor->disable();
}

ref<SourceAccessor> TracingEnvironment::fsRoot()
{
    return tracingAccessor;
}

Hash TracingEnvironment::getFileHash(const std::string & path)
{
    auto hash = inner->getFileHash(path);

    trace::Response<trace::FileReadRequest> resp{
        .request = {.absPath = path},
        .response = {.contentHash = hash},
    };
    writer.logResponse(resp);

    return hash;
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

trace::ResultVariant TracingEnvironment::ambientQuery(
    const trace::QueryVariant & query,
    std::function<trace::ResultVariant(const trace::QueryVariant &)> resolve)
{
    auto result = resolve(query);
    writer.logAmbientInteraction(query, result);
    return result;
}

TraceSink * TracingEnvironment::getTraceSink()
{
    return &writer.getSink();
}

} // namespace nix

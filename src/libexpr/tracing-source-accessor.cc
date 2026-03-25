#include "nix/expr/tracing-source-accessor.hh"
#include "nix/expr/trace-types.hh"

namespace nix {

TracingSourceAccessor::TracingSourceAccessor(ref<SourceAccessor> inner, TraceSink & sink)
    : inner(inner)
    , sink(sink)
{
}

void TracingSourceAccessor::readFile(const CanonPath & path, Sink & destSink, fun<void(uint64_t)> sizeCallback)
{
    // Read via string to compute content hash
    auto contents = inner->readFile(path);
    auto hash = hashString(HashAlgorithm::SHA256, contents);

    sink.logEnvResponse(trace::Response<trace::FileReadRequest>{
        .request = {.absPath = path.abs()},
        .response = {.contentHash = hash},
    });

    sizeCallback(contents.size());
    destSink(contents);
}

std::optional<SourceAccessor::Stat> TracingSourceAccessor::maybeLstat(const CanonPath & path)
{
    return inner->maybeLstat(path);
}

SourceAccessor::DirEntries TracingSourceAccessor::readDirectory(const CanonPath & path)
{
    return inner->readDirectory(path);
}

std::string TracingSourceAccessor::readLink(const CanonPath & path)
{
    return inner->readLink(path);
}

std::optional<std::filesystem::path> TracingSourceAccessor::getPhysicalPath(const CanonPath & path)
{
    return inner->getPhysicalPath(path);
}

std::string TracingSourceAccessor::showPath(const CanonPath & path)
{
    return inner->showPath(path);
}

} // namespace nix

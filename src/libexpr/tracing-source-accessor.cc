#include "nix/expr/tracing-source-accessor.hh"

namespace nix {

TracingSourceAccessor::TracingSourceAccessor(ref<SourceAccessor> inner, FileReadLogFn logFn)
    : inner(inner)
    , logFn(std::move(logFn))
{
}

SpeculativeReadResult TracingSourceAccessor::readSpeculatively(const CanonPath & path)
{
    auto contents = inner->readFile(path);
    auto hash = hashString(HashAlgorithm::SHA256, contents);
    auto pathStr = path.abs();

    auto emitTrace = [logFn = this->logFn, pathStr, hash]() {
        trace::Response<trace::FileReadRequest> resp{
            .request = {.absPath = pathStr},
            .response = {.contentHash = hash},
        };
        logFn(resp);
    };

    return SpeculativeReadResult{
        .contents = std::move(contents),
        .emitTrace = std::move(emitTrace),
    };
}

void TracingSourceAccessor::readFile(const CanonPath & path, Sink & destSink, fun<void(uint64_t)> sizeCallback)
{
    if (!enabled) {
        inner->readFile(path, destSink, sizeCallback);
        return;
    }

    // Read via string to compute content hash
    auto contents = inner->readFile(path);
    auto hash = hashString(HashAlgorithm::SHA256, contents);

    trace::Response<trace::FileReadRequest> resp{
        .request = {.absPath = path.abs()},
        .response = {.contentHash = hash},
    };
    logFn(resp);

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

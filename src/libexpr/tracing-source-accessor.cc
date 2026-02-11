#include "nix/expr/tracing-source-accessor.hh"

#include <iostream>

namespace nix {

TracingSourceAccessor::TracingSourceAccessor(
    ref<SourceAccessor> inner, std::function<void(const nlohmann::json &)> logFn)
    : inner(inner)
    , logFn(std::move(logFn))
{
}

SpeculativeReadResult TracingSourceAccessor::readSpeculatively(const CanonPath & path)
{
    auto contents = inner->readFile(path);
    auto hash = hashString(HashAlgorithm::SHA256, contents);
    auto pathStr = path.abs();

    // Capture what we need for deferred trace emission
    auto emitTrace = [logFn = this->logFn, pathStr, hash]() {
        trace::Response<trace::FileReadRequest> trace{
            .request = {.absPath = pathStr},
            .response = {.contentHash = hash},
        };
        logFn(trace);
    };

    return SpeculativeReadResult{
        .contents = std::move(contents),
        .emitTrace = std::move(emitTrace),
    };
}

std::string TracingSourceAccessor::readFile(const CanonPath & path)
{
    auto contents = inner->readFile(path);
    auto hash = hashString(HashAlgorithm::SHA256, contents);

    trace::Response<trace::FileReadRequest> trace{
        .request = {.absPath = path.abs()},
        .response = {.contentHash = hash},
    };
    logFn(trace);

    return contents;
}

void TracingSourceAccessor::readFile(const CanonPath & path, Sink & sink, std::function<void(uint64_t)> sizeCallback)
{
    // Read via string to get hash, then write to sink
    auto contents = readFile(path);
    sizeCallback(contents.size());
    sink(contents);
}

bool TracingSourceAccessor::pathExists(const CanonPath & path)
{
    return inner->pathExists(path);
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

AllowListSourceAccessor * TracingSourceAccessor::asAllowListSourceAccessor()
{
    return inner->asAllowListSourceAccessor();
}

} // namespace nix

#pragma once
/**
 * @file
 * TracingSourceAccessor - SourceAccessor wrapper that records file access traces.
 */

#include "nix/expr/trace-sink.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/ref.hh"

namespace nix {

/**
 * SourceAccessor wrapper that traces file read operations with content hashes.
 * Other operations (pathExists, lstat, readDirectory, etc.) delegate without
 * tracing — they don't affect evaluation results, only file dispatch.
 */
class TracingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> inner;
    TraceSink & sink;

public:
    TracingSourceAccessor(ref<SourceAccessor> inner, TraceSink & sink);

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
    std::string showPath(const CanonPath & path) override;
};

} // namespace nix

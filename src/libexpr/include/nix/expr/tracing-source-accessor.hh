#pragma once
/**
 * @file
 * TracingSourceAccessor - SourceAccessor wrapper that records file access traces.
 */

#include "nix/expr/trace-types.hh"
#include "nix/util/source-accessor.hh"
#include "nix/util/hash.hh"
#include "nix/util/ref.hh"

#include <functional>

namespace nix {

/**
 * Result of a speculative read — contains file contents and a trigger
 * function to emit the trace when the read is actually demanded.
 */
struct SpeculativeReadResult
{
    std::string contents;
    /** Call to emit the trace event. */
    std::function<void()> emitTrace;
};

/**
 * Callback type for logging file read responses.
 */
using FileReadLogFn = std::function<void(const trace::Response<trace::FileReadRequest> &)>;

/**
 * SourceAccessor wrapper that traces file read operations with content hashes.
 * Other operations (pathExists, lstat, readDirectory, etc.) delegate without
 * tracing — they don't affect evaluation results, only file dispatch.
 */
class TracingSourceAccessor : public SourceAccessor
{
    ref<SourceAccessor> inner;
    FileReadLogFn logFn;

public:
    TracingSourceAccessor(ref<SourceAccessor> inner, FileReadLogFn logFn);

    /**
     * Read file speculatively without emitting a trace.
     * Returns the contents plus a trigger function to emit the trace later.
     * Use this for parallel pre-parsing where the trace should only be
     * emitted when the file is actually demanded during evaluation.
     */
    SpeculativeReadResult readSpeculatively(const CanonPath & path);

    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;

    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
    std::string showPath(const CanonPath & path) override;
};

} // namespace nix

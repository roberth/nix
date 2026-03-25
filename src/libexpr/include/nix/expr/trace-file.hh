#pragma once
/**
 * @file
 * TraceFile - Concrete TraceSink that writes JSON trace entries to a file.
 * TracingDatabase - Manages trace file storage and discovery.
 */

#include "nix/expr/trace-sink.hh"
#include "nix/expr/trace-types.hh"

#include <filesystem>
#include <fstream>
#include <functional>

namespace nix {

/**
 * A single trace session that logs JSON entries to a file.
 *
 * Writes a JSON array of trace entries. Each entry is a query, result,
 * or environment request/response pair.
 */
class TraceFile : public TraceSink
{
    std::filesystem::path path;
    std::ofstream file;
    bool first = true;
    std::function<void()> onClose;

public:
    explicit TraceFile(std::filesystem::path path, std::function<void()> onClose = {});
    ~TraceFile();

    void log(const nlohmann::json & entry) override;

    const std::filesystem::path & getPath() const
    {
        return path;
    }
};

/**
 * Manages trace file storage.
 *
 * Traces are stored in ~/.cache/nix/eval-tracing-v0/traces/ as
 * individual JSON files. A "latest.json" symlink points to the most
 * recent trace.
 */
class TracingDatabase
{
    std::filesystem::path basePath;

public:
    TracingDatabase();

    std::filesystem::path tracesDir() const;

    /** Create a new unique trace file path (does not update symlink). */
    std::filesystem::path newTraceFile();

    /** Update the "latest.json" symlink to point to the given trace file. */
    void updateLatestSymlink(const std::filesystem::path & tracePath);

    /** Get the path to the most recent trace file, if any. */
    std::optional<std::filesystem::path> latestTraceFile() const;

    /** Parse a trace file into typed trace entries. */
    std::vector<trace::TraceEntry> parseTraceFile(const std::filesystem::path & tracePath) const;

    /**
     * Read file paths from a trace file.
     * Returns absolute paths of .nix files that were read during that trace.
     */
    std::vector<std::string> getTracedFilePaths(const std::filesystem::path & tracePath) const;
};

} // namespace nix

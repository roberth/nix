#pragma once
/**
 * @file
 * TraceFile - Concrete TraceSink that writes JSON trace entries to a file.
 * TracingDatabase - Manages trace file storage and discovery.
 */

#include "nix/expr/trace-sink.hh"

#include <filesystem>
#include <fstream>

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

public:
    explicit TraceFile(std::filesystem::path path);
    ~TraceFile();

    void log(const nlohmann::json & entry) override;

    const std::filesystem::path & getPath() const { return path; }
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

    /** Create a new unique trace file path and update the latest symlink. */
    std::filesystem::path newTraceFile();
};

} // namespace nix

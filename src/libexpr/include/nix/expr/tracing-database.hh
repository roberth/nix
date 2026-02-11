#pragma once

#include "nix/expr/trace-types.hh"

#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace nix {

/**
 * A single trace session that logs JSON entries to a file.
 */
class TraceFile
{
    std::filesystem::path path;
    std::ofstream file;
    bool first = true;
    uint64_t nextValueNum = 0;
    std::function<void()> onClose;

public:
    explicit TraceFile(std::filesystem::path path, std::function<void()> onClose = {});
    ~TraceFile();

    void log(const nlohmann::json & entry);

    /**
     * Allocate a new value number for tracing.
     */
    uint64_t allocValue();

    /**
     * Log a query and return its value number.
     */
    template<typename T>
    uint64_t logQuery(const T & queryPayload)
    {
        auto v = allocValue();
        log(trace::Query<T>{queryPayload, v});
        return v;
    }

    /**
     * Log a result for a value.
     */
    template<typename T>
    void logResult(uint64_t v, const T & resultPayload)
    {
        log(trace::Result<T>{resultPayload, v});
    }
};

class TracingDatabase
{
    std::filesystem::path basePath;

public:
    TracingDatabase();

    std::filesystem::path tracesDir() const;
    std::filesystem::path newTraceFile();

    /**
     * Update the "latest.json" symlink to point to the given trace file.
     * Should be called after a trace is complete.
     */
    void updateLatestSymlink(const std::filesystem::path & tracePath);

    /**
     * Get the path to the latest trace file, if one exists.
     */
    std::optional<std::filesystem::path> latestTraceFile() const;

    /**
     * Read file paths from a trace file.
     * Returns absolute paths of all files that were read during that trace.
     */
    std::vector<std::string> getTracedFilePaths(const std::filesystem::path & tracePath) const;

    /**
     * Parse a trace file into typed trace entries.
     */
    std::vector<trace::TraceEntry> parseTraceFile(const std::filesystem::path & tracePath) const;
};

} // namespace nix

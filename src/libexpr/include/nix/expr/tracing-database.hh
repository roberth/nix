#pragma once

#include "nix/expr/trace-types.hh"

#include <filesystem>
#include <fstream>

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

public:
    explicit TraceFile(std::filesystem::path path);
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
};

} // namespace nix

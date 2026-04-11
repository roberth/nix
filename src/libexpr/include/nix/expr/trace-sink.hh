#pragma once
/**
 * @file
 * Abstract sink for trace entries. TracingEvaluator and TracingObject log
 * through this interface; concrete implementations (TraceFile, TracingWriter)
 * handle persistence.
 */

#include "nix/expr/trace-ids.hh"
#include "nix/expr/trace-types.hh"

#include <nlohmann/json.hpp>

namespace nix {

/**
 * Receives trace entries from TracingEvaluator/TracingObject.
 */
class TraceSink
{
    uint64_t nextValueNum = 0;

public:
    virtual ~TraceSink() = default;

    /** Log a raw JSON trace entry. */
    virtual void log(const nlohmann::json & entry) = 0;

    /** Allocate a new value handle for tracing. */
    ValueHandle allocValue()
    {
        return ValueHandle(nextValueNum++);
    }

    /**
     * Log a query and return its value handle.
     */
    template<typename T>
    ValueHandle logQuery(const T & queryPayload)
    {
        auto v = allocValue();
        nlohmann::json j;
        trace::to_json(j, trace::Query<T>{queryPayload, v.value()});
        log(j);
        return v;
    }

    /**
     * Log a result for a value handle.
     */
    template<typename T>
    void logResult(ValueHandle v, const T & resultPayload)
    {
        nlohmann::json j;
        trace::to_json(j, trace::Result<T>{resultPayload, v.value()});
        log(j);
    }

    /**
     * Log an environment request/response pair.
     */
    template<typename T>
    void logEnvResponse(const trace::Response<T> & response)
    {
        nlohmann::json j;
        trace::to_json(j, response);
        log(j);
    }
};

} // namespace nix

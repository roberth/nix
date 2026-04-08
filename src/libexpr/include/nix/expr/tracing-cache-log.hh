#pragma once
/**
 * @file
 * Logging helper for the tracing eval cache.
 *
 * Logs at info level when _NIX_TRACING_CACHE_LOGGING=1 is set,
 * otherwise at debug level.
 */

#include "nix/util/logging.hh"
#include "nix/util/environment-variables.hh"

namespace nix {

/**
 * Get the log level for tracing cache messages.
 * Caches the result of checking _NIX_TRACING_CACHE_LOGGING.
 */
inline Verbosity tracingCacheLogLevel()
{
    static Verbosity level = getEnv("_NIX_TRACING_CACHE_LOGGING").value_or("") == "1" ? lvlInfo : lvlDebug;
    return level;
}

#define tracingCacheLog(args...) printMsg(tracingCacheLogLevel(), args)

} // namespace nix

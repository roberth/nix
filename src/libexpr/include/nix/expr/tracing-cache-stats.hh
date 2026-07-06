#pragma once
/**
 * @file
 * Per-process counters for `builtins.cache` walker outcomes.
 *
 * Tests use these to assert state hash-layer properties (same-shape collapse,
 * hit rate, etc.) that aren't visible from final-output comparison.
 *
 * Enable: set `NIX_CACHE_STATS_FILE=/path/to/stats.json`. On process
 * exit the file is written with `{ "hits": N, "misses": M, "fallbacks": F }`.
 *
 * - **hits**: `walk` matched a Terminal (fast path or walk()).
 * - **misses**: `walk` returned nullopt (no recorded entry reachable).
 * - **fallbacks**: a `TracingReplayObject` lookup miss activated the
 *   recording-side inner evaluator (`ensureInner` fired).
 *
 * The counters are global and process-wide because tests aggregate
 * across all `builtins.cache` calls; per-call detail can be added if
 * needed by extending the JSON shape.
 */

#include <cstdint>

namespace nix {

struct TracingCacheStats
{
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t fallbacks = 0;
};

/** Singleton accessor. Counters are zero-initialised on first call. */
TracingCacheStats & tracingCacheStats();

/**
 * Idempotent: arms an `atexit` handler that writes the current
 * counters to the path in `NIX_CACHE_STATS_FILE` if that env var is
 * set. Safe to call multiple times — registers only once per process.
 * Called from `prim_cache` so the handler only arms when a cache call
 * has actually happened.
 */
void armTracingCacheStatsExitWriter();

} // namespace nix

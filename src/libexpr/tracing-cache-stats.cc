#include "nix/expr/tracing-cache-stats.hh"
#include "nix/util/environment-variables.hh"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace nix {

TracingCacheStats & tracingCacheStats()
{
    static TracingCacheStats stats;
    return stats;
}

void armTracingCacheStatsExitWriter()
{
    static bool armed = false;
    if (armed)
        return;
    armed = true;

    auto path = getEnv("NIX_CACHE_STATS_FILE");
    if (!path)
        return;

    /* Capture path by value into the atexit closure via a static
       string — std::atexit takes a plain function pointer, no captures.
       Multiple `armTracingCacheStatsExitWriter` calls early-return on
       `armed`, so this static is written exactly once. */
    static std::string statsPath;
    statsPath = *path;

    std::atexit([]() {
        const auto & s = tracingCacheStats();
        nlohmann::json j = {
            {"hits", s.hits},
            {"misses", s.misses},
            {"fallbacks", s.fallbacks},
            {"persistent_substitution_collisions", s.persistentSubstitutionCollisions},
        };
        std::ofstream out(statsPath);
        if (out)
            out << j.dump();
    });
}

} // namespace nix

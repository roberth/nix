/* A plugin that holds the writes to the tarball cache still in one phase,
   so that `tarball-cancel.sh` can interrupt Nix in that phase. The phase is
   the value of `_NIX_TEST_TARBALL_CACHE_PAUSE`. Nix itself has no such
   hook: the plugin sets the phase hook of libfetchers when Nix loads it. */

#include "nix/fetchers/tarball-cache-gix.hh"
#include "nix/util/environment-variables.hh"

#include <chrono>
#include <cstdio>
#include <thread>

namespace {

struct RegisterPause
{
    RegisterPause()
    {
        auto phase = nix::getEnv("_NIX_TEST_TARBALL_CACHE_PAUSE");
        if (!phase || phase->empty())
            return;
        nix::fetchers::gix::setTarballCachePhaseHook(
            [phase = *phase](std::string_view reached, const std::function<bool()> & isCancelled) {
                if (reached != phase)
                    return;
                /* Not a log message: the test waits for this exact line. */
                std::fprintf(stderr, "nix: tarball cache test hook: paused in phase '%s'\n", phase.c_str());
                while (!isCancelled())
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
    }
};

RegisterPause registerPause;

} // namespace

/**
 * Fetch many tarballs into the tarball cache at once, as one task, and
 * report how long it took and how busy the reactor was.
 *
 * Usage: nix-fetchers-tarball-bench URL...
 *
 * The fetches run concurrently (`whenAll` of
 * `downloadIntoGixTarballCache()`), with `runOnReactor()` or, with
 * `_NIX_FILE_TRANSFER_BACKEND=thread`, with `runBlocking()`. The tarball
 * cache is the one of `XDG_CACHE_HOME`. A user interrupt cancels all
 * fetches; the program then prints "interrupted by the user" and exits with
 * status 1, like `nix`.
 *
 * This is a measurement tool for the task path under load. The load stands
 * for the concurrent insertions into the tarball cache that a parallel
 * evaluator causes when several threads force different inputs. Input
 * fetching itself stays lazy: Nix does not fetch inputs ahead of demand.
 */

#include "nix/fetchers/tarball.hh"
#include "nix/store/filetransfer.hh"
#include "nix/store/globals.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/logging.hh"
#include "nix/util/reactor.hh"
#include "nix/util/signals.hh"
#include "nix/util/util.hh"

#include <chrono>
#include <iostream>

#ifndef _WIN32
#  include <signal.h>
#endif

using namespace nix;

namespace {

Task<void> fetchCounting(FileTransferRequest request, size_t & done)
{
    co_await fetchers::downloadIntoGixTarballCache(std::move(request), false);
    ++done;
}

Task<void> fetchAll(const std::vector<std::string> & urls, size_t & done)
{
    std::vector<Task<void>> tasks;
    for (auto & url : urls)
        tasks.push_back(fetchCounting(FileTransferRequest(VerbatimURL(url)), done));
    co_await whenAll(std::move(tasks));
}

} // namespace

int main(int argc, char ** argv)
{
    try {
        initLibStore();
        /* For the messages of the watchdog of the reactor. */
        if (getEnv("NIX_TARBALL_BENCH_DEBUG"))
            verbosity = lvlDebug;
#ifndef _WIN32
        unix::startSignalHandlerThread();
        /* Like `initNix()`: a user interrupt reaches the threads of
           `runInThread` with this signal, whose default action ends the
           process. */
        struct sigaction act;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;
        act.sa_handler = [](int) {};
        if (sigaction(NIX_SIG_MULTI_INT, &act, nullptr))
            throw SysError("installing a handler for the interrupt signal of threads");
#endif

        std::vector<std::string> urls(argv + 1, argv + argc);
        size_t done = 0;
        bool onReactor = fileTransferUsesReactor();

        auto start = std::chrono::steady_clock::now();
        if (onReactor)
            runOnReactor(fetchAll(urls, done));
        else
            runBlocking(fetchAll(urls, done));
        auto elapsed = std::chrono::steady_clock::now() - start;

        auto stats = reactorStats();
        std::cout << "fetched " << done << " of " << urls.size() << " tarballs in "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << " ms ("
                  << (onReactor ? "reactor" : "thread") << "); reactor: busy "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(stats.busy).count() << " ms in "
                  << stats.polls << " polls, longest poll " << stats.longestPoll.count() << " us, busiest second "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(stats.busiestSecond).count() << " ms, "
                  << stats.slowPolls << " slow polls\n";
        return 0;
    } catch (Interrupted &) {
        std::cerr << "error: interrupted by the user\n";
        return 1;
    } catch (std::exception & e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

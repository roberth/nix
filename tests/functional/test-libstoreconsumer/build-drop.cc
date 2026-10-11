/**
 * Await a build from a task on the reactor, and drop the task once the
 * builder runs.
 *
 * Usage: test-build-drop DRV PID_FILE
 *
 * The builder of DRV writes its process ID to PID_FILE and then waits.
 * The program prints "dropped" when the wait for PID_FILE won the race and
 * the build was dropped, or "completed" when the build completed first.
 */

#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/store/build/worker.hh"
#include "nix/util/file-system.hh"
#include "nix/util/reactor.hh"

#include <iostream>

using namespace nix;

static Task<void> waitForFile(std::filesystem::path path)
{
    while (!pathExists(path) || readFile(path).empty())
        co_await sleepFor(std::chrono::milliseconds(50));
}

int main(int argc, char ** argv)
{
    try {
        if (argc != 3) {
            std::cerr << "Usage: " << argv[0] << " DRV PID_FILE\n";
            return 1;
        }

        initLibStore();

        auto store = openStore();
        auto drvPath = store->parseStorePath(argv[1]);

        Worker worker{*store, *store};

        std::vector<DerivedPath> reqs{DerivedPath::Built{
            .drvPath = makeConstantStorePathRef(drvPath),
            .outputs = OutputsSpec::All{},
        }};

        /* `runOnReactor` returns after the drop of the build and the
           cleanups that it deferred, so `worker` outlives them. */
        auto result =
            runOnReactor(race(worker.buildPathsWithResultsAsync(std::move(reqs), bmNormal), waitForFile(argv[2])));

        std::cout << (result.index() == 1 ? "dropped" : "completed") << "\n";
        return 0;

    } catch (const std::exception & e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}

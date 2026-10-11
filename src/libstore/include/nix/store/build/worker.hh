#pragma once
///@file

#include "nix/util/types.hh"
#include "nix/store/store-api.hh"
#include "nix/store/build.hh"
#include "nix/store/derived-path-map.hh"
#include "nix/store/build/goal.hh"
#include "nix/store/build-result.hh"
#include "nix/store/realisation.hh"
#include "nix/util/muxable-pipe.hh"

#include <functional>
#include <future>
#include <atomic>
#include <thread>
#include <queue>

namespace nix {

/* Forward definition. */
struct WorkerSettings;
struct DerivationTrampolineGoal;
struct DerivationGoal;
struct DerivationResolutionGoal;
struct DerivationBuildingGoal;
class PathSubstitutionGoal;
class DrvOutputSubstitutionGoal;

/**
 * Workaround for not being able to declare a something like
 *
 * ```c++
 * class PathSubstitutionGoal : public Goal;
 * ```
 * even when Goal is a complete type.
 *
 * This is still a static cast. The purpose of exporting it is to define it in
 * a place where `PathSubstitutionGoal` is concrete, and use it in a place where it
 * is opaque.
 */
GoalPtr upcast_goal(std::shared_ptr<PathSubstitutionGoal> subGoal);
GoalPtr upcast_goal(std::shared_ptr<DrvOutputSubstitutionGoal> subGoal);
GoalPtr upcast_goal(std::shared_ptr<DerivationGoal> subGoal);

typedef std::chrono::time_point<std::chrono::steady_clock> steady_time_point;

/**
 * A mapping used to remember for each child process to what goal it
 * belongs, and comm channels for receiving log data and output
 * path creation commands.
 */
struct Child
{
    WeakGoalPtr goal;
    Goal * goal2; // ugly hackery
    std::set<MuxablePipePollState::CommChannel> channels;
    bool respectTimeouts;
    bool inBuildSlot;
    /**
     * Time we last got output on stdout/stderr
     */
    steady_time_point lastOutput;
    steady_time_point timeStarted;
};

#ifndef _WIN32 // TODO Enable building on Windows
/* Forward definition. */
struct HookInstance;
#endif

/**
 * Owns a worker. Optimization around ensurePath to prevent a Worker from
 * being constructed when it's not needed.
 */
class LocalBuilder : public Builder
{
public:
    LocalBuilder(ref<Store> store, ref<Store> evalStore)
        : store(store)
        , evalStore(evalStore) {};

    /* Builder interface — see `Builder` for documentation. */

    void buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;
    std::vector<KeyedBuildResult>
    buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;
    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode) override;
    void ensurePath(const StorePath & path) override;
    void repairPath(const StorePath & path) override;

private:
    /**
     * Intentionally construct a new worker for each operation, to avoid
     * reusing a worker between calls, allowing for thread safety.
     */
    inline std::shared_ptr<Worker> getWorker()
    {
        return std::make_shared<Worker>(*store, *evalStore);
    }

    ref<Store> store;
    ref<Store> evalStore;
};

/**
 * Coordinates one or more realisations and their interdependencies.
 */
class Worker : public Builder
{
private:

    /* Note: the worker should only have strong pointers to the
       top-level goals. */

    /**
     * The top-level goals of the worker.
     */
    Goals topGoals;

    /**
     * Goals that are ready to do some work.
     */
    WeakGoals awake;

    /**
     * Child processes currently running.
     */
    std::list<Child> children;

    /**
     * Number of build slots occupied.  This includes local builds but does not
     * include substitutions or remote builds via the build hook.
     */
    size_t nrLocalBuilds;

    /**
     * Number of substitution slots occupied.
     */
    size_t nrSubstitutions;

    /**
     * Maps used to prevent multiple instantiations of a goal for the
     * same derivation / path.
     */

    DerivedPathMap<std::map<OutputsSpec, std::weak_ptr<DerivationTrampolineGoal>>> derivationTrampolineGoals;

    std::map<StorePath, std::map<OutputName, std::weak_ptr<DerivationGoal>>> derivationGoals;
    std::map<StorePath, std::weak_ptr<DerivationResolutionGoal>> derivationResolutionGoals;
    std::map<StorePath, std::weak_ptr<DerivationBuildingGoal>> derivationBuildingGoals;
    std::map<StorePath, std::weak_ptr<PathSubstitutionGoal>> substitutionGoals;
    std::map<DrvOutput, std::weak_ptr<DrvOutputSubstitutionGoal>> drvOutputSubstitutionGoals;

    /**
     * Cache for pathContentsGood().
     */
    std::map<StorePath, bool> pathContentsGoodCache;

public:

    /**
     * The goals whose wakers woke. The waker of a goal enqueues the goal
     * here, from any thread, and wakes the task that runs the goals
     * (`runGoals`), which moves them to `awake`.
     */
    class WakeupQueue
    {
        /**
         * Queue of goals that need to be woken up.
         */
        Sync<std::queue<WeakGoalPtr>> wakeupQueue_;

        /**
         * The waker of the task that runs the goals, from its last poll.
         */
        Sync<nix::Waker> executor;

        friend class Worker;

        /**
         * Move the enqueued goals to `worker.awake`.
         */
        void wakeAll(Worker & worker);

        WakeupQueue() = default;

    public:
        void enqueue(WeakGoalPtr goal);
    };

private:

    /**
     * This is behind a ref, so that other threads can take a weak_ptr to it.
     */
    ref<WakeupQueue> wakerState;

public:

    /**
     * Goals waiting for a build slot.
     */
    WaitQueue wantingToBuild;

    /**
     * Goals waiting for a substitution slot.
     */
    WaitQueue wantingToSubstitute;

    /**
     * The number of goals that are suspended on something other than
     * another goal or a slot: the reactor (output of a child, a timer, a
     * transfer) or a thread. Only such a goal can make progress without
     * the help of another goal. When no goal is awake and this count is 0,
     * no goal can continue, and the worker reports a deadlock.
     */
    size_t nrGoalsSuspendedElsewhere = 0;

    const Activity act;
    const Activity actDerivations;
    const Activity actSubstitutions;

    /**
     * Tracks different types of build failures for exit status computation.
     */
    ExitStatusFlags exitStatusFlags;

#ifdef _WIN32
    AutoCloseFD ioport;
#endif

    Store & store;
    Store & evalStore;

    const WorkerSettings & settings;

    /**
     * Function to get the substituters to use for path substitution.
     *
     * Defaults to `getDefaultSubstituters`. This allows tests to
     * inject custom substituters.
     */
    fun<std::list<ref<Store>>()> getSubstituters;

#ifndef _WIN32 // TODO Enable building on Windows
    std::unique_ptr<HookInstance> hook;
#endif

    uint64_t expectedBuilds = 0;
    uint64_t doneBuilds = 0;
    uint64_t failedBuilds = 0;
    uint64_t runningBuilds = 0;

    uint64_t expectedSubstitutions = 0;
    uint64_t doneSubstitutions = 0;
    uint64_t failedSubstitutions = 0;
    uint64_t runningSubstitutions = 0;
    uint64_t expectedDownloadSize = 0;
    uint64_t doneDownloadSize = 0;
    uint64_t expectedNarSize = 0;
    uint64_t doneNarSize = 0;

    /**
     * Whether to ask the build hook if it can build a derivation. If
     * it answers with "decline-permanently", we don't try again.
     */
    bool tryBuildHook = true;

    Worker(Store & store, Store & evalStore);
    ~Worker();

    /**
     * Make a goal (with caching).
     */

    /**
     * @ref DerivationGoal "derivation goal"
     */
private:
    template<class G, typename... Args>
    std::shared_ptr<G> initGoalIfNeeded(std::weak_ptr<G> & goal_weak, Args &&... args);

    std::shared_ptr<DerivationTrampolineGoal> makeDerivationTrampolineGoal(
        ref<const SingleDerivedPath> drvReq, const OutputsSpec & wantedOutputs, BuildMode buildMode);

public:
    std::shared_ptr<DerivationTrampolineGoal> makeDerivationTrampolineGoal(
        const StorePath & drvPath, const OutputsSpec & wantedOutputs, const Derivation & drv, BuildMode buildMode);

    std::shared_ptr<DerivationGoal> makeDerivationGoal(
        const StorePath & drvPath,
        ref<const Derivation> drv,
        const OutputName & wantedOutput,
        BuildMode buildMode,
        bool storeDerivation);

    /**
     * @ref DerivationResolutionGoal "derivation resolution goal"
     */
    std::shared_ptr<DerivationResolutionGoal>
    makeDerivationResolutionGoal(const StorePath & drvPath, ref<const Derivation> drv, BuildMode buildMode);

    /**
     * @ref DerivationBuildingGoal "derivation building goal"
     */
    std::shared_ptr<DerivationBuildingGoal>
    makeDerivationBuildingGoal(const StorePath & drvPath, ref<const BasicDerivation> drv, BuildMode buildMode);

    /**
     * @ref PathSubstitutionGoal "substitution goal"
     */
    std::shared_ptr<PathSubstitutionGoal> makePathSubstitutionGoal(
        const StorePath & storePath, RepairFlag repair = NoRepair, std::optional<ContentAddress> ca = std::nullopt);
    std::shared_ptr<DrvOutputSubstitutionGoal> makeDrvOutputSubstitutionGoal(const DrvOutput & id);

    /**
     * Make a goal corresponding to the `DerivedPath`.
     *
     * It will be a `DerivationGoal` for a `DerivedPath::Built` or
     * a `PathSubstitutionGoal` for a `DerivedPath::Opaque`.
     */
    GoalPtr makeGoal(const DerivedPath & req, BuildMode buildMode = bmNormal);

    /**
     * Remove a dead goal.
     */
    void removeGoal(GoalPtr goal);

    /**
     * Wake up a goal (i.e., there is something for it to do).
     */
    void wakeUp(GoalPtr goal);

    /**
     * Get a weak reference to the wakeup queue. It can be used to safely enqueue Goals
     * for wakeup from other threads.
     */
    std::weak_ptr<WakeupQueue> getCrossThreadWaker();

    /**
     * Return the number of local build processes currently running (but not
     * remote builds via the build hook).
     */
    size_t getNrLocalBuilds();

    /**
     * Return the number of substitution processes currently running.
     */
    size_t getNrSubstitutions();

    /**
     * Registers a running child process.  `inBuildSlot` means that
     * the process counts towards the jobs limit.
     */
    void childStarted(
        GoalPtr goal,
        const std::set<MuxablePipePollState::CommChannel> & channels,
        bool inBuildSlot,
        bool respectTimeouts);

    /**
     * Unregisters a running child process.
     *
     * This overload requires `goal` to point to a fully constructed,
     * valid goal object, as it calls `goal->jobCategory()`.
     */
    void childTerminated(Goal * goal);

    /**
     * Unregisters a running child process, like the other overload.
     *
     * This overload only uses `goal` as a pointer for comparison with
     * weak goal references, so it is safe to call from destructors
     * where the goal object may be partially destroyed.
     */
    void childTerminated(Goal * goal, JobCategory jobCategory);

    /**
     * Whether a goal of `category` can take a build or substitution slot
     * now. `Goal::waitForBuildSlot` waits in `wantingToBuild` or
     * `wantingToSubstitute` if not.
     */
    bool slotAvailable(JobCategory category);

    /**
     * Run the goals until the specified top-level goals have finished,
     * as a task. The task is the executor of the goals: the goals run on
     * the thread that polls it, and their drops happen there.
     *
     * Goal code still makes blocking store calls, so the task must be
     * polled on a thread that may block, for example under
     * `runBlocking`, and never on the reactor thread. To await a build
     * from any task, use `buildPathsWithResultsAsync`.
     *
     * A user interrupt cancels the task, through a race with
     * `waitForInterrupt()`.
     */
    Task<void> runGoals(Goals topGoals);

    /**
     * `runGoals` on the current thread, blocking.
     */
    void run(const Goals & topGoals);

    /**
     * One poll of the executor of `runGoals`: run the goals that woke, in
     * the order of `CompareGoalPtrs`, until none is awake. Returns true
     * when the top-level goals have finished. Throws when no goal can
     * continue.
     */
    bool runRound();

    /**
     * Make goal wakes wake `waker`, the waker of the task that runs the
     * goals.
     */
    void setExecutorWaker(const nix::Waker & waker);

    /**
     * Wait until a child of `goal` has output, reaches EOF, or times out,
     * and give the event to `goal`. The reactor wakes the wait.
     */
    Task<void> waitForChild(Goal & goal);

    /**
     * Like `buildPathsWithResults`, as a task that runs the goals on the
     * thread that polls it (see `runGoals`). The goals live in the task:
     * dropping it cancels the build, kills and reaps the builders, and
     * releases the locks, on that thread.
     */
    Task<std::vector<KeyedBuildResult>> buildPathsWithResultsTask(std::vector<DerivedPath> reqs, BuildMode buildMode);

    /**
     * Like `buildPathsWithResultsTask`, on a thread of its own
     * (`runInThread`), so that any task can await it, also on the reactor
     * thread. Dropping the task cancels the build on that thread; the drop
     * waits for the thread, or defers the wait to the cleanup scope of the
     * run on the reactor. The worker must outlive the task and its
     * cleanup.
     */
    Task<std::vector<KeyedBuildResult>> buildPathsWithResultsAsync(std::vector<DerivedPath> reqs, BuildMode buildMode);

#ifdef _WIN32
    /**
     * Read the output of all children that is available now, and give it
     * to their goals. Windows has no `waitFd`, so `waitForChild` polls
     * with this.
     */
    void pollChildrenNow();
#endif

    /**
     * Check whether the given valid path exists and has the right
     * contents.
     */
    bool pathContentsGood(const StorePath & path);

    void markContentsGood(const StorePath & path);

    void updateProgress()
    {
        actDerivations.progress(doneBuilds, expectedBuilds + doneBuilds, runningBuilds, failedBuilds);
        actSubstitutions.progress(
            doneSubstitutions, expectedSubstitutions + doneSubstitutions, runningSubstitutions, failedSubstitutions);
        act.setExpected(actFileTransfer, expectedDownloadSize + doneDownloadSize);
        act.setExpected(actCopyPath, expectedNarSize + doneNarSize);
    }

    /* Builder interface — see `Builder` for documentation. */

    void buildPaths(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;
    std::vector<KeyedBuildResult>
    buildPathsWithResults(const std::vector<DerivedPath> & reqs, BuildMode buildMode) override;
    BuildResult buildDerivation(const StorePath & drvPath, const BasicDerivation & drv, BuildMode buildMode) override;
    void ensurePath(const StorePath & path) override;
    void repairPath(const StorePath & path) override;
};

} // namespace nix

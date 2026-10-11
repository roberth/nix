#pragma once
///@file

#include "nix/store/store-api.hh"
#include "nix/store/build-result.hh"
#include "nix/util/task.hh"

#include <list>
#include <queue>
#include <variant>

namespace nix {

class TimedOut final : public CloneableError<TimedOut, BuildError>
{
    void anchor() override;

public:
    time_t maxDuration;

    TimedOut(time_t maxDuration);
};

/**
 * Forward definition.
 */
struct Goal;
class Worker;

/**
 * A pointer to a goal.
 */
typedef std::shared_ptr<Goal> GoalPtr;
typedef std::weak_ptr<Goal> WeakGoalPtr;

struct CompareGoalPtrs
{
    bool operator()(const GoalPtr & a, const GoalPtr & b) const;
};

/**
 * Set of goals.
 */
typedef std::set<GoalPtr, CompareGoalPtrs> Goals;
typedef std::set<WeakGoalPtr, std::owner_less<WeakGoalPtr>> WeakGoals;

/**
 * A map of paths to goals (and the other way around).
 */
typedef std::map<StorePath, WeakGoalPtr> WeakGoalMap;

/**
 * Used as a hint to the worker on how to schedule a particular goal. For example,
 * builds are typically CPU- and memory-bound, while substitutions are I/O bound.
 * Using this information, the worker might decide to schedule more or fewer goals
 * of each category in parallel.
 */
enum struct JobCategory {
    /**
     * A build of a derivation; it will use CPU and disk resources.
     */
    Build,
    /**
     * A substitution an arbitrary store object; it will use network resources.
     */
    Substitution,
    /**
     * A goal that does no "real" work by itself, and just exists to depend on
     * other goals which *do* do real work. These goals therefore are not
     * limited.
     *
     * These goals cannot infinitely create themselves, so there is no risk of
     * a "fork bomb" type situation (which would be a problem even though the
     * goal do no real work) either.
     */
    Administration,
};

/**
 * The wakers of tasks that wait for the same event, in the order in which
 * they started to wait.
 *
 * An `Entry` lives in the leaf that waits, and leaves the queue when it is
 * destroyed. A cancelled task therefore does not take the turn of a task
 * that still waits.
 */
class WaitQueue
{
public:
    class Entry
    {
        friend class WaitQueue;

        WaitQueue * queue = nullptr;
        std::list<Entry *>::iterator pos;
        Waker waker;
        bool woken = false;

    public:
        Entry() = default;
        Entry(const Entry &) = delete;
        Entry & operator=(const Entry &) = delete;
        ~Entry();

        /**
         * Whether the queue woke this entry.
         */
        bool isWoken() const noexcept
        {
            return woken;
        }

        /**
         * Wait in `queue`. If the entry waits already, only replace its
         * waker.
         */
        void wait(WaitQueue & queue, const Waker & waker);
    };

    WaitQueue() = default;
    WaitQueue(const WaitQueue &) = delete;
    WaitQueue & operator=(const WaitQueue &) = delete;
    ~WaitQueue();

    bool empty() const noexcept
    {
        return entries.empty();
    }

    /**
     * Wake the entry that waits longest, and remove it from the queue.
     * Returns false if the queue is empty.
     */
    bool wakeOne();

    /**
     * Wake all entries, and empty the queue.
     */
    void wakeAll();

private:
    std::list<Entry *> entries;
};

/**
 * A goal is one unit of work of the `Worker`: a substitution, a build, or
 * a goal that only waits for other goals.
 *
 * The work is a `Task<ExitCode>`, and the `Worker` is its executor: it
 * polls the task when the task's waker wakes. Goal code is ordinary task
 * code. It awaits other tasks, for example `Store::queryPathInfoAsync`,
 * and the leaves below, which the `Worker` wakes:
 *
 * - `await`: other goals. A goal is shared: several goals can wait for
 *   it, and the `Worker` deduplicates goals by store path. Waiting for a
 *   goal therefore holds a `GoalPtr`, not the goal's task. The last
 *   `GoalPtr` to go cancels the goal, as dropping a task does.
 * - `waitForBuildSlot`, `yield`: scheduling.
 *
 * Two waits go to the reactor instead (`nix/util/reactor.hh`):
 * `waitForChildEvent` (output, EOF or a timeout of a child process,
 * through `waitFd` and `sleepFor`) and `waitForAWhile` (`sleepFor`).
 */
struct Goal : public std::enable_shared_from_this<Goal>
{
private:
    /* VTable anchor to avoid weak linkage of the vtable - it breaks
       dynamic_cast across shared libraries on Darwin. */
    virtual void anchor();
public:
    typedef enum { ecSuccess, ecFailed, ecNoSubstituters } ExitCode;

    /**
     * Event types for child process communication, delivered via coroutines.
     */
    struct ChildOutput
    {
        Descriptor fd;
        std::string data;
    };

    struct ChildEOF
    {
        Descriptor fd;
    };

    using ChildEvent = std::variant<ChildOutput, ChildEOF, std::unique_ptr<TimedOut>>;

private:
    class ChildEvents
    {
        /**
         * Structured queue of child events:
         * - outputs: stream of data from child
         * - eof: optional end-of-stream marker
         * - timeout: optional timeout that flushes/overrides other events
         */
        std::queue<ChildOutput> childOutputs;
        std::optional<ChildEOF> childEOF;
        std::unique_ptr<TimedOut> childTimeout;

    public:
        void pushChildEvent(ChildOutput event);
        void pushChildEvent(ChildEOF event);
        void pushChildEvent(TimedOut event);
        bool hasChildEvent() const;
        ChildEvent popChildEvent();
    };

    /**
     * One `await` call: the goals that it still waits for. The leaf of the
     * call owns it, and the goals that it waits for refer to it weakly.
     */
    struct WaiteeSet;

    /**
     * The `await` calls that wait for this goal.
     */
    std::vector<std::weak_ptr<WaiteeSet>> waiters;

    /**
     * Memoised result of key().
     */
    std::optional<std::string> cachedKey;

    ChildEvents childEvents;

    /**
     * The work of the goal. The `Worker` polls it with `waker`.
     */
    Task<ExitCode> task;

    /**
     * Wakes this goal through the `Worker`, from any thread. Created on
     * the first poll, because it refers to the goal weakly.
     */
    Waker waker;

    /**
     * Whether the task suspended on a leaf that the `Worker` wakes, in
     * the current poll. See `Worker::nrGoalsSuspendedElsewhere`.
     */
    bool suspendedOnWorker = false;

    /**
     * Whether this goal counts in `Worker::nrGoalsSuspendedElsewhere`.
     */
    bool suspendedElsewhere = false;

    void removeWaiter(const WaiteeSet * set);

    void finish(ExitCode result);

public:

    /**
     * Backlink to the worker.
     */
    Worker & worker;

    /**
     * Number of goals we are/were waiting for that have failed.
     */
    size_t nrFailed = 0;

    /**
     * Number of substitution goals we are/were waiting for that
     * failed because there are no substituters.
     */
    size_t nrNoSubstituters = 0;

    /**
     * Name of this goal for debugging purposes.
     */
    std::string name;

    /**
     * How the goal finished, once it has: set exactly once, by @ref
     * work. Not set while the goal is still running, or if it never
     * ran or was cancelled.
     */
    std::optional<ExitCode> exitCode;

    /**
     * Build result.
     */
    BuildResult buildResult;

    virtual void cleanup() {}

    /**
     * Hack to say that this goal should not log the failure, but instead keep
     * it around. Set by a waitee which sees itself as the designated
     * continuation of this goal, responsible for reporting its
     * successes or failures.
     *
     * @todo this is yet another not-nice hack in the goal system that
     * we ought to get rid of. See #11927
     */
    bool preserveFailure = false;

    /**
     * @param init The work of the goal. It does not run until the
     * `Worker` polls it.
     */
    Goal(Worker & worker, Task<ExitCode> init);

    virtual ~Goal();

    /**
     * Poll the goal's task. If it completed, finish the goal: record the
     * exit code, notify the goals that wait for it, and remove it from
     * the worker.
     */
    void work();

    /**
     * Called by the worker when data is received from a child process.
     * Stores the event for `waitForChildEvent`.
     */
    void handleChildOutput(Descriptor fd, std::string_view data);

    /**
     * Called by the worker when EOF is received from a child process.
     * Stores the event for `waitForChildEvent`.
     */
    void handleEOF(Descriptor fd);

    /**
     * Called by the worker when a build times out.
     * Stores the event for `waitForChildEvent`.
     */
    void timedOut(TimedOut && ex);

    void trace(std::string_view s);

    std::string getName() const
    {
        return name;
    }

    /**
     * Used for comparisons. The order matters a bit for scheduling. We
     * want:
     *
     * 1. Substitution
     * 2. Derivation administrativia
     * 3. Actual building
     *
     * Also, ensure that derivations get processed in order of their
     * name, i.e. a derivation named "aardvark" always comes before
     * "baboon".
     */
    virtual std::string key() = 0;

    /**
     * Memoising variant of key(). We really don't want to pay the overhead of
     * allocating strings just to compare Goals.
     */
    std::string_view keyCached() &
    {
        if (cachedKey)
            return *cachedKey;
        return *(cachedKey = key());
    }

    /**
     * @brief Hint for the scheduler, which concurrency limit applies.
     * @see JobCategory
     */
    virtual JobCategory jobCategory() const = 0;

protected:
    /**
     * Wait until the goals in `waitees` have finished, and count the
     * failures in `nrFailed` and `nrNoSubstituters`.
     *
     * Without `--keep-going`, the first failure ends the wait, and the
     * goals that did not finish yet are released. A goal that nothing
     * else holds is cancelled then.
     */
    Task<void> await(Goals waitees);

    /**
     * Wait for several seconds. Used for retrying goals blocked on
     * acquiring lockfiles.
     */
    Task<void> waitForAWhile();

    /**
     * Wait until a build or substitution slot (according to
     * `jobCategory()`) may be free. The caller checks again.
     */
    Task<void> waitForBuildSlot();

    /**
     * Let the other goals that are awake run first.
     */
    Task<void> yield();

    /**
     * The turn of a goal with the build hook. Destroying it ends the
     * turn, and wakes the next goal that waits for one.
     */
    class BuildHookTurn
    {
        Worker * worker;

    public:
        explicit BuildHookTurn(Worker & worker)
            : worker(&worker)
        {
        }

        BuildHookTurn(BuildHookTurn && other) noexcept
            : worker(std::exchange(other.worker, nullptr))
        {
        }

        BuildHookTurn & operator=(BuildHookTurn &&) = delete;

        ~BuildHookTurn();
    };

    /**
     * Wait until no other goal talks to the build hook, and take the
     * turn (`Worker::buildHookBusy`).
     */
    Task<BuildHookTurn> waitForBuildHook();

    /**
     * Wait for the next output, EOF or timeout of the child processes of
     * this goal.
     */
    Task<ChildEvent> waitForChildEvent();
};

void addToWeakGoals(WeakGoals & goals, GoalPtr p);

} // namespace nix

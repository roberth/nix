#pragma once
/**
 * @file
 *
 * Blocking code on its own thread, as a task.
 */

#include "nix/util/file-descriptor.hh"
#include "nix/util/reactor.hh"
#include "nix/util/task.hh"
#include "nix/util/signals.hh"

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <thread>
#include <variant>

namespace nix {

/**
 * Run the blocking function `fn` on a new thread, as a task.
 *
 * The thread starts on the first poll, not when the task is created. The
 * task completes with the result of `fn`, or with its exception.
 *
 * Destroying the task before it completes asks `fn` to stop, and then
 * waits for the thread to end. From then on, `checkInterrupt()` on that
 * thread throws `Interrupted`, so `fn` stops at its next check. On Unix,
 * the blocking reads and writes of `nix::read` and `nix::write` (and the
 * sources, sinks and functions that use them, such as `FdSource` and
 * `readFull`) wait for the descriptor and for the cancellation at once,
 * and throw `Interrupted` too. Any other blocking call that neither
 * checks nor registers with `onThreadCancelled` delays the destruction
 * until it returns. The thread also receives user interrupts, as
 * `ReceiveInterrupts` describes.
 *
 * The thread ends with `fn`. A process that `fn` starts with
 * `dieWithParent` (the default of `startProcess`) is killed then; use a
 * `BlockingPool` for such a function.
 *
 * Outside of a run on the reactor, the destruction blocks the thread that
 * destroys the task. That is acceptable for a thread that does nothing
 * else while it waits, such as the build `Worker`. In a run on the reactor
 * (`runOnReactor`), the wait is a cleanup of the run instead, so that the
 * reactor thread does not block; see "Asynchronous drop" in
 * `doc/manual/source/development/async.md`.
 */
template<typename T, typename F>
Task<T> runInThread(F fn);

namespace detail {

/**
 * The cancellation of one `runInThread` thread.
 */
struct ThreadCancellation
{
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::list<fun<void()>> callbacks;

#ifndef _WIN32
    /**
     * Its read side becomes readable (end of file) when the thread is
     * cancelled, so that a `poll` for a descriptor also wakes up on the
     * cancellation. Created by the first `descriptor()` call, because most
     * threads never wait for a descriptor.
     */
    Pipe pipe;

    /**
     * The read side of `pipe`.
     */
    Descriptor descriptor();
#endif

    /**
     * Set `cancelled`, close the write side of `pipe` and call the
     * callbacks.
     */
    void cancel();

    /**
     * Make `checkInterrupt()` and `onThreadCancelled()` on the current
     * thread refer to `cancellation`.
     */
    static void enter(std::shared_ptr<ThreadCancellation> cancellation);

    /**
     * Undo `enter`, for a thread of a `BlockingPool` that goes on with
     * another function.
     */
    static void leave();
};

} // namespace detail

/**
 * A callback that `onThreadCancelled` registered. Destroying it
 * unregisters the callback.
 */
class ThreadCancelledCallback
{
    std::shared_ptr<detail::ThreadCancellation> cancellation;
    std::list<fun<void()>>::iterator pos;

public:
    ThreadCancelledCallback() = default;

    ThreadCancelledCallback(
        std::shared_ptr<detail::ThreadCancellation> cancellation, std::list<fun<void()>>::iterator pos) noexcept
        : cancellation(std::move(cancellation))
        , pos(pos)
    {
    }

    ThreadCancelledCallback(ThreadCancelledCallback && other) noexcept
        : cancellation(std::move(other.cancellation))
        , pos(other.pos)
    {
    }

    ThreadCancelledCallback & operator=(ThreadCancelledCallback &&) = delete;
    ThreadCancelledCallback(const ThreadCancelledCallback &) = delete;
    ThreadCancelledCallback & operator=(const ThreadCancelledCallback &) = delete;

    ~ThreadCancelledCallback();
};

/**
 * Call `callback` when the task of `runInThread` that runs the current
 * thread is destroyed before it completes. The callback runs on the
 * thread that destroys the task, before that thread waits for this one.
 *
 * A blocking operation uses this to wake up, for example by cancelling
 * the transfer that it waits for. On a thread that `runInThread` does not
 * run, or after the returned object is destroyed, the callback is never
 * called. If the task is cancelled already, the callback runs at once.
 */
[[nodiscard]] ThreadCancelledCallback onThreadCancelled(fun<void()> callback);

namespace detail {

template<typename T>
struct ThreadState
{
    std::mutex mutex;
    std::variant<std::monostate, std::conditional_t<std::is_void_v<T>, std::monostate, T>, std::exception_ptr> result;
    Waker waker;
    std::shared_ptr<ThreadCancellation> cancellation = std::make_shared<ThreadCancellation>();
};

/**
 * Completes when the thread of `state` has stored its result, and is about
 * to end.
 */
template<typename T>
struct ThreadDoneLeaf : LeafAwaiter
{
    std::shared_ptr<ThreadState<T>> state;

    explicit ThreadDoneLeaf(std::shared_ptr<ThreadState<T>> state)
        : state(std::move(state))
    {
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        Waker old;
        std::lock_guard lock(state->mutex);
        if (state->result.index() != 0)
            return NIX_POLL_READY;
        replaceWaker(state->waker, waker, old);
        return NIX_POLL_PENDING;
    }

    void await_resume() noexcept {}
};

/**
 * Joins a thread when it is destroyed, also if the task that owns it is
 * dropped before the thread ended.
 */
struct JoinOnDestruction
{
    std::thread thread;

    ~JoinOnDestruction()
    {
        if (thread.joinable())
            thread.join();
    }
};

/**
 * The wait of a dropped `ThreadLeaf`, as a cleanup of the run on the
 * reactor.
 */
template<typename T>
Task<void> joinWhenDone(std::shared_ptr<ThreadState<T>> state, std::thread thread)
{
    JoinOnDestruction joiner{std::move(thread)};
    co_await ThreadDoneLeaf<T>(std::move(state));
    /* `joiner` joins the thread, which has stored its result and returns
       at once. */
}

template<typename T, typename F>
struct ThreadLeaf : LeafAwaiter
{
    F fn;
    std::shared_ptr<ThreadState<T>> state = std::make_shared<ThreadState<T>>();
    std::thread thread;
    bool started = false;

    /**
     * The cleanup scope of the run on the reactor that polled this leaf
     * first, if any.
     */
    std::optional<CleanupScope> scope;

    explicit ThreadLeaf(F fn)
        : fn(std::move(fn))
    {
    }

    ThreadLeaf(const ThreadLeaf &) = delete;

    ~ThreadLeaf()
    {
        if (thread.joinable()) {
            Waker old;
            {
                /* Nothing wakes the cancelled task. */
                std::lock_guard lock(state->mutex);
                old = std::move(state->waker);
            }
            state->cancellation->cancel();
            if (scope && *scope)
                scope->defer(joinWhenDone<T>(state, std::move(thread)));
            else
                thread.join();
        }
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (!started) {
            started = true;
            scope.emplace(CleanupScope::current());
            thread = std::thread([state = state, fn = std::move(fn)]() mutable {
                ThreadCancellation::enter(state->cancellation);
                ReceiveInterrupts receiveInterrupts;
                decltype(state->result) result;
                try {
                    if constexpr (std::is_void_v<T>) {
                        fn();
                        result.template emplace<1>();
                    } else
                        result.template emplace<1>(fn());
                } catch (...) {
                    result.template emplace<2>(std::current_exception());
                }
                Waker toWake;
                {
                    std::lock_guard lock(state->mutex);
                    state->result = std::move(result);
                    toWake = std::move(state->waker);
                }
                std::move(toWake).wake();
            });
        }

        Waker old;
        std::lock_guard lock(state->mutex);
        if (state->result.index() != 0)
            return NIX_POLL_READY;
        replaceWaker(state->waker, waker, old);
        return NIX_POLL_PENDING;
    }

    T await_resume()
    {
        /* The thread has stored its result, so it is about to end. */
        thread.join();
        if (auto * e = std::get_if<2>(&state->result))
            std::rethrow_exception(*e);
        if constexpr (!std::is_void_v<T>)
            return std::move(std::get<1>(state->result));
    }
};

} // namespace detail

template<typename T, typename F>
Task<T> runInThread(F fn)
{
    co_return co_await detail::ThreadLeaf<T, F>(std::move(fn));
}

namespace detail {

/**
 * A function that a `BlockingPool` runs, with its cancellation and its
 * completion.
 */
struct PoolJob
{
    std::shared_ptr<ThreadCancellation> cancellation = std::make_shared<ThreadCancellation>();
    std::mutex mutex;
    std::condition_variable doneCv;
    bool done = false;
    Waker waker;

    virtual ~PoolJob();

    /**
     * Run the function, store its result or its exception, and `finish`.
     */
    virtual void run() noexcept = 0;

    /**
     * Mark the job as done, and wake its task.
     */
    void finish() noexcept;
};

/**
 * Completes when a job is done.
 */
struct PoolJobDoneLeaf : LeafAwaiter
{
    std::shared_ptr<PoolJob> job;

    explicit PoolJobDoneLeaf(std::shared_ptr<PoolJob> job)
        : job(std::move(job))
    {
    }

    nix_poll pollLeaf(const Waker & waker) override;

    void await_resume() noexcept {}
};

} // namespace detail

/**
 * Threads that run blocking functions as tasks, like `runInThread`, and
 * that stay alive between the functions, until the pool is destroyed.
 *
 * Use it instead of `runInThread` for a function that starts a process
 * that must outlive the function. A child that `startProcess` starts with
 * `dieWithParent` gets `SIGKILL` when the *thread* that forked it ends
 * (`PR_SET_PDEATHSIG` on Linux), and the thread of `runInThread` ends with
 * its function. The pool also saves the start of a thread for each
 * function.
 *
 * A function has the same cancellation as under `runInThread`: dropping
 * its task before it completes makes `checkInterrupt()`, `nix::read` and
 * `nix::write` on its thread throw `Interrupted`, calls its
 * `onThreadCancelled` callbacks, and waits until the function returns, in
 * place or as a cleanup of the run on the reactor. A function that did not
 * start yet does not run.
 *
 * At most `maxThreads` functions run at once; the others wait, in the
 * order of their first poll. The destructor waits for the threads to end:
 * no task of the pool may exist any more.
 */
class BlockingPool
{
public:
    explicit BlockingPool(size_t maxThreads);
    BlockingPool(const BlockingPool &) = delete;
    BlockingPool & operator=(const BlockingPool &) = delete;
    ~BlockingPool();

    template<typename T, typename F>
    Task<T> run(F fn);

    struct State;

private:
    std::shared_ptr<State> state;

    template<typename T, typename F>
    struct Leaf;

    static void submit(const std::shared_ptr<State> & state, std::shared_ptr<detail::PoolJob> job);

    /**
     * Remove `job` from the queue, if no thread took it yet.
     */
    static bool withdraw(State & state, const detail::PoolJob * job);
};

template<typename T, typename F>
struct BlockingPool::Leaf : LeafAwaiter
{
    struct Job : detail::PoolJob
    {
        F fn;
        std::variant<std::monostate, std::conditional_t<std::is_void_v<T>, std::monostate, T>, std::exception_ptr>
            result;

        explicit Job(F fn)
            : fn(std::move(fn))
        {
        }

        void run() noexcept override
        {
            try {
                if constexpr (std::is_void_v<T>) {
                    fn();
                    result.template emplace<1>();
                } else
                    result.template emplace<1>(fn());
            } catch (...) {
                result.template emplace<2>(std::current_exception());
            }
            finish();
        }
    };

    std::shared_ptr<State> pool;
    std::shared_ptr<Job> job;
    bool submitted = false;
    std::optional<CleanupScope> scope;

    Leaf(std::shared_ptr<State> pool, F fn)
        : pool(std::move(pool))
        , job(std::make_shared<Job>(std::move(fn)))
    {
    }

    Leaf(const Leaf &) = delete;

    ~Leaf()
    {
        if (!submitted || withdraw(*pool, job.get()))
            return;
        Waker old;
        {
            std::lock_guard lock(job->mutex);
            if (job->done)
                return;
            /* Nothing wakes the cancelled task. */
            old = std::move(job->waker);
        }
        job->cancellation->cancel();
        if (scope && *scope)
            scope->defer([](std::shared_ptr<detail::PoolJob> job) -> Task<void> {
                co_await detail::PoolJobDoneLeaf(std::move(job));
            }(job));
        else {
            std::unique_lock lock(job->mutex);
            job->doneCv.wait(lock, [&] { return job->done; });
        }
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        {
            Waker old;
            std::lock_guard lock(job->mutex);
            if (job->done)
                return NIX_POLL_READY;
            detail::replaceWaker(job->waker, waker, old);
        }
        if (!submitted) {
            submitted = true;
            scope.emplace(CleanupScope::current());
            submit(pool, job);
        }
        return NIX_POLL_PENDING;
    }

    T await_resume()
    {
        if (auto * e = std::get_if<2>(&job->result))
            std::rethrow_exception(*e);
        if constexpr (!std::is_void_v<T>)
            return std::move(std::get<1>(job->result));
    }
};

template<typename T, typename F>
Task<T> BlockingPool::run(F fn)
{
    co_return co_await Leaf<T, F>(state, std::move(fn));
}

} // namespace nix

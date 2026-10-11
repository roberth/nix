#pragma once
/**
 * @file
 *
 * Blocking code on its own thread, as a task.
 */

#include "nix/util/reactor.hh"
#include "nix/util/task.hh"
#include "nix/util/signals.hh"

#include <atomic>
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
 * thread throws `Interrupted`, so `fn` stops at its next check. A blocking
 * call that neither checks nor registers with `onThreadCancelled` delays
 * the destruction until it returns. The thread also receives user
 * interrupts, as `ReceiveInterrupts` describes.
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

    /**
     * Set `cancelled` and call the callbacks.
     */
    void cancel();

    /**
     * Make `checkInterrupt()` and `onThreadCancelled()` on the current
     * thread refer to `cancellation`.
     */
    static void enter(std::shared_ptr<ThreadCancellation> cancellation);
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

} // namespace nix

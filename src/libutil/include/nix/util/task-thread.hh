#pragma once
/**
 * @file
 *
 * Blocking code on its own thread, as a task.
 */

#include "nix/util/task.hh"
#include "nix/util/signals.hh"

#include <atomic>
#include <mutex>
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
 * call that does not check delays the destruction until it returns. The
 * thread also receives user interrupts, as `ReceiveInterrupts` describes.
 *
 * The destruction blocks the thread that destroys the task. That is
 * acceptable for a thread that does nothing else while it waits, such as
 * the build `Worker`, but not for an executor thread that other tasks
 * share.
 */
template<typename T, typename F>
Task<T> runInThread(F fn);

namespace detail {

template<typename T>
struct ThreadState
{
    std::mutex mutex;
    std::variant<std::monostate, std::conditional_t<std::is_void_v<T>, std::monostate, T>, std::exception_ptr> result;
    Waker waker;
    std::atomic<bool> cancelled{false};
};

template<typename T, typename F>
struct ThreadLeaf : LeafAwaiter
{
    F fn;
    std::shared_ptr<ThreadState<T>> state = std::make_shared<ThreadState<T>>();
    std::thread thread;
    bool started = false;

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
            state->cancelled = true;
            thread.join();
        }
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (!started) {
            started = true;
            thread = std::thread([state = state, fn = std::move(fn)]() mutable {
#ifndef _WIN32
                unix::interruptCheck = [state]() { return state->cancelled.load(); };
#endif
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

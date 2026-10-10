#pragma once
/**
 * @file
 *
 * Adapters between `nix::Task` and Boost.Asio.
 *
 * - `asyncRunTask` runs a task as an Asio asynchronous operation. The
 *   task is polled on the handler's executor. An Asio cancellation
 *   destroys the task, which cancels it.
 * - `taskToAwaitable` is `asyncRunTask` with `use_awaitable`.
 * - `awaitableToTask` awaits an Asio awaitable from a task. Destroying the
 *   task emits a terminal cancellation to the awaitable.
 */

#include "nix/util/task.hh"
#include "nix/util/signals.hh"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace nix {

namespace asio = boost::asio;

namespace detail {

template<typename T>
using AsioTaskSignature =
    std::conditional_t<std::is_void_v<T>, void(std::exception_ptr), void(std::exception_ptr, std::optional<T>)>;

/**
 * Polls a task on an Asio executor until it completes, then invokes the
 * completion handler. It is its own waker: waking it posts a poll to the
 * executor.
 *
 * All polls, the cancellation and the completion run on the executor, so
 * the task never runs on two threads at once.
 */
template<typename T, typename Handler>
struct AsioTaskRunner : WakeTarget
{
    using Executor = asio::associated_executor_t<Handler>;

    Executor executor;
    Task<T> task;
    std::optional<Handler> handler;
    std::atomic<bool> scheduled{false};

    /**
     * The task can wait for a wake from another thread, for example the
     * curl thread. Keep the `io_context` running until then.
     */
    asio::executor_work_guard<asio::io_context::executor_type> workGuard;

    AsioTaskRunner(Task<T> task, Handler handler)
        : executor(asio::get_associated_executor(handler))
        , task(std::move(task))
        , handler(std::move(handler))
        , workGuard(
              asio::make_work_guard(static_cast<asio::io_context &>(asio::query(executor, asio::execution::context))))
    {
    }

    void start()
    {
        auto slot = asio::get_associated_cancellation_slot(*handler);
        if (slot.is_connected()) {
            addRef();
            slot.assign([self = WakeTargetRef<AsioTaskRunner>(this)](asio::cancellation_type) {
                asio::post(self->executor, [self]() { self->cancel(); });
            });
        }
        wake();
    }

    void wake() noexcept override
    {
        if (scheduled.exchange(true))
            return;
        addRef();
        asio::post(executor, [self = WakeTargetRef<AsioTaskRunner>(this)]() {
            self->scheduled = false;
            self->step();
        });
    }

    void step()
    {
        if (!handler)
            return;
        if (task.poll(waker()) == NIX_POLL_PENDING)
            return;
        std::exception_ptr ex;
        if constexpr (std::is_void_v<T>) {
            try {
                task.takeResult();
            } catch (...) {
                ex = std::current_exception();
            }
            task.reset();
            complete(ex);
        } else {
            std::optional<T> value;
            try {
                value.emplace(task.takeResult());
            } catch (...) {
                ex = std::current_exception();
            }
            task.reset();
            complete(ex, std::move(value));
        }
    }

    void cancel()
    {
        if (!handler)
            return;
        /* Destroying the task cancels it. */
        task.reset();
        auto ex = std::make_exception_ptr(Cancelled("task was cancelled"));
        if constexpr (std::is_void_v<T>)
            complete(ex);
        else
            complete(ex, std::nullopt);
    }

    template<typename... Args>
    void complete(Args &&... args)
    {
        auto h = std::move(*handler);
        handler.reset();
        /* Breaks the reference cycle through the slot handler. */
        asio::get_associated_cancellation_slot(h).clear();
        workGuard.reset();
        std::move(h)(std::forward<Args>(args)...);
    }
};

} // namespace detail

template<typename T, typename CompletionToken>
auto asyncRunTask(Task<T> task, CompletionToken && token)
{
    return asio::async_initiate<CompletionToken, detail::AsioTaskSignature<T>>(
        [](auto handler, Task<T> task) {
            auto runner =
                makeWakeTarget<detail::AsioTaskRunner<T, decltype(handler)>>(std::move(task), std::move(handler));
            runner->start();
        },
        token,
        std::move(task));
}

/**
 * Await a task from an Asio coroutine.
 */
template<typename T>
asio::awaitable<T> taskToAwaitable(Task<T> task)
{
    if constexpr (std::is_void_v<T>)
        co_await asyncRunTask(std::move(task), asio::use_awaitable);
    else
        co_return std::move(*co_await asyncRunTask(std::move(task), asio::use_awaitable));
}

namespace detail {

template<typename T>
asio::awaitable<std::monostate> voidToMonostate(asio::awaitable<T> aw)
{
    co_await std::move(aw);
    co_return std::monostate{};
}

template<typename T, typename Executor>
Task<T> awaitableToTaskNonVoid(Executor executor, asio::awaitable<T> aw)
{
    auto signal = std::make_shared<asio::cancellation_signal>();
    co_return co_await callbackToTask<T>([executor, signal, aw = std::move(aw)](Callback<T> callback) mutable {
        auto cb = std::make_shared<Callback<T>>(std::move(callback));
        /* The completion handler owns the signal: the spawned
           coroutine refers to its slot until it completes, which can
           be long after the task is gone. */
        asio::co_spawn(
            executor,
            std::move(aw),
            asio::bind_cancellation_slot(signal->slot(), [cb, signal](std::exception_ptr ex, T value) {
                if (ex)
                    cb->rethrow(ex);
                else
                    (*cb)(std::move(value));
            }));
        /* The canceller. The signal must be emitted on the executor. */
        return [executor, signal]() {
            asio::post(executor, [signal]() { signal->emit(asio::cancellation_type::terminal); });
        };
    });
}

} // namespace detail

/**
 * Await an Asio awaitable from a task, by spawning it on `executor`.
 * The value type must be default-constructible, as `co_spawn` requires.
 */
template<typename T, typename Executor>
Task<T> awaitableToTask(Executor executor, asio::awaitable<T> aw)
{
    if constexpr (std::is_void_v<T>)
        co_await detail::awaitableToTaskNonVoid(executor, detail::voidToMonostate(std::move(aw)));
    else
        co_return co_await detail::awaitableToTaskNonVoid(executor, std::move(aw));
}

} // namespace nix

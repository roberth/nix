#include <gtest/gtest.h>

#include "nix/util/task-asio.hh"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <thread>

namespace nix {

namespace {

using Log = std::vector<std::string>;

struct Guard
{
    Log & log;
    std::string name;

    ~Guard()
    {
        log.push_back(name + " destroyed");
    }
};

/**
 * A callback-based operation that the test completes by hand.
 */
struct PendingOp
{
    std::optional<Callback<int>> callback;
    bool cancelled = false;

    Task<int> start()
    {
        return callbackToTask<int>([this](Callback<int> cb) {
            callback.emplace(std::move(cb));
            return [this]() { cancelled = true; };
        });
    }
};

} // namespace

TEST(TaskAsio, awaitTaskFromAsio)
{
    asio::io_context ctx;
    std::thread thread;
    std::optional<int> result;

    auto task = callbackToTask<int>(
        [&](Callback<int> cb) { thread = std::thread([cb = std::move(cb)]() mutable { cb(21); }); });

    asio::co_spawn(
        ctx,
        [&]() -> asio::awaitable<void> { result = 2 * co_await taskToAwaitable(std::move(task)); },
        [](std::exception_ptr ex) {
            if (ex)
                std::rethrow_exception(ex);
        });

    /* `run` must not return while the task waits for the other thread. */
    ctx.run();
    thread.join();
    EXPECT_EQ(result, 42);
}

TEST(TaskAsio, asioCancellationDestroysTask)
{
    asio::io_context ctx;
    Log log;
    PendingOp op;

    auto task = [](Log & log, PendingOp & op) -> Task<int> {
        Guard guard{log, "task local"};
        co_return co_await op.start();
    }(log, op);

    asio::cancellation_signal signal;
    std::exception_ptr outcome;
    bool completed = false;

    asyncRunTask(
        std::move(task),
        asio::bind_cancellation_slot(
            signal.slot(), asio::bind_executor(ctx, [&](std::exception_ptr ex, std::optional<int> value) {
                completed = true;
                outcome = ex;
                log.push_back(value ? "value" : "no value");
            })));

    ctx.poll();
    EXPECT_TRUE(op.callback);
    EXPECT_FALSE(completed);

    signal.emit(asio::cancellation_type::terminal);
    ctx.poll();

    EXPECT_TRUE(completed);
    EXPECT_TRUE(op.cancelled);
    EXPECT_EQ(log, (Log{"task local destroyed", "no value"}));
    EXPECT_THROW(std::rethrow_exception(outcome), Cancelled);

    /* The operation completes late; nothing resumes. */
    (*op.callback)(1);
    ctx.poll();
    EXPECT_EQ(log, (Log{"task local destroyed", "no value"}));
}

TEST(TaskAsio, awaitAsioFromTask)
{
    asio::io_context ctx;
    std::optional<int> result;

    auto task = [](asio::io_context & ctx) -> Task<int> {
        auto v = co_await awaitableToTask(ctx.get_executor(), []() -> asio::awaitable<int> {
            co_await asio::post(asio::use_awaitable);
            co_return 6;
        }());
        co_return v + 1;
    }(ctx);

    asyncRunTask(std::move(task), asio::bind_executor(ctx, [&](std::exception_ptr ex, std::optional<int> value) {
                     if (ex)
                         std::rethrow_exception(ex);
                     result = value;
                 }));
    ctx.run();
    EXPECT_EQ(result, 7);
}

TEST(TaskAsio, droppingTaskCancelsAwaitable)
{
    asio::io_context ctx;
    Log log;

    auto task = [](Log & log, asio::io_context & ctx) -> Task<void> {
        Guard guard{log, "task local"};
        co_await awaitableToTask(ctx.get_executor(), [](Log & log) -> asio::awaitable<void> {
            asio::steady_timer timer(co_await asio::this_coro::executor, std::chrono::hours(1));
            try {
                co_await timer.async_wait(asio::use_awaitable);
                log.push_back("timer fired");
            } catch (boost::system::system_error & e) {
                log.push_back(e.code() == asio::error::operation_aborted ? "timer aborted" : "timer error");
                throw;
            }
        }(log));
    }(log, ctx);

    asio::cancellation_signal signal;
    asyncRunTask(
        std::move(task), asio::bind_cancellation_slot(signal.slot(), asio::bind_executor(ctx, [&](std::exception_ptr) {
                                                          log.push_back("task completion handler");
                                                      })));

    ctx.poll();
    EXPECT_EQ(log, Log{});

    signal.emit(asio::cancellation_type::terminal);

    /* `run` returns: the timer is gone, so no work is left. */
    ctx.run();
    EXPECT_EQ(log, (Log{"task local destroyed", "task completion handler", "timer aborted"}));
}

} // namespace nix

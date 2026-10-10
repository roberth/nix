#include <gtest/gtest.h>

#include "nix/util/error.hh"
#include "nix/util/signals.hh"
#include "nix/util/task.hh"
#include "nix/util/task-asio.hh"
#include "nix/rust/future_probe.rs.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/io_context.hpp>

#include <cerrno>
#include <chrono>
#include <mutex>
#include <thread>

namespace nix {

/* These tests drive Rust futures from C++ tasks and C++ tasks from Rust
   futures, through the C ABI in `nix/rust/future-abi.h`. The Rust side is
   `src/librust/src/future_probe.rs`. */

namespace {

/**
 * The event log that the probe futures write to. Names in this file are
 * unique within the test program, because the unity build puts several
 * test files into one translation unit.
 */
struct RustFutureLog : rs::ProbeLog
{
    mutable std::mutex mutex;
    mutable std::vector<std::string> events;

    void push(rust::Str event) const override
    {
        std::lock_guard lock(mutex);
        events.emplace_back(event);
    }

    void add(std::string event) const
    {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    std::vector<std::string> get() const
    {
        std::lock_guard lock(mutex);
        return events;
    }
};

struct RustFutureWakeCounter : WakeTarget
{
    std::atomic<int> count = 0;

    void wake() noexcept override
    {
        count++;
    }
};

/**
 * Logs its destruction, which shows where a cancellation reached.
 */
struct RustFutureFrameGuard
{
    const RustFutureLog & log;
    std::string name;

    ~RustFutureFrameGuard()
    {
        log.add(name + " destroyed");
    }
};

/**
 * A C ABI waker that counts what Rust does with it.
 */
struct RustFutureCWakerCounts
{
    std::atomic<int> clones = 0;
    std::atomic<int> wakes = 0;
    std::atomic<int> drops = 0;
};

extern "C" {
static nix_waker rustFutureCWakerClone(const void * data);

static void rustFutureCWakerWake(const void * data)
{
    auto * counts = static_cast<RustFutureCWakerCounts *>(const_cast<void *>(data));
    counts->wakes++;
    counts->drops++;
}

static void rustFutureCWakerWakeByRef(const void * data)
{
    static_cast<RustFutureCWakerCounts *>(const_cast<void *>(data))->wakes++;
}

static void rustFutureCWakerDrop(const void * data)
{
    static_cast<RustFutureCWakerCounts *>(const_cast<void *>(data))->drops++;
}

static const nix_waker_vtable rustFutureCWakerVtable = {
    .clone = rustFutureCWakerClone,
    .wake = rustFutureCWakerWake,
    .wake_by_ref = rustFutureCWakerWakeByRef,
    .drop = rustFutureCWakerDrop,
};

static nix_waker rustFutureCWakerClone(const void * data)
{
    static_cast<RustFutureCWakerCounts *>(const_cast<void *>(data))->clones++;
    return nix_waker{data, &rustFutureCWakerVtable};
}
}

Task<rust::String> cxxString(std::string value)
{
    co_return rust::String(value);
}

} // namespace

TEST(RustFuture, valueFromRust)
{
    EXPECT_EQ(std::string(runBlocking(awaitCFuture<rust::String>(rs::probe_future_ready("from Rust")))), "from Rust");
}

TEST(RustFuture, rustErrorBecomesNixError)
{
    try {
        runBlocking(awaitCFuture<void>(rs::probe_future_error("no such thing")));
        FAIL() << "the future did not fail";
    } catch (const Error & e) {
        EXPECT_EQ(e.info().msg.str(), "no such thing");
    }
}

TEST(RustFuture, rustPanicBecomesNixError)
{
    try {
        runBlocking(awaitCFuture<void>(rs::probe_future_panic("probe panic")));
        FAIL() << "the future did not fail";
    } catch (const Error & e) {
        EXPECT_EQ(e.info().msg.str(), "Rust panic: probe panic");
    }
}

TEST(RustFuture, cxxValueThroughRust)
{
    RustFutureLog log;
    auto future = rs::probe_future_forward(toCFuture(cxxString("from C++")), log);
    EXPECT_EQ(std::string(runBlocking(awaitCFuture<rust::String>(future))), "Rust saw: from C++");
    /* The Rust future is dropped as soon as it completes. */
    EXPECT_EQ(log.get(), std::vector<std::string>{"rust: dropped"});
}

TEST(RustFuture, cxxExceptionKeepsItsTypeThroughRust)
{
    RustFutureLog log;
    auto inner = []() -> Task<rust::String> {
        throw SysError(ENOENT, "opening a file");
        co_return rust::String();
    }();
    auto future = rs::probe_future_forward(toCFuture(std::move(inner)), log);
    try {
        runBlocking(awaitCFuture<rust::String>(future));
        FAIL() << "the future did not fail";
    } catch (const SysError & e) {
        EXPECT_EQ(e.errNo, ENOENT);
    }
}

TEST(RustFuture, droppingTheTaskDropsTheRustFuture)
{
    RustFutureLog log;
    auto task = awaitCFuture<void>(rs::probe_future_pending(log));

    auto counter = makeWakeTarget<RustFutureWakeCounter>();
    EXPECT_EQ(task.poll(counter->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(log.get(), std::vector<std::string>{"rust: polled"});

    task.reset();
    EXPECT_EQ(log.get(), (std::vector<std::string>{"rust: polled", "rust: dropped"}));
}

TEST(RustFuture, droppingTheRustFutureCancelsTheCxxTask)
{
    RustFutureLog log;

    /* A C++ operation that never completes, and logs its cancellation. */
    auto inner = [](const RustFutureLog & log) -> Task<rust::String> {
        RustFutureFrameGuard guard{log, "C++ frame"};
        co_return co_await callbackToTask<rust::String>(
            [&log](Callback<rust::String>) { return [&log]() { log.add("C++ operation cancelled"); }; });
    }(log);

    auto task = awaitCFuture<rust::String>(rs::probe_future_forward(toCFuture(std::move(inner)), log));

    auto counter = makeWakeTarget<RustFutureWakeCounter>();
    EXPECT_EQ(task.poll(counter->waker()), NIX_POLL_PENDING);
    EXPECT_EQ(log.get(), std::vector<std::string>{});

    /* C++ drops the Rust future, which drops the C++ task, whose frame
       destroys the leaf, which cancels the operation. */
    task.reset();
    EXPECT_EQ(log.get(), (std::vector<std::string>{"C++ operation cancelled", "C++ frame destroyed", "rust: dropped"}));
    EXPECT_EQ(counter->count, 0);
}

#ifndef _WIN32

TEST(RustFuture, interruptDropsARustFutureThatAnotherThreadWakes)
{
    RustFutureLog log;
    auto pollsBefore = rs::probe_ticking_polls();

    /* Interrupt once the Rust thread woke the future a few times. */
    std::thread interrupter([&]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (rs::probe_ticking_polls() < pollsBefore + 4 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        unix::triggerInterrupt();
    });

    bool interrupted = false;
    try {
        runBlocking(awaitCFuture<void>(rs::probe_future_ticking(log, 2)));
    } catch (const Interrupted &) {
        interrupted = true;
    }
    interrupter.join();
    setInterrupted(false);

    EXPECT_TRUE(interrupted);
    EXPECT_GE(rs::probe_ticking_polls(), pollsBefore + 4);
    /* `rust: dropped` comes after the Rust thread stopped. */
    EXPECT_EQ(log.get(), (std::vector<std::string>{"rust: started", "rust: dropped"}));
}

#endif

TEST(RustFuture, rustThreadWakesAnAsioTaskAlsoAfterItCompleted)
{
    std::optional<std::string> result;
    {
        asio::io_context ctx;
        asyncRunTask(
            awaitCFuture<rust::String>(rs::probe_future_woken_from_thread(20, 50)),
            asio::bind_executor(ctx, [&](std::exception_ptr ex, std::optional<rust::String> value) {
                if (ex)
                    std::rethrow_exception(ex);
                result = std::string(*value);
            }));
        /* `run` returns when the task completed. The Rust thread still
           holds a waker of the task, and wakes it once more in 50 ms. */
        ctx.run();
    }
    /* The executor is gone now. The late wake must find nothing to do. */
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rs::probe_wake_threads_running() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    EXPECT_EQ(result, "woken from a Rust thread");
    EXPECT_EQ(rs::probe_wake_threads_running(), 0u);
}

TEST(RustFuture, wakerOwnershipAcrossTheBoundary)
{
    RustFutureCWakerCounts counts;
    nix_waker waker{&counts, &rustFutureCWakerVtable};

    EXPECT_EQ(std::string(rs::probe_waker_roundtrip(waker)), "C++ waker unwrapped: true, Rust waker unwrapped: true");

    /* Rust cloned the borrowed waker three times, woke one clone by value
       and one by reference, and dropped the other two. */
    EXPECT_EQ((std::array<int, 3>{counts.clones, counts.wakes, counts.drops}), (std::array<int, 3>{3, 2, 3}));
}

} // namespace nix

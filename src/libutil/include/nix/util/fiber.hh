#pragma once
/**
 * @file
 *
 * Fibers: stackful coroutines on pooled stacks, and the bridge between
 * a fiber and a `Task`.
 *
 * The evaluator is recursive C++. It cannot become a chain of `Task`s
 * without a rewrite of every function between a primop and the top of
 * the evaluation. A fiber gives it a stack of its own instead, so that
 * the whole evaluation can suspend at one point deep in the recursion:
 *
 * - `runOnFiber(fn)` is a task. Its poll switches onto the stack of a
 *   fiber that runs `fn`, and returns when `fn` suspends or returns.
 * - `awaitOnFiber(task)`, called by `fn` (at any depth), polls `task` on
 *   the fiber. If the task is pending, the fiber suspends, and the poll
 *   of `runOnFiber` returns `NIX_POLL_PENDING`. The waker of that poll
 *   is the one that `task` gets, so a wake of `task` polls the
 *   `runOnFiber` task again, which resumes the fiber.
 *
 * See "Evaluator on fibers" in `doc/manual/source/development/async.md`.
 */

#include "nix/util/fun.hh"
#include "nix/util/task.hh"

#include <cstddef>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>

namespace nix {

/**
 * Hooks that let a conservative garbage collector scan the stacks of
 * suspended fibers. libutil has no garbage collector; the evaluator
 * (libexpr) installs these when it initialises the Boehm GC.
 *
 * The garbage collector keeps a current stack per thread, and a saved
 * stack pointer for each stack that a thread left. The hooks keep both
 * in step with the stack switches:
 *
 * - `registerStack(base, size)` is called when the pool allocates a
 *   stack. `base` is the high end. It returns a cookie for the stack.
 *   `unregisterStack(cookie)` is called before the stack is freed.
 * - `switchTo(cookie)` is called on the stack that the thread leaves, just
 *   before it resumes the fiber on the stack `cookie`. It records the
 *   stack pointer of the stack that the thread leaves, and makes `cookie`
 *   the current stack. It returns a handle of the stack that the thread
 *   leaves.
 * - `switchedBack(handle)` is called on that stack after the fiber
 *   suspended or ended. It makes `handle` the current stack again.
 * - `suspend(cookie)` is called on the fiber stack just before the fiber
 *   suspends. It records the stack pointer of the fiber stack.
 *   `resumed(cookie)` is called on the fiber stack when the fiber starts
 *   or continues. It clears the saved stack pointer.
 *
 * A stack of the pool keeps its registration while it is free. Its saved
 * stack pointer is clear, so the garbage collector does not scan it.
 */
struct FiberStackHooks
{
    void * (*registerStack)(void * base, size_t size);
    void (*unregisterStack)(void * cookie);
    void * (*switchTo)(void * cookie);
    void (*switchedBack)(void * handle);
    void (*suspend)(void * cookie);
    void (*resumed)(void * cookie);
};

/**
 * Install the hooks. Call this before the first fiber starts, from the
 * initialisation of the garbage collector. The hooks must stay valid
 * until the process exits.
 */
void setFiberStackHooks(const FiberStackHooks & hooks);

/**
 * Make `runOnFiber` run its function in place, without a fiber, and
 * `awaitOnFiber` block. The garbage collector calls this when it
 * cannot scan the stacks of suspended fibers.
 */
void disableFibers(std::string reason);

/**
 * Whether `runOnFiber` uses a fiber. If not, the reason.
 */
std::optional<std::string> fibersDisabledReason();

/**
 * The size of a fiber stack, including its guard page. The evaluator
 * runs on it, so it is as large as the stack that the evaluator asks
 * for on the main thread (60 MiB). Only the pages that the fiber
 * touches use memory.
 */
constexpr size_t fiberStackSize = 60 * 1024 * 1024;

/**
 * A stackful coroutine on a stack from the pool.
 *
 * `resume()` runs the body until it calls `Fiber::suspend()` or returns.
 * An exception that escapes the body is kept, and `resume()` returns as
 * if the body returned; see `exception()`.
 *
 * A fiber runs only on the thread that started it: code in the body may
 * keep the address of a `thread_local` across a suspension. A fiber must
 * not be destroyed while it is suspended, because nothing would unwind
 * its stack; the owner resumes it until it ends (see `runOnFiber`).
 */
class Fiber
{
public:
    struct Impl;

private:
    std::unique_ptr<Impl> impl;

public:
    explicit Fiber(fun<void()> body);
    ~Fiber();

    Fiber(const Fiber &) = delete;
    Fiber & operator=(const Fiber &) = delete;

    /**
     * Run the body until it suspends or ends. Returns true if it ended.
     */
    bool resume();

    bool finished() const noexcept;

    bool started() const noexcept;

    /**
     * The exception that escaped the body, if any.
     */
    std::exception_ptr exception() const noexcept;

    /**
     * Suspend the current fiber; return when it is resumed. Must be
     * called on a fiber, on the stack of that fiber.
     */
    static void suspend();

    /**
     * The fiber that the current thread runs, or null.
     */
    static Fiber * current() noexcept;
};

/**
 * Statistics of the stack pool of the process.
 */
struct FiberStackStats
{
    /**
     * Stacks that the pool allocated with `mmap` since the start.
     */
    size_t allocated = 0;
    /**
     * Stacks in use by a fiber now.
     */
    size_t inUse = 0;
    /**
     * Stacks in the pool, free for reuse.
     */
    size_t free = 0;
};

FiberStackStats fiberStackStats();

namespace detail {

/**
 * The state of one `runOnFiber` task, which the fiber sees.
 */
struct FiberRun
{
    /**
     * The waker of the poll that resumed the fiber.
     */
    const Waker * waker = nullptr;

    /**
     * Set when the task is dropped while the fiber is suspended.
     */
    bool cancelled = false;
};

/**
 * The run of the fiber that the current thread runs, or null.
 */
FiberRun * currentFiberRun() noexcept;

/**
 * On a fiber: poll with the waker of the run until `poll` returns
 * `NIX_POLL_READY`, and suspend the fiber after each pending poll.
 * Throws `Interrupted` when the run is cancelled, before or instead of a
 * poll.
 */
void awaitOnFiberImpl(FiberRun & run, fun<nix_poll(const Waker &)> poll);

Task<void> runOnFiberImpl(fun<void()> body);

} // namespace detail

/**
 * Run the blocking function `fn` on a fiber, as a task.
 *
 * The fiber starts on the first poll, on the thread that polls. Each poll
 * resumes it on that thread; a poll on another thread aborts. `fn` can
 * call `awaitOnFiber` to wait for a task without blocking the thread.
 *
 * The task completes with the result of `fn`, or with its exception, as
 * the same exception object.
 *
 * Destroying the task while the fiber is suspended in `awaitOnFiber`
 * cancels it: the fiber resumes, `awaitOnFiber` throws `Interrupted`
 * without polling again, and the stack unwinds, which runs the
 * destructors of the frames on it and drops the task that it awaited.
 * Then the stack returns to the pool. A `catch` in `fn` can stop the
 * unwinding, but a later `awaitOnFiber` throws again; the destruction
 * waits until `fn` returns.
 *
 * With fibers disabled (`fibersDisabledReason()`), the first poll runs
 * `fn` in place, and `awaitOnFiber` blocks.
 */
template<typename F>
Task<std::invoke_result_t<F>> runOnFiber(F fn)
{
    using T = std::invoke_result_t<F>;
    if constexpr (std::is_void_v<T>)
        co_await detail::runOnFiberImpl(std::move(fn));
    else {
        /* Both live in this frame, which owns the task that runs the
           fiber, so they outlive the fiber. */
        std::optional<T> result;
        co_await detail::runOnFiberImpl([&]() { result.emplace(fn()); });
        co_return std::move(*result);
    }
}

/**
 * Wait for `task` and return its result, or rethrow its exception.
 *
 * On a fiber of `runOnFiber`, this suspends the fiber while `task` is
 * pending, so the thread can run other tasks. Elsewhere, and while an
 * exception is handled or unwinds (in a `catch` block or a destructor),
 * it blocks the thread like `runBlocking`: the state of the C++ runtime
 * for exceptions is per thread, and another fiber on the same thread
 * would change it.
 */
template<typename T>
T awaitOnFiber(Task<T> task)
{
    auto * run = detail::currentFiberRun();
    if (!run || std::uncaught_exceptions() > 0 || std::current_exception())
        return runBlocking(std::move(task));
    detail::awaitOnFiberImpl(*run, [&](const Waker & waker) { return task.poll(waker); });
    return task.takeResult();
}

} // namespace nix

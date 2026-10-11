#pragma once
/**
 * @file
 *
 * The reactor: one tokio runtime for the process, with one thread
 * (`src/librust/src/reactor.rs`). See
 * `doc/manual/source/development/async.md`.
 *
 * - `runOnReactor` runs a task on the reactor thread and blocks the
 *   calling thread until it is done, like `runBlocking`.
 * - `sleepFor` and `waitFd` are tasks that the reactor wakes. Any thread
 *   can poll them.
 * - `postToReactor`, `ReactorTimer` and `FdWatch` run callbacks on the
 *   reactor thread, for C libraries that report the timers and descriptors
 *   that they need, such as the curl multi interface. All callbacks run on
 *   one thread, never at the same time.
 */

#include "nix/util/task.hh"

#include "nix/rust/reactor.hh"
#include "nix/rust/reactor.rs.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace nix {

/**
 * Whether the current thread is the reactor thread.
 */
bool onReactorThread();

namespace detail {

void runOnReactorImpl(Task<void> task);

} // namespace detail

/**
 * Run a task to completion on the reactor thread. The calling thread
 * blocks until the task is complete and the cleanups that its drops
 * deferred are done.
 *
 * A user interrupt cancels the task, waits for its cleanups, and throws
 * `Interrupted`, like `runBlocking`.
 *
 * Must not be called on the reactor thread, which would wait for itself.
 */
template<typename T>
T runOnReactor(Task<T> task)
{
    if constexpr (std::is_void_v<T>)
        detail::runOnReactorImpl(std::move(task));
    else {
        std::optional<T> result;
        detail::runOnReactorImpl([](Task<T> task, std::optional<T> & out) -> Task<void> {
            out.emplace(co_await std::move(task));
        }(std::move(task), result));
        return std::move(*result);
    }
}

/**
 * A task that completes `duration` after its first poll.
 */
Task<void> sleepFor(std::chrono::milliseconds duration);

struct FdReadiness
{
    bool readable = false;
    bool writable = false;

    bool operator==(const FdReadiness &) const = default;
};

/**
 * A task that completes when `fd` is ready for reading or writing, as
 * selected. An error or a hangup counts as readiness. `fd` must stay open
 * while the task exists, and must have no other registration with the
 * reactor. Unix only.
 */
Task<FdReadiness> waitFd(int fd, bool readable, bool writable);

/**
 * Run `f` once on the reactor thread. Functions run in the order of the
 * calls. An exception that escapes `f` is logged.
 */
void postToReactor(std::function<void()> f);

/**
 * The cleanup scope of the run on the reactor that polls the caller, for
 * a destructor that must wait, for example for a thread. See "Asynchronous
 * drop" in `doc/manual/source/development/async.md`.
 *
 * Take it with `current()` on the first poll, and keep it: the destructor
 * can run where no scope is visible. In the destructor, start the
 * cancellation, then give the wait to `defer()` if there is a scope, or
 * wait in place if not.
 */
class CleanupScope
{
    ::rust::Box<rs::CleanupScopeRef> scope;

    explicit CleanupScope(::rust::Box<rs::CleanupScopeRef> scope)
        : scope(std::move(scope))
    {
    }

public:
    /**
     * The scope of the current run on the reactor, or an empty scope
     * outside of one, for example under `runBlocking`.
     */
    static CleanupScope current();

    explicit operator bool() const;

    /**
     * Run `cleanup` on the reactor as part of the scope; the run completes
     * only after it. The scope must not be empty.
     */
    void defer(Task<void> cleanup) const;
};

/**
 * Runs a function on the reactor thread when it expires. Use it on the
 * reactor thread only; then the function never runs after `cancel()` or
 * after the destruction.
 */
class ReactorTimer
{
    ::rust::Box<rs::ReactorTimer> timer;

public:
    explicit ReactorTimer(std::function<void()> onExpiry);

    /**
     * Expire `delay` from now, instead of at the previous time.
     */
    void set(std::chrono::milliseconds delay);

    void cancel();
};

/**
 * Calls a function on the reactor thread while a file descriptor is ready
 * for the directions of interest, like a level-triggered `poll`. Use it on
 * the reactor thread only; then the function never runs after the
 * destruction. The descriptor must stay open until the destruction. Unix
 * only.
 */
class FdWatch
{
    ::rust::Box<rs::FdWatch> watch;

public:
    FdWatch(int fd, std::function<void(bool readable, bool writable)> onReady);

    void setInterest(bool readable, bool writable);
};

} // namespace nix

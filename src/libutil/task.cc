#include "nix/util/task.hh"
#include "nix/util/task-thread.hh"
#include "nix/util/signals.hh"
#include "nix/util/error.hh"
#include "nix/util/rust-error.hh"
#include "nix/util/util.hh"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nix {

void RustError::anchor() {}

void RustPanic::anchor() {}

const nix_waker_vtable WakeTarget::vtable = {
    .clone = &WakeTarget::cloneFn,
    .wake = &WakeTarget::wakeFn,
    .wake_by_ref = &WakeTarget::wakeByRefFn,
    .drop = &WakeTarget::dropFn,
};

static WakeTarget * asTarget(const void * data) noexcept
{
    return const_cast<WakeTarget *>(static_cast<const WakeTarget *>(data));
}

nix_waker WakeTarget::cloneFn(const void * data) noexcept
{
    asTarget(data)->addRef();
    return nix_waker{data, &vtable};
}

void WakeTarget::wakeFn(const void * data) noexcept
{
    auto * target = asTarget(data);
    target->wake();
    target->release();
}

void WakeTarget::wakeByRefFn(const void * data) noexcept
{
    asTarget(data)->wake();
}

void WakeTarget::dropFn(const void * data) noexcept
{
    asTarget(data)->release();
}

WakeTarget::~WakeTarget() = default;

Leaf::~Leaf() = default;

LeafAwaiter::~LeafAwaiter() = default;

namespace detail {

void WhenAllChild::wake() noexcept
{
    Waker parent;
    {
        std::lock_guard lock(shared->mutex);
        shared->woken.push_back(index);
        parent = shared->parent;
    }
    parent.wakeByRef();
}

/* `noexcept`: see `nix_exception_from_rust`. */
static nix_exception * makeRustError(const char * message, size_t length, nix_rust_value value) noexcept
{
    std::string text(message, length);
    /* A single string argument is the literal message, not a format
       string. */
    if (value.kind == NIX_RUST_PANIC)
        return new nix_exception{std::make_exception_ptr(RustPanic("Rust panic: " + text, value))};
    return new nix_exception{std::make_exception_ptr(RustError(text, value))};
}

void registerRustErrorFactory()
{
    /* A function-local static runs the registration once, also when
       several threads get here at the same time. Registration at load
       time would not work in a static build: the linker drops an object
       file that nothing refers to, together with its initializers. */
    [[maybe_unused]] static const bool registered = [] {
        nix_set_rust_error_factory(&makeRustError);
        return true;
    }();
}

void abortOutputTypeMismatch(const char * expected, const char * actual)
{
    std::fprintf(stderr, "nix: a C ABI future has output type '%s', but '%s' was expected\n", actual, expected);
    std::abort();
}

CFutureLeaf::~CFutureLeaf()
{
    if (future.vtable && !complete)
        future.vtable->drop(future.self);
}

nix_poll CFutureLeaf::pollLeaf(const Waker & waker)
{
    auto r = future.vtable->poll(future.self, &waker.c());
    if (r == NIX_POLL_READY)
        complete = true;
    return r;
}

namespace {

struct BlockingWakeTarget : WakeTarget
{
    std::mutex mutex;
    std::condition_variable cv;
    bool woken = true;

    void wake() noexcept override
    {
        {
            std::lock_guard lock(mutex);
            woken = true;
        }
        cv.notify_one();
    }
};

} // namespace

void blockOn(std::function<bool(const Waker &)> run)
{
    auto target = makeWakeTarget<BlockingWakeTarget>();
    auto waker = target->waker();

    /* Wake the loop on a user interrupt, so that `checkInterrupt()`
       throws and the caller's task is destroyed while unwinding. */
    auto interruptCallback = createInterruptCallback([target]() { target->wake(); });

    /* Likewise when the `runInThread` task that runs this thread is
       dropped: `checkInterrupt()` then throws on this thread. */
    auto cancelCallback = onThreadCancelled([target]() { target->wake(); });

    while (true) {
        {
            std::unique_lock lock(target->mutex);
            target->cv.wait(lock, [&] { return target->woken; });
            target->woken = false;
        }
        checkInterrupt();
        if (run(waker))
            return;
    }
}

namespace {

/**
 * The cancellation of the `runInThread` task that runs this thread.
 */
thread_local std::shared_ptr<ThreadCancellation> currentThreadCancellation;

} // namespace

void ThreadCancellation::cancel()
{
    std::lock_guard lock(mutex);
    cancelled = true;
    for (auto & callback : callbacks) {
        try {
            callback();
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
}

void ThreadCancellation::enter(std::shared_ptr<ThreadCancellation> cancellation)
{
#ifndef _WIN32
    unix::interruptCheck = [cancellation]() { return cancellation->cancelled.load(); };
#endif
    currentThreadCancellation = std::move(cancellation);
}

} // namespace detail

namespace {

struct InterruptLeaf : LeafAwaiter
{
    struct State
    {
        std::mutex mutex;
        Waker waker;

        void wake()
        {
            Waker w;
            {
                std::lock_guard lock(mutex);
                w = waker;
            }
            w.wakeByRef();
        }
    };

    std::shared_ptr<State> state = std::make_shared<State>();
    std::unique_ptr<InterruptCallback> interruptCallback;
    std::optional<ThreadCancelledCallback> cancelCallback;

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (!cancelCallback) {
            interruptCallback = createInterruptCallback([state = state]() { state->wake(); });
            cancelCallback.emplace(onThreadCancelled([state = state]() { state->wake(); }));
        }
        {
            Waker old;
            std::lock_guard lock(state->mutex);
            detail::replaceWaker(state->waker, waker, old);
        }
        return isInterrupted() ? NIX_POLL_READY : NIX_POLL_PENDING;
    }

    void await_resume()
    {
        checkInterrupt();
    }
};

} // namespace

Task<void> waitForInterrupt()
{
    co_await InterruptLeaf();
    /* `isInterrupted()` held, but the flag can be reset in between. */
    throw Interrupted("interrupted by the user");
}

ThreadCancelledCallback::~ThreadCancelledCallback()
{
    if (cancellation) {
        std::lock_guard lock(cancellation->mutex);
        cancellation->callbacks.erase(pos);
    }
}

ThreadCancelledCallback onThreadCancelled(fun<void()> callback)
{
    auto cancellation = detail::currentThreadCancellation;
    if (!cancellation)
        return {};

    std::lock_guard lock(cancellation->mutex);
    if (cancellation->cancelled) {
        /* Too late to wait for the cancellation. */
        callback();
        return {};
    }
    auto pos = cancellation->callbacks.insert(cancellation->callbacks.end(), std::move(callback));
    return {cancellation, pos};
}

} // namespace nix

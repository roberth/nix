#include "nix/util/task.hh"
#include "nix/util/signals.hh"
#include "nix/util/error.hh"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace nix {

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

static nix_exception * makeRustError(nix_rust_error_kind kind, const char * message, size_t length) noexcept
{
    std::string text(message, length);
    try {
        /* A single string argument is the literal message, not a format
           string. */
        throw Error(kind == NIX_RUST_PANIC ? "Rust panic: " + text : text);
    } catch (...) {
        return rs::currentException();
    }
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

} // namespace detail

} // namespace nix

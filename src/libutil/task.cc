#include "nix/util/task.hh"
#include "nix/util/signals.hh"

#include <condition_variable>

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

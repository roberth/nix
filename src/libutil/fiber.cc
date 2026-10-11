#include "nix/util/fiber.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/signals.hh"
#include "nix/util/sync.hh"

#include <boost/context/fiber.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>

#include <atomic>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace nix {

namespace {

/**
 * Written once, by the initialisation of the garbage collector, before
 * the first fiber starts; read on each stack switch.
 */
FiberStackHooks hooks{};

std::mutex disabledMutex;

std::optional<std::string> disabledReason =
#ifdef _WIN32
    /* Boost.Context supports Windows, but no test of the fibers ran
       there. */
    "fibers are not tested on Windows"
#else
    std::nullopt
#endif
    ;

/**
 * A free stack of the pool. It keeps its registration with the garbage
 * collector.
 */
struct FreeStack
{
    boost::context::stack_context sctx;
    void * cookie;
};

struct StackPool
{
    std::vector<FreeStack> free;

    /**
     * The cookies of the stacks in use, by the high end of the stack.
     */
    std::unordered_map<void *, void *> cookies;

    size_t allocated = 0;
};

/**
 * The pool keeps at most this many free stacks. A stack keeps the pages
 * that a fiber touched, so a reused stack costs no page faults, but a
 * free stack costs memory.
 */
constexpr size_t maxFreeStacks = 16;

/**
 * Leaked, so that a fiber stack that is freed late during the exit of
 * the process still finds the pool.
 */
Sync<StackPool> & stackPool()
{
    static auto * pool = new Sync<StackPool>();
    return *pool;
}

/**
 * The cookie of the stack that `PoolAllocator::allocate` returned last
 * on this thread. The constructor of `boost::context::fiber` calls the
 * allocator and returns only the fiber, so the cookie comes back
 * through here.
 */
thread_local void * lastStackCookie = nullptr;

struct PoolAllocator
{
    boost::context::stack_context allocate()
    {
        {
            auto pool(stackPool().lock());
            if (!pool->free.empty()) {
                auto stack = pool->free.back();
                pool->free.pop_back();
                /* The hooks can come after the stack. */
                if (!stack.cookie && hooks.registerStack)
                    stack.cookie = hooks.registerStack(stack.sctx.sp, stack.sctx.size);
                pool->cookies.emplace(stack.sctx.sp, stack.cookie);
                lastStackCookie = stack.cookie;
                return stack.sctx;
            }
        }
        auto sctx = boost::context::protected_fixedsize_stack(fiberStackSize).allocate();
        void * cookie = hooks.registerStack ? hooks.registerStack(sctx.sp, sctx.size) : nullptr;
        auto pool(stackPool().lock());
        pool->allocated++;
        pool->cookies.emplace(sctx.sp, cookie);
        lastStackCookie = cookie;
        return sctx;
    }

    void deallocate(boost::context::stack_context & sctx) noexcept
    {
        void * cookie;
        {
            auto pool(stackPool().lock());
            auto i = pool->cookies.find(sctx.sp);
            assert(i != pool->cookies.end());
            cookie = i->second;
            pool->cookies.erase(i);
            if (pool->free.size() < maxFreeStacks) {
                pool->free.push_back({sctx, cookie});
                return;
            }
        }
        if (cookie && hooks.unregisterStack)
            hooks.unregisterStack(cookie);
        boost::context::protected_fixedsize_stack(fiberStackSize).deallocate(sctx);
    }
};

thread_local Fiber * currentFiber = nullptr;

thread_local detail::FiberRun * currentRun = nullptr;

} // namespace

void setFiberStackHooks(const FiberStackHooks & newHooks)
{
    hooks = newHooks;
}

void disableFibers(std::string reason)
{
    std::lock_guard lock(disabledMutex);
    disabledReason = std::move(reason);
}

std::optional<std::string> fibersDisabledReason()
{
    std::lock_guard lock(disabledMutex);
    return disabledReason;
}

FiberStackStats fiberStackStats()
{
    auto pool(stackPool().lock());
    return FiberStackStats{
        .allocated = pool->allocated,
        .inUse = pool->cookies.size(),
        .free = pool->free.size(),
    };
}

struct Fiber::Impl
{
    fun<void()> body;

    /**
     * The continuation of the fiber, while it does not run.
     */
    boost::context::fiber ctx;

    /**
     * The continuation of the code that resumed the fiber, while the
     * fiber runs.
     */
    boost::context::fiber caller;

    /**
     * The cookie of the stack for the `FiberStackHooks`.
     */
    void * cookie = nullptr;

    std::exception_ptr exception;

    bool started = false;
    bool finished = false;

    std::thread::id thread;

    /**
     * The current activity of the logger in the fiber, while the fiber
     * does not run. It is thread-local state that a `PushActivity` on the
     * fiber sets, and another fiber on the same thread must not see it.
     */
    ActivityId activity = 0;

    explicit Impl(fun<void()> body)
        : body(std::move(body))
    {
    }
};

Fiber::Fiber(fun<void()> body)
    : impl(std::make_unique<Impl>(std::move(body)))
{
    auto * i = impl.get();
    i->activity = getCurActivity();
    i->ctx = boost::context::fiber(
        std::allocator_arg, PoolAllocator{}, [i](boost::context::fiber && caller) -> boost::context::fiber {
            i->caller = std::move(caller);
            if (hooks.resumed)
                hooks.resumed(i->cookie);
            try {
                i->body();
            } catch (...) {
                i->exception = std::current_exception();
            }
            i->finished = true;
            /* The stack returns to the pool after this switch. Its saved
               stack pointer is clear, so the collector ignores it. */
            return std::move(i->caller);
        });
    i->cookie = lastStackCookie;
}

Fiber::~Fiber()
{
    /* Destroying a suspended `boost::context::fiber` unwinds its stack
       with a `forced_unwind` exception that bypasses the cancellation of
       `runOnFiber`. */
    if (impl->started && !impl->finished)
        panic("a suspended fiber is destroyed");
}

bool Fiber::resume()
{
    auto * i = impl.get();
    assert(!i->finished);
    if (!i->started) {
        i->started = true;
        i->thread = std::this_thread::get_id();
    } else if (i->thread != std::this_thread::get_id())
        /* Code on the fiber may hold the address of a `thread_local` of
           the other thread. */
        panic("a fiber is resumed on a thread other than the one that started it");

    auto * prevFiber = std::exchange(currentFiber, this);
    auto prevActivity = getCurActivity();
    setCurActivity(i->activity);

    void * handle = hooks.switchTo ? hooks.switchTo(i->cookie) : nullptr;
    i->ctx = std::move(i->ctx).resume();
    if (hooks.switchedBack)
        hooks.switchedBack(handle);

    i->activity = getCurActivity();
    setCurActivity(prevActivity);
    currentFiber = prevFiber;

    return i->finished;
}

bool Fiber::finished() const noexcept
{
    return impl->finished;
}

bool Fiber::started() const noexcept
{
    return impl->started;
}

std::exception_ptr Fiber::exception() const noexcept
{
    return impl->exception;
}

void Fiber::suspend()
{
    auto * fiber = currentFiber;
    assert(fiber && "Fiber::suspend() is called outside of a fiber");
    auto * i = fiber->impl.get();
    if (hooks.suspend)
        hooks.suspend(i->cookie);
    i->caller = std::move(i->caller).resume();
    if (hooks.resumed)
        hooks.resumed(i->cookie);
}

Fiber * Fiber::current() noexcept
{
    return currentFiber;
}

namespace detail {

FiberRun * currentFiberRun() noexcept
{
    return currentRun;
}

void awaitOnFiberImpl(FiberRun & run, fun<nix_poll(const Waker &)> poll)
{
    while (true) {
        if (run.cancelled)
            throw Interrupted("the task that runs this fiber was dropped");
        assert(run.waker);
        if (poll(*run.waker) == NIX_POLL_READY)
            return;
        Fiber::suspend();
    }
}

namespace {

/**
 * The fiber of a `runOnFiber` task. It lives in the frame of the task.
 */
struct FiberLeaf : LeafAwaiter
{
    fun<void()> body;
    FiberRun run;
    std::unique_ptr<Fiber> fiber;
    std::exception_ptr exception;

    explicit FiberLeaf(fun<void()> body)
        : body(std::move(body))
    {
    }

    /**
     * Resume the fiber with `waker` as the waker of the run. Returns
     * true if the fiber ended.
     */
    bool resumeWith(const Waker & waker)
    {
        run.waker = &waker;
        auto * prevRun = std::exchange(currentRun, &run);
        bool done = fiber->resume();
        currentRun = prevRun;
        run.waker = nullptr;
        if (done)
            exception = fiber->exception();
        return done;
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (!fiber) {
            if (fibersDisabledReason()) {
                try {
                    body();
                } catch (...) {
                    exception = std::current_exception();
                }
                return NIX_POLL_READY;
            }
            fiber = std::make_unique<Fiber>(std::move(body));
        }
        return resumeWith(waker) ? NIX_POLL_READY : NIX_POLL_PENDING;
    }

    void await_resume()
    {
        if (exception)
            std::rethrow_exception(exception);
    }

    ~FiberLeaf() override
    {
        if (!fiber || !fiber->started() || fiber->finished())
            return;
        /* Cancel: `awaitOnFiberImpl` throws on the fiber, which unwinds
           its stack. A body that catches the exception and awaits again
           gets it again; one that suspends by other means is resumed
           until it ends. */
        run.cancelled = true;
        Waker noWaker;
        while (!resumeWith(noWaker))
            ;
    }
};

} // namespace

Task<void> runOnFiberImpl(fun<void()> body)
{
    FiberLeaf leaf(std::move(body));
    co_await leaf;
}

} // namespace detail

} // namespace nix

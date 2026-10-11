#include "nix/util/environment-variables.hh"
#include "nix/expr/eval-settings.hh"
#include "nix/util/config-global.hh"
#include "nix/expr/eval-gc.hh"
#include "nix/expr/value.hh"
#include "nix/util/fiber.hh"

#include "expr-config-private.hh"

#if NIX_USE_BOEHMGC

#  include <pthread.h>
#  ifdef __FreeBSD__
#    include <pthread_np.h>
#  endif

#  include <gc/gc_allocator.h>
#  include <gc/gc_tiny_fl.h> // For GC_GRANULE_BYTES

#  include <boost/coroutine2/coroutine.hpp>
#  include <boost/coroutine2/protected_fixedsize_stack.hpp>
#  include <boost/context/stack_context.hpp>

#endif

namespace nix {

#if NIX_USE_BOEHMGC

/*
 * Ensure that Boehm satisfies our alignment requirements. This is the default configuration [^]
 * and this assertion should never break for any platform. Let's assert it just in case.
 *
 * This alignment is particularly useful to be able to use aligned
 * load/store instructions for loading/writing Values.
 *
 * [^]: https://github.com/bdwgc/bdwgc/blob/54ac18ccbc5a833dd7edaff94a10ab9b65044d61/include/gc/gc_tiny_fl.h#L31-L33
 */
static_assert(sizeof(void *) * 2 == GC_GRANULE_BYTES, "Boehm GC must use GC_GRANULE_WORDS = 2");

/* Called when the Boehm GC runs out of memory. */
static void * oomHandler(size_t requested)
{
    /* Convert this to a proper C++ exception. */
    throw std::bad_alloc();
}

#  if NIX_GC_USER_STACKS

/*
 * The `FiberStackHooks` of libutil, with the client-registered stacks of
 * bdwgc. The collector scans the stack that a stopped thread executes
 * on from its stack pointer, and each other registered stack that has a
 * saved stack pointer from there. So each stack that holds live frames,
 * but that no thread executes on, must have a saved stack pointer: the
 * stack of the thread while it runs a fiber, and the stack of a
 * suspended fiber. `GC_current_stack` is the stack that the thread
 * executes on; the hooks keep it correct across the switches.
 */

/**
 * The saved stack pointer is lower than `GC_get_approx_sp()` by this
 * much. The approximation is taken in a callee of the function that
 * switches stacks. The slack covers what the switch puts below it: the
 * frames of `boost::context::fiber::resume` and the callee-saved
 * registers that its assembly stores on the stack (64 bytes on x86-64,
 * 176 on aarch64). Those registers can hold pointers of the callers.
 * The same value as in DeterminateSystems/nix-src.
 */
static constexpr size_t gcStackSwitchSlack = 512;

static void gcSaveStackPointer(struct GC_stack * stk)
{
    auto sp = (char *) GC_get_approx_sp();
    /* A switch from a stack that is not `stk`, for example from the
       unregistered stack of a `sinkToSource` coroutine, would make the
       collector scan from one stack to the base of another. */
    if (!(sp < (char *) stk->base && (!stk->limit || sp >= (char *) stk->limit)))
        panic(
            fmt("stack pointer %p is not in the stack [%p, %p) that the thread leaves",
                (void *) sp,
                stk->limit,
                stk->base));
    stk->saved_sp = sp - gcStackSwitchSlack;
}

static void * fiberRegisterStack(void * base, size_t size)
{
    auto stk = new GC_stack{};
    stk->base = base;
    stk->limit = (char *) base - size;
    GC_register_stack(stk);
    return stk;
}

static void fiberUnregisterStack(void * cookie)
{
    auto stk = (struct GC_stack *) cookie;
    GC_unregister_stack(stk);
    delete stk;
}

static void * fiberSwitchTo(void * cookie)
{
    /* Null on a thread that is not registered with the collector. Such a
       thread holds no roots. */
    auto prev = GC_current_stack;
    if (prev)
        gcSaveStackPointer(prev);
    GC_current_stack = (struct GC_stack *) cookie;
    return prev;
}

static void fiberSwitchedBack(void * handle)
{
    auto prev = (struct GC_stack *) handle;
    GC_current_stack = prev;
    if (prev)
        prev->saved_sp = nullptr;
}

static void fiberSuspend(void * cookie)
{
    gcSaveStackPointer((struct GC_stack *) cookie);
}

static void fiberResumed(void * cookie)
{
    auto stk = (struct GC_stack *) cookie;
    stk->saved_sp = nullptr;
    GC_current_stack = stk;
}

#  endif

static inline void initGCReal()
{
    /* Initialise the Boehm garbage collector. */

    /* Don't look for interior pointers. This reduces the odds of
       misdetection a bit. */
    GC_set_all_interior_pointers(0);

    /* We don't have any roots in data segments, so don't scan from
       there. */
    GC_set_no_dls(1);

    /* Enable perf measurements. This is just a setting; not much of a
       start of something. */
    GC_start_performance_measurement();

    GC_INIT();

    /* Enable parallel marking. */
    GC_allow_register_threads();

    /* Register valid displacements in case we are using alignment niches
       for storing the type information. This way tagged pointers are considered
       to be valid, even when they are not aligned. */
    if constexpr (detail::useBitPackedValueStorage<sizeof(void *)>)
        for (std::size_t i = 1; i < sizeof(std::uintptr_t); ++i)
            GC_register_displacement(i);

    GC_set_oom_fn(oomHandler);

    /* Make the stacks of suspended fibers visible to the collector. */
#  if NIX_GC_USER_STACKS
    setFiberStackHooks(
        FiberStackHooks{
            .registerStack = fiberRegisterStack,
            .unregisterStack = fiberUnregisterStack,
            .switchTo = fiberSwitchTo,
            .switchedBack = fiberSwitchedBack,
            .suspend = fiberSuspend,
            .resumed = fiberResumed,
        });
#  else
    disableFibers("the Boehm GC of this build cannot scan the stacks of fibers");
#  endif

    /* Funnel boehm warnings into debug logs. The message is `char *`
       before bdwgc 8.3 and `const char *` from 8.3 on; the generic
       lambda converts to either. */
    GC_set_warn_proc([](auto * msg, GC_word word) noexcept {
        std::array<char, 4096> buffer{};
        auto res = snprintf(buffer.data(), buffer.size(), msg, word);
        /* Ignore garbage. */
        if (res < 0)
            return;

        try {
            debug("%s", chomp(std::string_view(buffer.data(), std::min<size_t>(res, buffer.size() - 1))));
        } catch (...) {
            /* Swallow all errors. */
        }
    });

    /* Set the initial heap size to something fairly big (25% of
       physical RAM, up to a maximum of 384 MiB) so that in most cases
       we don't need to garbage collect at all.  (Collection has a
       fairly significant overhead.)  The heap size can be overridden
       through libgc's GC_INITIAL_HEAP_SIZE environment variable.  We
       should probably also provide a nix.conf setting for this.  Note
       that GC_expand_hp() causes a lot of virtual, but not physical
       (resident) memory to be allocated.  This might be a problem on
       systems that don't overcommit. */
    if (!getEnv("GC_INITIAL_HEAP_SIZE")) {
        size_t size = 32 * 1024 * 1024;
#  if HAVE_SYSCONF && defined(_SC_PAGESIZE) && defined(_SC_PHYS_PAGES)
        size_t maxSize = 384 * 1024 * 1024;
        long pageSize = sysconf(_SC_PAGESIZE);
        long pages = sysconf(_SC_PHYS_PAGES);
        if (pageSize != -1)
            size = (pageSize * pages) / 4; // 25% of RAM
        if (size > maxSize)
            size = maxSize;
#  endif
        debug("setting initial heap size to %1% bytes", size);
        GC_expand_hp(size);
    }
}

static size_t gcCyclesAfterInit = 0;

size_t getGCCycles()
{
    assertGCInitialized();
    return static_cast<size_t>(GC_get_gc_no()) - gcCyclesAfterInit;
}

#endif

static bool gcInitialised = false;

void initGC()
{
    if (gcInitialised)
        return;

#if NIX_USE_BOEHMGC
    initGCReal();

    gcCyclesAfterInit = GC_get_gc_no();
#endif

    // NIX_PATH must override the regular setting
    // See the comment in applyConfig
    if (auto nixPathEnv = getEnv("NIX_PATH")) {
        globalConfig.set("nix-path", concatStringsSep(" ", EvalSettings::parseNixPath(nixPathEnv.value())));
    }

    gcInitialised = true;
}

void assertGCInitialized()
{
    assert(gcInitialised);
}

} // namespace nix

#include "nix/util/task.hh"
#include "nix/util/task-thread.hh"
#include "nix/util/signals.hh"
#include "nix/util/error.hh"
#include "nix/util/rust-error.hh"
#include "nix/util/util.hh"

#include "file-descriptor-private.hh"

#ifndef _WIN32
#  include <climits>
#  include <poll.h>
#  include <sys/stat.h>
#endif

#include <algorithm>
#include <array>
#include <deque>
#include <thread>
#include <vector>
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

#ifndef _WIN32
Descriptor ThreadCancellation::descriptor()
{
    std::lock_guard lock(mutex);
    if (!pipe.readSide) {
        pipe.create();
        if (cancelled)
            pipe.writeSide.close();
    }
    return pipe.readSide.get();
}
#endif

void ThreadCancellation::cancel()
{
    std::lock_guard lock(mutex);
    cancelled = true;
#ifndef _WIN32
    /* The end of file wakes a `poll` in `waitForFdOrThreadCancellation`.
       It stays readable, so a later `poll` wakes up as well. */
    if (pipe.writeSide)
        pipe.writeSide.close();
#endif
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

void ThreadCancellation::leave()
{
#ifndef _WIN32
    unix::interruptCheck = nullptr;
#endif
    currentThreadCancellation.reset();
}

PoolJob::~PoolJob() = default;

void PoolJob::finish() noexcept
{
    Waker toWake;
    {
        std::lock_guard lock(mutex);
        done = true;
        toWake = std::move(waker);
    }
    doneCv.notify_all();
    std::move(toWake).wake();
}

nix_poll PoolJobDoneLeaf::pollLeaf(const Waker & waker)
{
    Waker old;
    std::lock_guard lock(job->mutex);
    if (job->done)
        return NIX_POLL_READY;
    replaceWaker(job->waker, waker, old);
    return NIX_POLL_PENDING;
}

} // namespace detail

struct BlockingPool::State
{
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<detail::PoolJob>> queue;
    std::vector<std::thread> threads;
    /**
     * The threads that wait for a job.
     */
    size_t idle = 0;
    size_t maxThreads;
    bool stopping = false;
};

static void runPoolThread(std::shared_ptr<BlockingPool::State> state)
{
    ReceiveInterrupts receiveInterrupts;
    while (true) {
        std::shared_ptr<detail::PoolJob> job;
        {
            std::unique_lock lock(state->mutex);
            ++state->idle;
            state->cv.wait(lock, [&] { return state->stopping || !state->queue.empty(); });
            --state->idle;
            if (state->queue.empty())
                return;
            job = std::move(state->queue.front());
            state->queue.pop_front();
        }
        detail::ThreadCancellation::enter(job->cancellation);
        job->run();
        detail::ThreadCancellation::leave();
    }
}

BlockingPool::BlockingPool(size_t maxThreads)
    : state(std::make_shared<State>())
{
    state->maxThreads = std::max<size_t>(1, maxThreads);
}

BlockingPool::~BlockingPool()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(state->mutex);
        assert(state->queue.empty());
        state->stopping = true;
        threads = std::move(state->threads);
    }
    state->cv.notify_all();
    for (auto & thread : threads)
        thread.join();
}

void BlockingPool::submit(const std::shared_ptr<State> & state, std::shared_ptr<detail::PoolJob> job)
{
    std::lock_guard lock(state->mutex);
    state->queue.push_back(std::move(job));
    /* Start a thread if the idle ones cannot take all queued jobs. */
    if (state->queue.size() > state->idle && state->threads.size() < state->maxThreads)
        state->threads.emplace_back(runPoolThread, state);
    else
        state->cv.notify_one();
}

bool BlockingPool::withdraw(State & state, const detail::PoolJob * job)
{
    std::lock_guard lock(state.mutex);
    auto i = std::ranges::find_if(state.queue, [&](auto & j) { return j.get() == job; });
    if (i == state.queue.end())
        return false;
    state.queue.erase(i);
    return true;
}

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

#ifndef _WIN32
size_t waitForFdOrThreadCancellation(Descriptor fd, short events, size_t size)
{
    auto & cancellation = detail::currentThreadCancellation;
    /* `poll` ignores a negative descriptor, and would wait for the
       cancellation only. Let the call of the caller fail instead. */
    if (!cancellation || fd < 0)
        return size;

    if (events & POLLOUT) {
        /* A write to a file does not wait for a peer, and `poll` reports
           a file as ready anyway. Only a pipe or a socket needs the wait
           and the bound, which would make a large write to a file slow. */
        struct stat st;
        if (::fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)))
            return size;
    }

    std::array<pollfd, 2> fds{
        pollfd{.fd = fd, .events = events, .revents = 0},
        pollfd{.fd = cancellation->descriptor(), .events = POLLIN, .revents = 0},
    };
    while (true) {
        if (::poll(fds.data(), fds.size(), -1) == -1) {
            if (errno == EINTR) {
                checkInterrupt();
                continue;
            }
            throw SysError("waiting for a file descriptor");
        }
        if (fds[1].revents) {
            /* The cancellation sets the flag before it closes the pipe, so
               `checkInterrupt()` throws, unless an exception unwinds the
               stack already, as for a user interrupt. Then the call
               waits, as before.

               Without the flag, this is a child that the thread forked,
               with a copy of the thread-local state, and the child closed
               the descriptor of the pipe (`POLLNVAL`) or reused its
               number. Nothing cancels a child; let the call wait. */
            checkInterrupt();
            return size;
        }
        if (fds[0].revents)
            /* A blocking read returns what is there. A blocking write
               waits until it wrote everything, but `poll` only promises
               room for `PIPE_BUF` bytes. */
            return (events & POLLOUT) ? std::min<size_t>(size, PIPE_BUF) : size;
    }
}
#endif

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

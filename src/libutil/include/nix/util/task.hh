#pragma once
/**
 * @file
 *
 * `nix::Task<T>`: a C++20 coroutine with the semantics of a Rust future.
 *
 * See `doc/manual/source/development/async.md` for the contract and the
 * reasons for it. In short:
 *
 * - A task is lazy. Its body does not run until the task is first polled
 *   or awaited.
 * - A task has one owner. It is move-only.
 * - Destroying a task that is not complete cancels it. The coroutine frame
 *   is destroyed at its suspension point, the destructors of its locals
 *   run, and the task that it awaits is destroyed too. Nothing resumes
 *   the task afterwards. `catch` blocks do not run on cancellation.
 * - An exception that escapes the body is stored and rethrown to the
 *   awaiter with its dynamic type.
 * - A task is driven by `poll(waker)`, the same interface as
 *   `Future::poll` in Rust. When `poll` returns `NIX_POLL_PENDING`, the
 *   task has arranged for the waker to be woken when it can make
 *   progress. The waker has the C layout of `future-abi.h`.
 *
 * When a task awaits another task, control transfers directly between the
 * two coroutines (symmetric transfer). Only the outermost task in a chain
 * is polled. The chain shares one `TaskContext`, which records the leaf
 * operation that the chain is suspended on.
 */

#include "nix/rust/future-abi.hh"

#include <atomic>
#include <cassert>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nix {

/**
 * An owning handle to a `nix_waker`.
 *
 * Copying clones the waker, destruction drops it. A default-constructed
 * waker is empty and waking it does nothing.
 */
class Waker
{
    nix_waker raw{nullptr, nullptr};

public:
    Waker() = default;

    /**
     * Take ownership of a raw waker.
     */
    explicit Waker(nix_waker raw) noexcept
        : raw(raw)
    {
    }

    Waker(const Waker & other) noexcept
        : raw(other.raw.vtable ? other.raw.vtable->clone(other.raw.data) : other.raw)
    {
    }

    Waker(Waker && other) noexcept
        : raw(std::exchange(other.raw, {nullptr, nullptr}))
    {
    }

    Waker & operator=(const Waker & other) noexcept
    {
        if (this != &other)
            *this = Waker(other);
        return *this;
    }

    Waker & operator=(Waker && other) noexcept
    {
        if (this != &other) {
            reset();
            raw = std::exchange(other.raw, {nullptr, nullptr});
        }
        return *this;
    }

    ~Waker()
    {
        reset();
    }

    void reset() noexcept
    {
        auto r = std::exchange(raw, {nullptr, nullptr});
        if (r.vtable)
            r.vtable->drop(r.data);
    }

    /**
     * Wake the task and consume this waker.
     */
    void wake() && noexcept
    {
        auto r = std::exchange(raw, {nullptr, nullptr});
        if (r.vtable)
            r.vtable->wake(r.data);
    }

    void wakeByRef() const noexcept
    {
        if (raw.vtable)
            raw.vtable->wake_by_ref(raw.data);
    }

    /**
     * Whether waking `other` wakes the same task as waking this waker.
     * Like Rust's `Waker::will_wake`, this compares identity only, so it
     * can return false for equivalent wakers.
     */
    bool willWake(const Waker & other) const noexcept
    {
        return raw.data == other.raw.data && raw.vtable == other.raw.vtable;
    }

    explicit operator bool() const noexcept
    {
        return raw.vtable != nullptr;
    }

    const nix_waker & c() const noexcept
    {
        return raw;
    }

    /**
     * Release ownership of the raw waker to the caller.
     */
    nix_waker release() && noexcept
    {
        return std::exchange(raw, {nullptr, nullptr});
    }

    /**
     * View a borrowed raw waker as a `Waker` without taking ownership,
     * for example the `waker` argument of a `nix_future_vtable::poll`.
     */
    static const Waker & borrow(const nix_waker * w) noexcept
    {
        return *reinterpret_cast<const Waker *>(w);
    }
};

static_assert(std::is_standard_layout_v<Waker>);
static_assert(sizeof(Waker) == sizeof(nix_waker));

/**
 * Base class for objects that can be woken through a `Waker`.
 *
 * The object is reference-counted intrusively, so that a `Waker` is two
 * pointers and cloning it does not allocate. Create instances with
 * `makeWakeTarget()` and wakers with `waker()`.
 */
class WakeTarget
{
    std::atomic<size_t> refs{1};

    static const nix_waker_vtable vtable;

    static nix_waker cloneFn(const void * data) noexcept;
    static void wakeFn(const void * data) noexcept;
    static void wakeByRefFn(const void * data) noexcept;
    static void dropFn(const void * data) noexcept;

public:
    WakeTarget() = default;
    WakeTarget(const WakeTarget &) = delete;
    WakeTarget & operator=(const WakeTarget &) = delete;

    /**
     * Called on any thread, possibly concurrently with itself.
     */
    virtual void wake() noexcept = 0;

    void addRef() noexcept
    {
        refs.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept
    {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    /**
     * A new waker that wakes this object.
     */
    Waker waker() noexcept
    {
        addRef();
        return Waker(nix_waker{this, &vtable});
    }

protected:
    virtual ~WakeTarget();
};

/**
 * An owning pointer to a `WakeTarget`.
 */
template<typename T>
class WakeTargetRef
{
    T * p = nullptr;

public:
    WakeTargetRef() = default;

    /**
     * Adopt a reference, without incrementing the count.
     */
    explicit WakeTargetRef(T * p) noexcept
        : p(p)
    {
    }

    WakeTargetRef(const WakeTargetRef & other) noexcept
        : p(other.p)
    {
        if (p)
            p->addRef();
    }

    WakeTargetRef(WakeTargetRef && other) noexcept
        : p(std::exchange(other.p, nullptr))
    {
    }

    WakeTargetRef & operator=(WakeTargetRef other) noexcept
    {
        std::swap(p, other.p);
        return *this;
    }

    ~WakeTargetRef()
    {
        if (p)
            p->release();
    }

    T * operator->() const noexcept
    {
        return p;
    }

    T & operator*() const noexcept
    {
        return *p;
    }

    T * get() const noexcept
    {
        return p;
    }
};

template<typename T, typename... Args>
WakeTargetRef<T> makeWakeTarget(Args &&... args)
{
    return WakeTargetRef<T>(new T(std::forward<Args>(args)...));
}

/**
 * An operation that a task chain can suspend on, other than another task.
 *
 * `pollLeaf` has the contract of `Future::poll`: it returns
 * `NIX_POLL_READY` when the operation is complete, and otherwise arranges
 * for `waker` to be woken when it can make progress.
 *
 * A leaf lives in the frame of the coroutine that awaits it. Its
 * destructor must cancel the operation if it is not complete, and must
 * make sure that nothing touches the leaf afterwards.
 */
class Leaf
{
public:
    virtual nix_poll pollLeaf(const Waker & waker) = 0;

    /**
     * Virtual only to satisfy `-Wnon-virtual-dtor`; leaves are never
     * destroyed through a `Leaf *`.
     */
    virtual ~Leaf();
};

namespace detail {

/**
 * State shared by all tasks in a chain of `co_await`s. It lives in the
 * frame of the outermost task.
 */
struct TaskContext
{
    /**
     * The waker of the current `poll` call, or null outside of `poll`.
     */
    const Waker * waker = nullptr;

    /**
     * The leaf that the chain is suspended on, or null.
     */
    Leaf * leaf = nullptr;

    /**
     * The coroutine to resume when `leaf` is ready.
     */
    std::coroutine_handle<> resumeAt;
};

struct PromiseBase
{
    TaskContext ownContext;

    /**
     * Points to `ownContext` while the task is polled directly, and to
     * the awaiting task's context while another task awaits it.
     */
    TaskContext * context = &ownContext;

    std::coroutine_handle<> continuation;

    std::exception_ptr exception;

    bool started = false;
    bool completed = false;

    std::suspend_always initial_suspend() noexcept
    {
        return {};
    }

    struct FinalAwaiter
    {
        bool await_ready() noexcept
        {
            return false;
        }

        template<typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept
        {
            auto & p = h.promise();
            p.completed = true;
            if (p.continuation)
                return p.continuation;
            return std::noop_coroutine();
        }

        void await_resume() noexcept {}
    };

    FinalAwaiter final_suspend() noexcept
    {
        return {};
    }

    void unhandled_exception() noexcept
    {
        exception = std::current_exception();
    }
};

template<typename T>
struct Promise : PromiseBase
{
    std::optional<T> value;

    template<typename U = T>
    void return_value(U && v)
    {
        value.emplace(std::forward<U>(v));
    }

    T takeResult()
    {
        if (exception)
            std::rethrow_exception(std::exchange(exception, nullptr));
        assert(value);
        return std::move(*value);
    }
};

template<>
struct Promise<void> : PromiseBase
{
    void return_void() noexcept {}

    void takeResult()
    {
        if (exception)
            std::rethrow_exception(std::exchange(exception, nullptr));
    }
};

} // namespace detail

/**
 * Await a leaf from a task. Derived classes implement `pollLeaf` and
 * `await_resume`.
 */
struct LeafAwaiter : Leaf
{
    ~LeafAwaiter() override;

    bool await_ready() noexcept
    {
        return false;
    }

    template<typename P>
    bool await_suspend(std::coroutine_handle<P> h)
    {
        auto * context = h.promise().context;
        assert(context->waker && "a task can only suspend while it is polled");
        if (pollLeaf(*context->waker) == NIX_POLL_READY)
            return false;
        context->leaf = this;
        context->resumeAt = h;
        return true;
    }
};

template<typename T>
class [[nodiscard]] Task
{
public:
    struct promise_type : detail::Promise<T>
    {
        Task get_return_object() noexcept
        {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
    };

    using Handle = std::coroutine_handle<promise_type>;

private:
    Handle h;

    explicit Task(Handle h) noexcept
        : h(h)
    {
    }

public:
    Task() = default;

    Task(Task && other) noexcept
        : h(std::exchange(other.h, {}))
    {
    }

    Task & operator=(Task && other) noexcept
    {
        if (this != &other) {
            reset();
            h = std::exchange(other.h, {});
        }
        return *this;
    }

    Task(const Task &) = delete;
    Task & operator=(const Task &) = delete;

    ~Task()
    {
        reset();
    }

    /**
     * Destroy the task. If it is not complete, this cancels it.
     */
    void reset() noexcept
    {
        if (auto h2 = std::exchange(h, {}))
            h2.destroy();
    }

    explicit operator bool() const noexcept
    {
        return bool(h);
    }

    bool isComplete() const noexcept
    {
        return h && h.promise().completed;
    }

    /**
     * Drive the task. The task must not be complete, and must not be
     * awaited by another task at the same time.
     */
    nix_poll poll(const Waker & waker)
    {
        assert(h && !h.promise().completed);
        auto & p = h.promise();
        auto & context = *p.context;
        assert(&context == &p.ownContext);

        context.waker = &waker;

        if (!p.started) {
            p.started = true;
            h.resume();
        } else if (context.leaf && context.leaf->pollLeaf(waker) == NIX_POLL_READY) {
            context.leaf = nullptr;
            std::exchange(context.resumeAt, {}).resume();
        }

        context.waker = nullptr;

        return p.completed ? NIX_POLL_READY : NIX_POLL_PENDING;
    }

    /**
     * The result of a complete task. Rethrows the exception that escaped
     * the task body, if any. Can be called once.
     */
    T takeResult()
    {
        assert(isComplete());
        return h.promise().takeResult();
    }

    struct Awaiter
    {
        Task task;

        bool await_ready() noexcept
        {
            return false;
        }

        template<typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept
        {
            auto & p = task.h.promise();
            assert(!p.started);
            p.started = true;
            p.context = parent.promise().context;
            p.continuation = parent;
            return task.h;
        }

        T await_resume()
        {
            return task.h.promise().takeResult();
        }
    };

    /**
     * Await the task from another task. The awaiting task owns the
     * awaited one until it completes, so cancelling the awaiting task
     * cancels the awaited one.
     */
    Awaiter operator co_await() && noexcept
    {
        return Awaiter{std::move(*this)};
    }
};

/**
 * Run a task to completion on the current thread, blocking between polls.
 *
 * A user interrupt (`checkInterrupt()`) wakes the loop, throws
 * `Interrupted`, and cancels the task by destroying it.
 */
template<typename T>
T runBlocking(Task<T> task);

namespace detail {

/**
 * Block the current thread until `run` returns true. `run` is called once
 * at the start and then each time the waker passed to it is woken.
 * Checks for user interrupts between calls.
 */
void blockOn(std::function<bool(const Waker &)> run);

} // namespace detail

template<typename T>
T runBlocking(Task<T> task)
{
    detail::blockOn([&](const Waker & waker) { return task.poll(waker) == NIX_POLL_READY; });
    return task.takeResult();
}

/**
 * Adapt a callback-based operation to a task.
 *
 * `initiate` receives a `Callback<T>` and starts the operation. It is
 * called on the first poll, not when the task is created. The callback
 * can be invoked on any thread.
 *
 * If `initiate` returns a callable, the task calls it when it is
 * cancelled before the callback was invoked, to cancel the operation.
 * The operation may still invoke the callback afterwards; the result is
 * then discarded. If `initiate` returns `void`, cancellation only
 * discards the result.
 */
template<typename T, typename F>
Task<T> callbackToTask(F initiate);

namespace detail {

template<typename T>
struct CallbackState
{
    std::mutex mutex;
    std::variant<std::monostate, T, std::exception_ptr> result;
    Waker waker;
    bool dropped = false;
};

} // namespace detail

/**
 * Await all tasks concurrently and return their results in order.
 *
 * If a task throws, the remaining tasks are cancelled and the exception
 * is rethrown. Only tasks that woke their waker are polled again.
 */
template<typename T>
Task<std::conditional_t<std::is_void_v<T>, void, std::vector<T>>> whenAll(std::vector<Task<T>> tasks);

namespace detail {
template<typename T>
struct TaskSetNextLeaf;
struct WhenAllShared;
} // namespace detail

/**
 * A set of tasks that run concurrently and complete in any order, like
 * `FuturesUnordered` in Rust. The owner adds tasks with `push()` at any
 * time, also while other tasks in the set are pending, and awaits
 * `next()` for the result of the next task that completes.
 *
 * `whenAll` takes a fixed list. A `TaskSet` is for work that grows while
 * it runs, for example a graph traversal that finds new nodes in the
 * results of earlier tasks.
 *
 * The tasks make progress only while the owner awaits `next()`.
 * Destroying the set cancels the tasks in it. Each task has its own
 * waker, so a wake polls only the task that woke.
 */
template<typename T>
class TaskSet
{
    static_assert(!std::is_void_v<T>, "TaskSet<void> is not implemented");

    friend struct detail::TaskSetNextLeaf<T>;

    std::shared_ptr<detail::WhenAllShared> shared;
    std::vector<Task<T>> slots;
    std::vector<Waker> wakers;
    std::vector<size_t> freeSlots;
    size_t count = 0;

public:
    TaskSet();
    TaskSet(TaskSet &&) = default;
    TaskSet & operator=(TaskSet &&) = default;

    /**
     * Add a task. The task does not run until the owner awaits `next()`.
     */
    void push(Task<T> task);

    /**
     * The number of tasks in the set that did not complete yet.
     */
    size_t size() const noexcept
    {
        return count;
    }

    bool empty() const noexcept
    {
        return count == 0;
    }

    /**
     * Await the next task to complete and return its result. Returns
     * `std::nullopt` if the set is empty. If the task throws, `next()`
     * rethrows the exception and the other tasks stay in the set.
     *
     * The returned task refers to this set, so the set must outlive it.
     */
    Task<std::optional<T>> next();
};

/**
 * Await two tasks concurrently. The first one to complete wins and the
 * other one is cancelled. Index 0 of the result is the first task's
 * result, index 1 the second's; `void` results are `std::monostate`.
 */
template<typename A, typename B>
Task<std::variant<
    std::conditional_t<std::is_void_v<A>, std::monostate, A>,
    std::conditional_t<std::is_void_v<B>, std::monostate, B>>>
race(Task<A> a, Task<B> b);

/**
 * Box a task as a C ABI future (`future-abi.h`), for code in another
 * language to poll. The future's `take` moves the result out, or returns
 * the task's exception as a `nix_exception`.
 *
 * `T` must have a Rust counterpart (`nix::rs::AbiType<T>`).
 */
template<typename T>
nix_future toCFuture(Task<T> task);

/**
 * The result of a completed C ABI future, from any language. Rethrows the
 * future's exception with its dynamic type. An error that started in Rust
 * arrives as a `nix::Error`.
 *
 * Checks that the future's output type is `T`. Does not drop the future.
 */
template<typename T>
T takeCFuture(const nix_future & future);

/**
 * Await a C ABI future from a task, from any language. The task owns the
 * future and drops it when the task completes or is cancelled, so
 * cancelling the task cancels the future.
 */
template<typename T>
Task<T> awaitCFuture(nix_future future);

namespace detail {

/**
 * Register the factory that turns Rust errors into `nix::Error`
 * (`nix_set_rust_error_factory`). Idempotent. `takeCFuture` calls it, so
 * that a Rust error reaching C++ through a future is a `nix::Error`.
 */
void registerRustErrorFactory();

/**
 * Abort with a message about the mismatch. Not inline, to keep the
 * template code small.
 */
[[noreturn]] void abortOutputTypeMismatch(const char * expected, const char * actual);

} // namespace detail

} // namespace nix

#include "nix/util/task-impl.hh"

#pragma once
///@file Template definitions for task.hh. Include task.hh instead.

#include "nix/util/callback.hh"

#include <cstring>
#include <functional>
#include <future>
#include <new>

namespace nix {

namespace detail {

/**
 * Drop a waker outside of a lock: dropping the last reference can destroy
 * the object that owns the task, which can reenter the leaf.
 */
inline void replaceWaker(Waker & slot, const Waker & waker, Waker & old)
{
    if (!slot.willWake(waker))
        old = std::exchange(slot, waker);
}

template<typename T, typename F>
struct CallbackLeaf : LeafAwaiter
{
    using Canceller = std::invoke_result_t<F &, Callback<T>>;
    static constexpr bool cancellable = !std::is_void_v<Canceller>;

    F initiate;
    std::shared_ptr<CallbackState<T>> state = std::make_shared<CallbackState<T>>();
    std::optional<std::conditional_t<cancellable, Canceller, std::monostate>> canceller;
    bool initiated = false;

    explicit CallbackLeaf(F initiate)
        : initiate(std::move(initiate))
    {
    }

    CallbackLeaf(const CallbackLeaf &) = delete;

    ~CallbackLeaf()
    {
        if (!initiated)
            return;
        bool complete;
        Waker old;
        {
            std::lock_guard lock(state->mutex);
            state->dropped = true;
            complete = state->result.index() != 0;
            old = std::move(state->waker);
        }
        if constexpr (cancellable)
            if (!complete && canceller)
                (*canceller)();
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        if (!initiated) {
            initiated = true;
            Callback<T> callback{[state = state](std::future<T> fut) {
                Waker toWake;
                {
                    std::lock_guard lock(state->mutex);
                    if (state->dropped)
                        return;
                    try {
                        state->result.template emplace<1>(fut.get());
                    } catch (...) {
                        state->result.template emplace<2>(std::current_exception());
                    }
                    toWake = std::move(state->waker);
                }
                std::move(toWake).wake();
            }};
            if constexpr (cancellable)
                canceller.emplace(initiate(std::move(callback)));
            else
                initiate(std::move(callback));
        }

        Waker old;
        std::lock_guard lock(state->mutex);
        if (state->result.index() != 0)
            return NIX_POLL_READY;
        replaceWaker(state->waker, waker, old);
        return NIX_POLL_PENDING;
    }

    T await_resume()
    {
        std::lock_guard lock(state->mutex);
        if (auto * e = std::get_if<2>(&state->result))
            std::rethrow_exception(*e);
        return std::move(std::get<1>(state->result));
    }
};

struct WhenAllShared
{
    std::mutex mutex;
    std::vector<size_t> woken;
    Waker parent;
};

struct WhenAllChild : WakeTarget
{
    std::shared_ptr<WhenAllShared> shared;
    size_t index;

    WhenAllChild(std::shared_ptr<WhenAllShared> shared, size_t index)
        : shared(std::move(shared))
        , index(index)
    {
    }

    void wake() noexcept override;
};

template<typename T>
struct WhenAllLeaf : LeafAwaiter
{
    static constexpr bool isVoid = std::is_void_v<T>;

    std::vector<Task<T>> tasks;
    std::vector<std::conditional_t<isVoid, std::monostate, std::optional<T>>> results;
    std::vector<Waker> childWakers;
    std::shared_ptr<WhenAllShared> shared = std::make_shared<WhenAllShared>();
    size_t remaining;
    std::exception_ptr error;
    bool first = true;

    explicit WhenAllLeaf(std::vector<Task<T>> tasks_)
        : tasks(std::move(tasks_))
        , results(tasks.size())
        , remaining(tasks.size())
    {
        childWakers.reserve(tasks.size());
        for (size_t i = 0; i < tasks.size(); ++i)
            childWakers.push_back(makeWakeTarget<WhenAllChild>(shared, i)->waker());
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        std::vector<size_t> toPoll;
        Waker old;
        {
            std::lock_guard lock(shared->mutex);
            replaceWaker(shared->parent, waker, old);
            toPoll = std::exchange(shared->woken, {});
        }

        if (first) {
            first = false;
            toPoll.clear();
            for (size_t i = 0; i < tasks.size(); ++i)
                toPoll.push_back(i);
        }

        for (auto i : toPoll) {
            auto & task = tasks[i];
            if (!task)
                continue;
            if (task.poll(childWakers[i]) == NIX_POLL_PENDING)
                continue;
            try {
                if constexpr (isVoid)
                    task.takeResult();
                else
                    results[i].emplace(task.takeResult());
            } catch (...) {
                error = std::current_exception();
                /* Cancel the others. */
                tasks.clear();
                return NIX_POLL_READY;
            }
            task.reset();
            --remaining;
        }

        return remaining == 0 ? NIX_POLL_READY : NIX_POLL_PENDING;
    }

    auto await_resume()
    {
        if (error)
            std::rethrow_exception(error);
        if constexpr (!isVoid) {
            std::vector<T> res;
            res.reserve(results.size());
            for (auto & r : results)
                res.push_back(std::move(*r));
            return res;
        }
    }
};

template<typename T>
using VoidToMonostate = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

template<typename A, typename B>
struct RaceLeaf : LeafAwaiter
{
    Task<A> a;
    Task<B> b;
    std::exception_ptr error;
    std::optional<std::variant<VoidToMonostate<A>, VoidToMonostate<B>>> result;

    RaceLeaf(Task<A> a, Task<B> b)
        : a(std::move(a))
        , b(std::move(b))
    {
    }

    template<size_t I, typename T>
    void tryTake(Task<T> & task)
    {
        try {
            if constexpr (std::is_void_v<T>) {
                task.takeResult();
                result.emplace(std::in_place_index<I>);
            } else
                result.emplace(std::in_place_index<I>, task.takeResult());
        } catch (...) {
            error = std::current_exception();
        }
    }

    nix_poll pollLeaf(const Waker & waker) override
    {
        /* Poll both every time. Two children do not justify per-child
           wakers. */
        if (a.poll(waker) == NIX_POLL_READY) {
            tryTake<0>(a);
            b.reset();
            return NIX_POLL_READY;
        }
        if (b.poll(waker) == NIX_POLL_READY) {
            tryTake<1>(b);
            a.reset();
            return NIX_POLL_READY;
        }
        return NIX_POLL_PENDING;
    }

    std::variant<VoidToMonostate<A>, VoidToMonostate<B>> await_resume()
    {
        if (error)
            std::rethrow_exception(error);
        return std::move(*result);
    }
};

} // namespace detail

template<typename T, typename F>
Task<T> callbackToTask(F initiate)
{
    co_return co_await detail::CallbackLeaf<T, F>(std::move(initiate));
}

template<typename T>
Task<std::conditional_t<std::is_void_v<T>, void, std::vector<T>>> whenAll(std::vector<Task<T>> tasks)
{
    if constexpr (std::is_void_v<T>)
        co_await detail::WhenAllLeaf<T>(std::move(tasks));
    else
        co_return co_await detail::WhenAllLeaf<T>(std::move(tasks));
}

template<typename A, typename B>
Task<std::variant<
    std::conditional_t<std::is_void_v<A>, std::monostate, A>,
    std::conditional_t<std::is_void_v<B>, std::monostate, B>>>
race(Task<A> a, Task<B> b)
{
    co_return co_await detail::RaceLeaf<A, B>(std::move(a), std::move(b));
}

namespace detail {

template<typename T>
struct CFutureTask
{
    /* `noexcept`, because Rust calls these through `extern "C"`; see
       `nix/rust/future-abi.h`. */
    static nix_poll poll(void * self, const nix_waker * waker) noexcept
    {
        return static_cast<Task<T> *>(self)->poll(Waker::borrow(waker));
    }

    static nix_exception * take(void * self, void * out) noexcept
    {
        try {
            if constexpr (std::is_void_v<T>)
                static_cast<Task<T> *>(self)->takeResult();
            else
                /* The receiver owns the value from now on. For the cxx
                   types (`rust::String`, `rust::Box`), a moved-from C++
                   object releases nothing, and the bits are what Rust
                   expects. */
                new (out) T(static_cast<Task<T> *>(self)->takeResult());
            return nullptr;
        } catch (...) {
            return rs::currentException();
        }
    }

    static void drop(void * self) noexcept
    {
        delete static_cast<Task<T> *>(self);
    }

    static constexpr nix_future_vtable vtable{
        .poll = &poll,
        .take = &take,
        .drop = &drop,
        .output_type = rs::AbiType<T>::name,
    };
};

struct CFutureLeaf : LeafAwaiter
{
    nix_future future;
    bool complete = false;

    explicit CFutureLeaf(nix_future future)
        : future(future)
    {
    }

    CFutureLeaf(const CFutureLeaf &) = delete;

    ~CFutureLeaf() override;

    nix_poll pollLeaf(const Waker & waker) override;

    void await_resume() noexcept {}
};

} // namespace detail

template<typename T>
nix_future toCFuture(Task<T> task)
{
    return nix_future{
        .self = new Task<T>(std::move(task)),
        .vtable = &detail::CFutureTask<T>::vtable,
    };
}

template<typename T>
T takeCFuture(const nix_future & future)
{
    if (std::strcmp(future.vtable->output_type, rs::AbiType<T>::name) != 0)
        detail::abortOutputTypeMismatch(rs::AbiType<T>::name, future.vtable->output_type);

    detail::registerRustErrorFactory();

    if constexpr (std::is_void_v<T>) {
        if (auto e = future.vtable->take(future.self, nullptr))
            rs::rethrowException(e);
    } else {
        alignas(T) unsigned char storage[sizeof(T)];
        if (auto e = future.vtable->take(future.self, storage))
            rs::rethrowException(e);
        /* `take` placed a value of type `T` in `storage`. */
        T * placed = std::launder(reinterpret_cast<T *>(storage));
        T result(std::move(*placed));
        placed->~T();
        return result;
    }
}

template<typename T>
Task<T> awaitCFuture(nix_future future)
{
    detail::CFutureLeaf leaf(future);
    co_await leaf;

    /* The future is complete but not dropped yet: `takeCFuture` reads its
       result, and the drop follows at the end of this scope, also when
       `takeCFuture` throws. */
    struct Dropper
    {
        nix_future f;

        ~Dropper()
        {
            f.vtable->drop(f.self);
        }
    } dropper{future};

    co_return takeCFuture<T>(future);
}

} // namespace nix

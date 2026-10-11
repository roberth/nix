#include "nix/util/reactor.hh"
#include "nix/util/error.hh"
#include "nix/util/logging.hh"
#include "nix/util/util.hh"

namespace nix {

namespace {

struct FunctionCallback final : rs::ReactorCallback
{
    std::function<void()> f;

    explicit FunctionCallback(std::function<void()> f)
        : f(std::move(f))
    {
    }

    void run() const noexcept override
    {
        try {
            f();
        } catch (...) {
            /* The reactor thread has nobody to report to. */
            ignoreExceptionInDestructor();
        }
    }
};

struct FunctionFdCallback final : rs::FdCallback
{
    std::function<void(bool, bool)> f;

    explicit FunctionFdCallback(std::function<void(bool, bool)> f)
        : f(std::move(f))
    {
    }

    void ready(bool readable, bool writable) const noexcept override
    {
        try {
            f(readable, writable);
        } catch (...) {
            ignoreExceptionInDestructor();
        }
    }
};

struct DebugSlowPollHook final : rs::SlowPollHook
{
    void slowPoll(::rust::Str what, uint64_t micros) const noexcept override
    {
        try {
            debug(
                "the reactor thread was busy for %d ms in one %s; it reacted to nothing else in that time",
                micros / 1000,
                std::string_view(what.data(), what.size()));
        } catch (...) {
        }
    }
};

/**
 * Call a bridge function of the reactor, and rethrow the original of its
 * error: a `nix::RustError` or a `nix::RustPanic` (`rs::call`).
 *
 * The first call also lets the watchdog of the reactor log slow polls: a
 * function-local static, because a registration at load time does not
 * survive a static link that drops the object file.
 */
template<typename F>
auto callReactor(F && f) -> decltype(f())
{
    detail::registerRustErrorFactory();
    [[maybe_unused]] static const bool watchdog = [] {
        rs::call([] { rs::reactor_set_slow_poll_hook(std::make_unique<DebugSlowPollHook>()); });
        return true;
    }();
    return rs::call(std::forward<F>(f));
}

} // namespace

ReactorStats reactorStats()
{
    auto s = callReactor([] { return rs::reactor_stats(); });
    return ReactorStats{
        .uptime = std::chrono::microseconds(s.uptime_us),
        .busy = std::chrono::microseconds(s.busy_us),
        .polls = s.polls,
        .longestPoll = std::chrono::microseconds(s.max_poll_us),
        .busiestSecond = std::chrono::microseconds(s.max_busy_per_second_us),
        .slowPolls = s.slow_polls,
    };
}

bool onReactorThread()
{
    return callReactor([] { return rs::on_reactor_thread(); });
}

void detail::runOnReactorImpl(Task<void> task)
{
    if (onReactorThread())
        throw Error("cannot block the reactor thread to run a task on it");
    runBlocking(awaitCFuture<void>(callReactor([&] { return rs::reactor_spawn(toCFuture(std::move(task))); })));
}

Task<void> sleepFor(std::chrono::milliseconds duration)
{
    co_await awaitCFuture<void>(callReactor([&] { return rs::reactor_sleep(std::max<int64_t>(0, duration.count())); }));
}

Task<FdReadiness> waitFd(int fd, bool readable, bool writable)
{
    auto ready =
        co_await awaitCFuture<uint64_t>(callReactor([&] { return rs::reactor_wait_fd(fd, readable, writable); }));
    co_return FdReadiness{.readable = (ready & 1) != 0, .writable = (ready & 2) != 0};
}

void postToReactor(std::function<void()> f)
{
    callReactor([&] { rs::reactor_post(std::make_unique<FunctionCallback>(std::move(f))); });
}

CleanupScope CleanupScope::current()
{
    return CleanupScope(callReactor([] { return rs::cleanup_scope_current(); }));
}

CleanupScope::operator bool() const
{
    return callReactor([&] { return scope->isSome(); });
}

void CleanupScope::defer(Task<void> cleanup) const
{
    assert(*this);
    callReactor([&] { scope->defer(toCFuture(std::move(cleanup))); });
}

ReactorTimer::ReactorTimer(std::function<void()> onExpiry)
    : timer(callReactor([&] { return rs::reactor_timer(std::make_unique<FunctionCallback>(std::move(onExpiry))); }))
{
}

void ReactorTimer::set(std::chrono::milliseconds delay)
{
    callReactor([&] { timer->set(std::max<int64_t>(0, delay.count())); });
}

void ReactorTimer::cancel()
{
    callReactor([&] { timer->set(-1); });
}

FdWatch::FdWatch(int fd, std::function<void(bool readable, bool writable)> onReady)
    : watch(callReactor(
          [&] { return rs::reactor_watch_fd(fd, std::make_unique<FunctionFdCallback>(std::move(onReady))); }))
{
}

void FdWatch::setInterest(bool readable, bool writable)
{
    callReactor([&] { watch->setInterest(readable, writable); });
}

} // namespace nix

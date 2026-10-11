#include "nix/util/reactor.hh"
#include "nix/util/error.hh"
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

} // namespace

bool onReactorThread()
{
    return rs::on_reactor_thread();
}

void detail::runOnReactorImpl(Task<void> task)
{
    if (onReactorThread())
        throw Error("cannot block the reactor thread to run a task on it");
    runBlocking(awaitCFuture<void>(rs::reactor_spawn(toCFuture(std::move(task)))));
}

Task<void> sleepFor(std::chrono::milliseconds duration)
{
    co_await awaitCFuture<void>(rs::reactor_sleep(std::max<int64_t>(0, duration.count())));
}

Task<FdReadiness> waitFd(int fd, bool readable, bool writable)
{
    auto ready = co_await awaitCFuture<uint64_t>(rs::reactor_wait_fd(fd, readable, writable));
    co_return FdReadiness{.readable = (ready & 1) != 0, .writable = (ready & 2) != 0};
}

void postToReactor(std::function<void()> f)
{
    rs::reactor_post(std::make_unique<FunctionCallback>(std::move(f)));
}

CleanupScope CleanupScope::current()
{
    return CleanupScope(rs::cleanup_scope_current());
}

CleanupScope::operator bool() const
{
    return scope->isSome();
}

void CleanupScope::defer(Task<void> cleanup) const
{
    assert(*this);
    scope->defer(toCFuture(std::move(cleanup)));
}

ReactorTimer::ReactorTimer(std::function<void()> onExpiry)
    : timer(rs::reactor_timer(std::make_unique<FunctionCallback>(std::move(onExpiry))))
{
}

void ReactorTimer::set(std::chrono::milliseconds delay)
{
    timer->set(std::max<int64_t>(0, delay.count()));
}

void ReactorTimer::cancel()
{
    timer->set(-1);
}

static ::rust::Box<rs::FdWatch> makeFdWatch(int fd, std::function<void(bool, bool)> onReady)
{
    try {
        return rs::reactor_watch_fd(fd, std::make_unique<FunctionFdCallback>(std::move(onReady)));
    } catch (::rust::Error & e) {
        throw Error("%s", e.what());
    }
}

FdWatch::FdWatch(int fd, std::function<void(bool readable, bool writable)> onReady)
    : watch(makeFdWatch(fd, std::move(onReady)))
{
}

void FdWatch::setInterest(bool readable, bool writable)
{
    watch->setInterest(readable, writable);
}

} // namespace nix

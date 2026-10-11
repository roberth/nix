/**
 * Evaluations on fibers that suspend in `builtins.fetchTarball`.
 *
 * A server on the loopback interface holds each response until the test
 * releases it, so the test decides when a fetch completes.
 */

#ifndef _WIN32

#  include <gtest/gtest.h>

#  include "nix/expr/tests/libexpr.hh"
#  include "nix/expr/eval-gc.hh"
#  include "nix/fetchers/tarball-cache-gix.hh"
#  include "nix/util/environment-variables.hh"
#  include "nix/util/fiber.hh"
#  include "nix/util/file-descriptor.hh"
#  include "nix/util/file-system.hh"
#  include "nix/util/fmt.hh"

#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>

#  include <chrono>
#  include <condition_variable>
#  include <list>
#  include <map>
#  include <mutex>
#  include <set>
#  include <thread>

namespace nix {

namespace {

/**
 * A tar archive with the given files under the directory `src`.
 */
std::string makeTar(const std::map<std::string, std::string> & files)
{
    auto header = [](const std::string & name, size_t size, char type) {
        std::string h(512, '\0');
        auto put = [&](size_t offset, const std::string & s) { h.replace(offset, s.size(), s); };
        put(0, name);
        put(100, type == '5' ? "0000755" : "0000644");
        put(108, "0000000");
        put(116, "0000000");
        put(124, fmt("%011o", size));
        put(136, fmt("%011o", 1700000000));
        put(148, "        ");
        h[156] = type;
        put(257,
            std::string(
                "ustar\0"
                "00",
                8));
        unsigned int sum = 0;
        for (unsigned char c : h)
            sum += c;
        put(148, fmt("%06o", sum));
        h[154] = '\0';
        h[155] = ' ';
        return h;
    };

    std::string tar = header("src/", 0, '5');
    for (auto & [name, contents] : files) {
        tar += header("src/" + name, contents.size(), '0');
        tar += contents;
        tar += std::string((512 - contents.size() % 512) % 512, '\0');
    }
    tar += std::string(1024, '\0');
    return tar;
}

/**
 * An HTTP/1.1 server on the loopback interface. It answers a request for
 * a path in `responses` only after `release(path)`. It records the
 * requests, and the connections that the client closes while a request
 * waits.
 */
struct GatedServer
{
    std::map<std::string, std::string> responses;
    AutoCloseFD listener;
    uint16_t port = 0;

    std::mutex mutex;
    std::condition_variable changed;
    std::set<std::string> requested, released, closedWhileWaiting;
    bool stopping = false;

    std::list<std::thread> threads;

    explicit GatedServer(std::map<std::string, std::string> responses)
        : responses(std::move(responses))
    {
        listener = AutoCloseFD(::socket(AF_INET, SOCK_STREAM, 0));
        if (!listener)
            throw SysError("creating socket");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener.get(), reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
            throw SysError("binding socket");
        if (::listen(listener.get(), 16) != 0)
            throw SysError("listening on socket");
        socklen_t len = sizeof(addr);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&addr), &len) != 0)
            throw SysError("getting socket name");
        port = ntohs(addr.sin_port);
        threads.emplace_back([this]() { acceptLoop(); });
    }

    ~GatedServer()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_all();
        for (auto & t : threads)
            t.join();
    }

    std::string url(const std::string & path) const
    {
        return fmt("http://127.0.0.1:%d%s", port, path);
    }

    bool isStopping()
    {
        std::lock_guard lock(mutex);
        return stopping;
    }

    static bool readable(int fd, int timeoutMs)
    {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        return ::poll(&pfd, 1, timeoutMs) > 0;
    }

    void acceptLoop()
    {
        while (!isStopping()) {
            if (!readable(listener.get(), 50))
                continue;
            int fd = ::accept(listener.get(), nullptr, nullptr);
            if (fd < 0)
                continue;
            std::lock_guard lock(mutex);
            threads.emplace_back([this, fd]() { serve(AutoCloseFD(fd)); });
        }
    }

    void serve(AutoCloseFD conn)
    {
        std::string buffer;
        char chunk[4096];
        while (!isStopping()) {
            auto end = buffer.find("\r\n\r\n");
            if (end == std::string::npos) {
                if (!readable(conn.get(), 50))
                    continue;
                auto n = ::recv(conn.get(), chunk, sizeof(chunk), 0);
                if (n <= 0)
                    return;
                buffer.append(chunk, n);
                continue;
            }
            auto request = buffer.substr(0, end);
            buffer.erase(0, end + 4);
            auto firstSpace = request.find(' ');
            auto secondSpace = request.find(' ', firstSpace + 1);
            auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

            {
                std::lock_guard lock(mutex);
                requested.insert(path);
            }
            changed.notify_all();

            /* Wait for the release, and watch for a close by the client
               meanwhile. */
            while (true) {
                {
                    std::unique_lock lock(mutex);
                    if (stopping)
                        return;
                    if (released.count(path))
                        break;
                }
                if (readable(conn.get(), 20)) {
                    auto n = ::recv(conn.get(), chunk, sizeof(chunk), MSG_PEEK);
                    if (n <= 0) {
                        {
                            std::lock_guard lock(mutex);
                            closedWhileWaiting.insert(path);
                        }
                        changed.notify_all();
                        return;
                    }
                }
            }

            auto response = responses.find(path);
            if (response == responses.end())
                writeFull(conn.get(), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
            else
                writeFull(
                    conn.get(),
                    fmt("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", response->second.size()) + response->second);
        }
    }

    bool waitFor(std::function<bool()> pred)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(10), pred);
    }

    bool waitRequested(const std::string & path)
    {
        return waitFor([&]() { return requested.count(path) > 0; });
    }

    bool waitClosed(const std::string & path)
    {
        return waitFor([&]() { return closedWhileWaiting.count(path) > 0; });
    }

    void release(const std::string & path)
    {
        {
            std::lock_guard lock(mutex);
            released.insert(path);
        }
        changed.notify_all();
    }
};

/**
 * A waker that a test thread can wait for.
 */
struct WaitableWaker : WakeTarget
{
    std::mutex mutex;
    std::condition_variable cv;
    bool woken = false;

    void wake() noexcept override
    {
        {
            std::lock_guard lock(mutex);
            woken = true;
        }
        cv.notify_all();
    }

    /**
     * Wait for a wake, for at most 10 seconds. Returns false on a timeout.
     */
    bool wait()
    {
        std::unique_lock lock(mutex);
        bool res = cv.wait_for(lock, std::chrono::seconds(10), [&]() { return woken; });
        woken = false;
        return res;
    }
};

/**
 * Poll `task` each time it wakes, until it is ready or `stop()` holds
 * after a poll. Returns whether it is ready. Fails the test after 10
 * seconds without a wake.
 */
template<typename T>
bool pollUntil(Task<T> & task, WaitableWaker & target, std::function<bool()> stop = []() { return false; })
{
    auto waker = target.waker();
    while (true) {
        if (task.poll(waker) == NIX_POLL_READY)
            return true;
        if (stop())
            return false;
        if (!target.wait()) {
            ADD_FAILURE() << "no wake within 10 seconds";
            return false;
        }
    }
}

struct FrameLog
{
    std::mutex mutex;
    std::vector<std::string> entries;

    void push(std::string s)
    {
        std::lock_guard lock(mutex);
        entries.push_back(std::move(s));
    }

    std::vector<std::string> get()
    {
        std::lock_guard lock(mutex);
        return entries;
    }
};

struct FrameGuard
{
    FrameLog & log;
    std::string name;

    ~FrameGuard()
    {
        log.push(name + " destroyed");
    }
};

} // namespace

class FiberFetchTest : public LibExprTest
{
protected:
    FiberFetchTest()
        : LibExprTest(openStore("dummy://?read-only=false"), [](bool & readOnlyMode) {
            EvalSettings settings{readOnlyMode};
            settings.nixPath = {};
            return settings;
        })
    {
    }

    void SetUp() override
    {
        if (auto reason = fibersDisabledReason())
            GTEST_SKIP() << *reason;
        if (!fetchers::gix::useGixTarballCache())
            GTEST_SKIP() << "only the gix tarball cache fetches as a task";
        /* The tarball cache and the fetcher cache, in a directory of this
           process. The package build runs the tests with a home
           directory that does not exist. */
        static AutoDelete cacheDir(createTempDir());
        setEnv("NIX_CACHE_HOME", cacheDir.path().string().c_str());
    }

    /**
     * Poll `task` once, and fail if it completes, with its exception if
     * it has one.
     */
    template<typename T>
    static void expectPending(Task<T> & task, WaitableWaker & target)
    {
        if (task.poll(target.waker()) == NIX_POLL_PENDING)
            return;
        try {
            task.takeResult();
            FAIL() << "the task completed without suspending";
        } catch (std::exception & e) {
            FAIL() << "the task failed without suspending: " << e.what();
        }
    }

    const std::string tarball = makeTar({{"hello", "world\n"}});

    /**
     * Evaluate `expr` to a string, as a task on a fiber.
     */
    Task<std::string> evalOnFiber(EvalState & st, std::string expr)
    {
        return runOnFiber([&st, expr]() {
            Value v;
            st.eval(st.parseExprFromString(expr, st.rootPath(CanonPath::root)), v);
            st.forceValue(v, noPos);
            return std::string(v.string_view());
        });
    }

    std::string readHello(const std::string & url)
    {
        return fmt("builtins.readFile \"${builtins.fetchTarball \"%s\"}/hello\"", url);
    }
};

/**
 * The evaluation suspends in the fetch: the first poll returns pending
 * while the server holds the response, and the fiber is in use. The
 * release wakes the task, and the evaluation continues with the
 * contents of the tarball.
 */
TEST_F(FiberFetchTest, suspendsInTheFetchAndResumes)
{
    GatedServer server({{"/a.tar", tarball}});
    auto inUse = fiberStackStats().inUse;

    auto task = evalOnFiber(state, readHello(server.url("/a.tar")));
    auto target = makeWakeTarget<WaitableWaker>();
    ASSERT_NO_FATAL_FAILURE(expectPending(task, *target));
    ASSERT_TRUE(server.waitRequested("/a.tar"));
    EXPECT_EQ(fiberStackStats().inUse, inUse + 1);

    server.release("/a.tar");
    ASSERT_TRUE(pollUntil(task, *target));
    EXPECT_EQ(task.takeResult(), "world\n");
    EXPECT_EQ(fiberStackStats().inUse, inUse);
}

/**
 * Dropping the evaluation while it waits in the fetch stops the transfer
 * (the client closes its connection), unwinds the frames of the
 * evaluator on the fiber, and returns the stack to the pool. The thunk
 * that was forced can be forced again, and a collection afterwards
 * leaves the state usable.
 */
TEST_F(FiberFetchTest, dropCancelsTheFetchAndUnwindsTheEvaluator)
{
    GatedServer server({{"/b.tar", tarball}});
    auto inUse = fiberStackStats().inUse;
    FrameLog log;

    /* A primop that holds a C++ object in its frame while it forces its
       argument, as the frames of the evaluator do. */
    PrimOp guardPrimOp{
        .name = "guard",
        .arity = 1,
        .impl =
            [&log](EvalState & st, CallSite callSite, Value * const * args, Value & v) {
                FrameGuard guard{log, "primop frame"};
                st.forceValue(*args[0], noPos);
                v = *args[0];
            },
    };
    auto * guard = state.allocValue();
    guard->mkPrimOp(&guardPrimOp);

    auto * fetched = maybeThunk(fmt("builtins.fetchTarball \"%s\"", server.url("/b.tar")), false);

    {
        auto task = runOnFiber([&]() {
            Value v;
            state.callFunction(*guard, *fetched, v, noPos);
            log.push("not reached");
        });
        auto target = makeWakeTarget<WaitableWaker>();
        ASSERT_NO_FATAL_FAILURE(expectPending(task, *target));
        ASSERT_TRUE(server.waitRequested("/b.tar"));
        EXPECT_EQ(log.get(), std::vector<std::string>{});

        auto start = std::chrono::steady_clock::now();
        task.reset();
        auto elapsed = std::chrono::steady_clock::now() - start;
        std::cerr << fmt(
            "dropping the suspended evaluation took %d µs\n",
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
    }

    EXPECT_EQ(log.get(), std::vector<std::string>{"primop frame destroyed"});
    EXPECT_TRUE(server.waitClosed("/b.tar"));
    EXPECT_EQ(fiberStackStats().inUse, inUse);

    GC_gcollect();

    /* The interrupted thunk is forced again, this time to the end. */
    auto * readHelloFn = state.allocValue();
    state.eval(
        state.parseExprFromString("p: builtins.readFile \"${p}/hello\"", state.rootPath(CanonPath::root)),
        *readHelloFn);
    server.release("/b.tar");
    auto task = runOnFiber([&]() {
        Value g;
        state.callFunction(*guard, *fetched, g, noPos);
        Value r;
        state.callFunction(*readHelloFn, g, r, noPos);
        state.forceValue(r, noPos);
        return std::string(r.string_view());
    });
    auto target = makeWakeTarget<WaitableWaker>();
    ASSERT_TRUE(pollUntil(task, *target));
    EXPECT_EQ(task.takeResult(), "world\n");
    EXPECT_EQ(log.get(), (std::vector<std::string>{"primop frame destroyed", "primop frame destroyed"}));
}

/**
 * Two evaluations, each with its own `EvalState`, run on one thread. Both
 * wait in their fetches at the same time, and they complete in the order
 * of the releases, not in the order in which they started.
 */
TEST_F(FiberFetchTest, twoEvaluationsInterleaveOnOneThread)
{
    GatedServer server({{"/c.tar", tarball}, {"/d.tar", makeTar({{"hello", "other\n"}})}});
    EvalState state2({}, state.store, fetchSettings, evalSettings, nullptr);

    FrameLog log;
    auto thread = std::this_thread::get_id();
    auto one = [&](EvalState & st, std::string name, std::string path) {
        return runOnFiber([&st, &log, &server, &thread, name, path]() {
            log.push(name + " started");
            Value v;
            st.eval(
                st.parseExprFromString(
                    fmt("builtins.readFile \"${builtins.fetchTarball \"%s\"}/hello\"", server.url(path)),
                    st.rootPath(CanonPath::root)),
                v);
            st.forceValue(v, noPos);
            EXPECT_EQ(std::this_thread::get_id(), thread);
            log.push(name + " done");
            return std::string(v.string_view());
        });
    };

    std::vector<Task<std::string>> tasks;
    tasks.push_back(one(state, "c", "/c.tar"));
    tasks.push_back(one(state2, "d", "/d.tar"));
    auto all = whenAll(std::move(tasks));

    auto target = makeWakeTarget<WaitableWaker>();
    ASSERT_NO_FATAL_FAILURE(expectPending(all, *target));
    ASSERT_TRUE(server.waitRequested("/c.tar"));
    ASSERT_TRUE(server.waitRequested("/d.tar"));
    EXPECT_EQ(log.get(), (std::vector<std::string>{"c started", "d started"}));

    server.release("/d.tar");
    ASSERT_FALSE(pollUntil(all, *target, [&]() { return log.get().size() == 3; }));
    EXPECT_EQ(log.get(), (std::vector<std::string>{"c started", "d started", "d done"}));

    server.release("/c.tar");
    ASSERT_TRUE(pollUntil(all, *target));
    EXPECT_EQ(log.get(), (std::vector<std::string>{"c started", "d started", "d done", "c done"}));
    EXPECT_EQ(all.takeResult(), (std::vector<std::string>{"world\n", "other\n"}));
}

} // namespace nix

#endif

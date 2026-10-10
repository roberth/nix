#ifndef _WIN32

#  include <gtest/gtest.h>

#  include "nix/store/filetransfer.hh"
#  include "nix/store/tests/https-store.hh"
#  include "nix/util/file-descriptor.hh"
#  include "nix/util/fmt.hh"
#  include "nix/rust/future_probe.rs.h"

#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>

#  include <future>
#  include <map>
#  include <thread>

namespace nix {

namespace {

/**
 * An HTTP/1.1 server on the loopback interface. It answers requests for
 * the paths in `responses`, and stalls on any other path: it never
 * answers, and records whether the client closes the connection.
 *
 * It serves one connection, which is sufficient because curl reuses it
 * for consecutive requests.
 */
struct StallingServer
{
    std::map<std::string, std::string> responses;
    AutoCloseFD listener;
    uint16_t port = 0;
    std::promise<void> stalled;
    std::promise<std::string> outcome;
    std::thread thread;

    explicit StallingServer(std::map<std::string, std::string> responses = {})
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
        if (::listen(listener.get(), 1) != 0)
            throw SysError("listening on socket");

        socklen_t len = sizeof(addr);
        if (::getsockname(listener.get(), reinterpret_cast<sockaddr *>(&addr), &len) != 0)
            throw SysError("getting socket name");
        port = ntohs(addr.sin_port);

        thread = std::thread([this]() { outcome.set_value(serve()); });
    }

    ~StallingServer()
    {
        thread.join();
    }

    /**
     * Wait until `fd` is readable, for at most 10 seconds, so that a
     * broken cancellation fails the test instead of hanging it.
     */
    static bool waitReadable(int fd)
    {
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        return ::poll(&pfd, 1, 10000) > 0;
    }

    std::string serve()
    {
        if (!waitReadable(listener.get()))
            return "no connection";
        AutoCloseFD conn(::accept(listener.get(), nullptr, nullptr));
        if (!conn)
            return "accept failed";

        std::string buffer;
        char chunk[4096];
        while (true) {
            auto end = buffer.find("\r\n\r\n");
            if (end != std::string::npos) {
                auto request = buffer.substr(0, end);
                buffer.erase(0, end + 4);
                auto firstSpace = request.find(' ');
                auto secondSpace = request.find(' ', firstSpace + 1);
                auto path = request.substr(firstSpace + 1, secondSpace - firstSpace - 1);

                auto response = responses.find(path);
                if (response != responses.end()) {
                    writeFull(
                        conn.get(),
                        fmt("HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n%s",
                            response->second.size(),
                            response->second));
                    continue;
                }

                stalled.set_value();
                /* Never answer; wait for the client to close. */
                while (true) {
                    if (!waitReadable(conn.get()))
                        return "still open after 10 s";
                    auto n = ::recv(conn.get(), chunk, sizeof(chunk), 0);
                    if (n == 0)
                        return "closed by client";
                    if (n < 0)
                        return "recv failed";
                }
            }

            if (!waitReadable(conn.get()))
                return "no request";
            auto n = ::recv(conn.get(), chunk, sizeof(chunk), 0);
            if (n <= 0)
                return "closed before request";
            buffer.append(chunk, n);
        }
    }

    /**
     * Wait for the outcome, after the test cancelled the stalled request.
     */
    std::string result()
    {
        auto f = outcome.get_future();
        if (f.wait_for(std::chrono::seconds(15)) != std::future_status::ready)
            return "server thread did not finish";
        return f.get();
    }
};

struct CountingTarget : WakeTarget
{
    std::atomic<int> count = 0;

    void wake() noexcept override
    {
        count++;
    }
};

} // namespace

TEST(FileTransferCancel, droppingTheTaskClosesTheConnection)
{
    StallingServer server;
    auto fileTransfer = makeFileTransfer();

    FileTransferRequest request(VerbatimURL(fmt("http://127.0.0.1:%d/stall", server.port)));
    auto task = fileTransfer->transfer(std::move(request));

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    /* The worker thread starts the transfer and sends the request. */
    ASSERT_EQ(server.stalled.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);

    /* Cancel by destroying the task. */
    task.reset();

    EXPECT_EQ(server.result(), "closed by client");

    /* The transfer failed with `Cancelled` on the worker thread, but the
       task was gone, so nothing was woken. */
    EXPECT_EQ(target->count, 0);
}

/**
 * The transfer is a C++ task, which a Rust future awaits, which a C++
 * task awaits. Dropping the outer C++ task drops the Rust future, which
 * drops the transfer task, which removes the transfer from curl.
 */
TEST(FileTransferCancel, droppingARustFutureThatAwaitsTheTransferClosesTheConnection)
{
    StallingServer server;
    auto fileTransfer = makeFileTransfer();

    FileTransferRequest request(VerbatimURL(fmt("http://127.0.0.1:%d/stall", server.port)));
    auto transfer = [](Task<FileTransferResult> transfer) -> Task<rust::String> {
        auto result = co_await std::move(transfer);
        co_return rust::String(result.data);
    }(fileTransfer->transfer(std::move(request)));

    struct Log : rs::ProbeLog
    {
        mutable std::vector<std::string> events;

        void push(rust::Str event) const override
        {
            events.emplace_back(event);
        }
    } log;

    auto task = awaitCFuture<rust::String>(rs::probe_future_forward(toCFuture(std::move(transfer)), log));

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    ASSERT_EQ(server.stalled.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);

    task.reset();

    EXPECT_EQ(server.result(), "closed by client");
    EXPECT_EQ(log.events, std::vector<std::string>{"rust: dropped"});
    EXPECT_EQ(target->count, 0);
}

/**
 * The chain that `querySubstitutablePathInfos` uses for each substituter:
 * `Store::queryPathInfoAsync` on an HTTP binary cache, down to the
 * transfer of the `.narinfo` file.
 */
TEST(HttpBinaryCacheStoreCancel, droppingQueryPathInfoClosesTheConnection)
{
    auto config = make_ref<testing::TestHttpBinaryCacheStoreConfig>(
        ParsedURL{
            .scheme = "http",
            .authority = ParsedURL::Authority{.host = "127.0.0.1"},
        },
        Store::Config::Params{});

    StallingServer server({{"/nix-cache-info", "StoreDir: " + config->storeDir + "\n"}});
    config->cacheUri.authority->port = server.port;

    auto fileTransfer = makeFileTransfer();
    auto store = config->openTestStore(fileTransfer);

    auto task = store->queryPathInfoAsync(StorePath{"g1w7hy3qg1w7hy3qg1w7hy3qg1w7hy3q-foo"});

    auto target = makeWakeTarget<CountingTarget>();
    EXPECT_EQ(task.poll(target->waker()), NIX_POLL_PENDING);

    ASSERT_EQ(server.stalled.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);

    task.reset();

    EXPECT_EQ(server.result(), "closed by client");
    EXPECT_EQ(target->count, 0);
}

} // namespace nix

#endif

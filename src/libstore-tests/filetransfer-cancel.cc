#ifndef _WIN32

#  include <gtest/gtest.h>

#  include "nix/store/filetransfer.hh"
#  include "nix/util/file-descriptor.hh"
#  include "nix/util/fmt.hh"

#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <poll.h>
#  include <sys/socket.h>

#  include <future>
#  include <thread>

namespace nix {

namespace {

/**
 * An HTTP server on the loopback interface that accepts one connection,
 * reads the request and never answers. It records whether the client
 * closed the connection.
 */
struct StallingServer
{
    AutoCloseFD listener;
    uint16_t port = 0;
    std::promise<void> accepted;
    std::promise<std::string> outcome;
    std::thread thread;

    StallingServer()
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

        thread = std::thread([this]() { serve(); });
    }

    ~StallingServer()
    {
        thread.join();
    }

    void serve()
    {
        AutoCloseFD conn(::accept(listener.get(), nullptr, nullptr));
        if (!conn) {
            outcome.set_value("accept failed");
            return;
        }
        accepted.set_value();

        /* Read the request and then wait for EOF. Give up after a
           timeout, so that a broken cancellation fails the test instead
           of hanging it. */
        char buf[4096];
        while (true) {
            pollfd pfd{.fd = conn.get(), .events = POLLIN, .revents = 0};
            int r = ::poll(&pfd, 1, 10000);
            if (r == 0) {
                outcome.set_value("still open after 10 s");
                return;
            }
            auto n = ::recv(conn.get(), buf, sizeof(buf), 0);
            if (n == 0) {
                outcome.set_value("closed by client");
                return;
            }
            if (n < 0) {
                outcome.set_value("recv failed");
                return;
            }
        }
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

    /* The worker thread starts the transfer and connects. */
    ASSERT_EQ(server.accepted.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);

    /* Cancel by destroying the task. */
    task.reset();

    auto outcome = server.outcome.get_future();
    ASSERT_EQ(outcome.wait_for(std::chrono::seconds(15)), std::future_status::ready);
    EXPECT_EQ(outcome.get(), "closed by client");

    /* The transfer failed with `Cancelled` on the worker thread, but the
       task was gone, so nothing was woken. */
    EXPECT_EQ(target->count, 0);
}

} // namespace nix

#endif

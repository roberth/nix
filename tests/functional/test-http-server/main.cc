/**
 * A small HTTP/1.1 server for the functional tests, which serves the files
 * of a directory and can stall in the middle of a response.
 *
 * Usage: test-http-server ROOT PORT_FILE LOG_FILE STALL_AFTER
 *
 * - The server listens on a free port of 127.0.0.1, and writes the port to
 *   PORT_FILE when it accepts connections.
 * - `GET /NAME` returns the file ROOT/NAME.
 * - `GET /stall/NAME` returns the first STALL_AFTER bytes of ROOT/NAME,
 *   and then sends nothing more, until the client closes the connection.
 * - Each line of LOG_FILE is an event: `served NAME N`, `stalled NAME N`,
 *   `closed by client NAME N`, where N is the number of bytes of the body
 *   that the server sent.
 *
 * The server runs until it is killed. It needs no Nix library, so that it
 * stays independent of the code under test.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string root;
std::string logFile;
size_t stallAfter = 0;
std::mutex logMutex;

void log(const std::string & event)
{
    std::lock_guard lock(logMutex);
    std::ofstream out(logFile, std::ios::app);
    out << event << "\n";
}

bool sendAll(int fd, const char * data, size_t size)
{
    while (size > 0) {
        /* `main` ignores SIGPIPE, so a closed connection is an error here. */
        auto n = ::send(fd, data, size, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        size -= n;
    }
    return true;
}

/**
 * Wait until the client closes the connection, and drain what it sends.
 */
void waitForClose(int fd)
{
    char buf[4096];
    while (true) {
        auto n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0 || (n < 0 && errno != EINTR))
            return;
    }
}

void respond(int fd, const std::string & path, bool & keepOpen)
{
    bool stall = path.starts_with("/stall/");
    auto name = path.substr(stall ? 7 : 1);

    std::ifstream file(root + "/" + name, std::ios::binary);
    if (!file || name.find("..") != std::string::npos) {
        std::string response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
        keepOpen = sendAll(fd, response.data(), response.size());
        return;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    auto body = contents.str();

    auto header = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (!sendAll(fd, header.data(), header.size())) {
        keepOpen = false;
        return;
    }

    size_t limit = stall ? std::min(stallAfter, body.size()) : body.size();
    size_t sent = 0;
    while (sent < limit) {
        auto chunk = std::min<size_t>(64 * 1024, limit - sent);
        if (!sendAll(fd, body.data() + sent, chunk)) {
            log("closed by client " + name + " " + std::to_string(sent));
            keepOpen = false;
            return;
        }
        sent += chunk;
    }

    if (stall) {
        log("stalled " + name + " " + std::to_string(sent));
        waitForClose(fd);
        log("closed by client " + name + " " + std::to_string(sent));
        keepOpen = false;
        return;
    }

    log("served " + name + " " + std::to_string(sent));
}

void serve(int fd)
{
    std::string buffer;
    char chunk[4096];
    bool keepOpen = true;
    while (keepOpen) {
        auto end = buffer.find("\r\n\r\n");
        if (end == std::string::npos) {
            auto n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
                break;
            buffer.append(chunk, n);
            continue;
        }
        auto request = buffer.substr(0, end);
        buffer.erase(0, end + 4);
        auto firstSpace = request.find(' ');
        auto secondSpace = request.find(' ', firstSpace + 1);
        respond(fd, request.substr(firstSpace + 1, secondSpace - firstSpace - 1), keepOpen);
    }
    ::close(fd);
}

} // namespace

int main(int argc, char ** argv)
{
    if (argc != 5) {
        std::fprintf(stderr, "usage: %s ROOT PORT_FILE LOG_FILE STALL_AFTER\n", argv[0]);
        return 2;
    }
    root = argv[1];
    std::string portFile = argv[2];
    logFile = argv[3];
    stallAfter = std::stoul(argv[4]);

    signal(SIGPIPE, SIG_IGN);

    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 || ::listen(listener, 64) != 0) {
        std::perror("test-http-server: listening");
        return 1;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len);

    /* Write the port atomically, so that a test that polls for the file
       never reads half of it. */
    {
        std::ofstream out(portFile + ".tmp");
        out << ntohs(addr.sin_port) << "\n";
    }
    std::rename((portFile + ".tmp").c_str(), portFile.c_str());

    while (true) {
        int fd = ::accept(listener, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            std::perror("test-http-server: accept");
            return 1;
        }
        std::thread(serve, fd).detach();
    }
}

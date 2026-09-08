// libev C++17 example: TCP echo server over BSD sockets.
//
// Mirrors examples/c/lwip-echo.c in C++: one ev_io on the listening fd
// (accept), one ev_io per accepted connection (echo). Connections live in a
// fixed-capacity pool (zero heap), unlike the C version's calloc/free. A
// detached loopback client self-checks the echo path end-to-end. State and
// callbacks are members of EchoServer; the pool is indexed, not pointer-chased.
// SPDX-License-Identifier: MIT

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "ev_raii.hpp"

namespace {

constexpr int kListenPort = 7700;
constexpr uint16_t kBufSize = 1024U;
constexpr uint16_t kMaxConns = 16U;
constexpr uint16_t kNoConn = kMaxConns;   // alloc sentinel

class EchoServer {
public:
    EchoServer() noexcept : listen_(loop_, this), timeout_(loop_, this) {}

    int run() noexcept;

private:
    struct Conn {
        ev_io io;
        bool used = false;
        char buf[kBufSize];
    };

    static void set_nonblock(int fd) noexcept;
    static void conn_thunk(struct ev_loop* loop, ev_io* w, int revents) noexcept;
    static void* client_main(void* arg) noexcept;

    void accept_cb(ev_io& w, uint32_t revents) noexcept;
    void timeout_cb(ev_timer& w, uint32_t revents) noexcept;
    void handle_conn(Conn& c, ev_io& w, uint32_t revents) noexcept;

    uint16_t alloc_conn() noexcept;
    void free_conn(Conn& c) noexcept;

    evx::Loop loop_;
    std::array<Conn, kMaxConns> conns_;
    evx::Io<EchoServer, &EchoServer::accept_cb> listen_;
    evx::Timer<EchoServer, &EchoServer::timeout_cb> timeout_;
    bool client_pass_ = false;
};

void EchoServer::set_nonblock(int fd) noexcept
{
    const int flags = fcntl(fd, F_GETFL, 0);
    static_cast<void>(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

uint16_t EchoServer::alloc_conn() noexcept
{
    for (uint16_t i = 0U; i < kMaxConns; ++i)
    {
        if (!conns_[i].used)
        {
            conns_[i].used = true;
            return i;
        }
    }
    return kNoConn;
}

void EchoServer::free_conn(Conn& c) noexcept
{
    ev_io_stop(loop_.raw(), &c.io);
    close(c.io.fd);
    c.used = false;
}

void EchoServer::conn_thunk(struct ev_loop*, ev_io* w, int revents) noexcept
{
    EchoServer* self = static_cast<EchoServer*>(w->data);

    for (Conn& c : self->conns_)
    {
        if (&c.io == w)
        {
            self->handle_conn(c, *w, static_cast<uint32_t>(revents));
            return;
        }
    }
}

void EchoServer::handle_conn(Conn& c, ev_io& w, uint32_t revents) noexcept
{
    if (revents & EV_READ)
    {
        const ssize_t n = recv(w.fd, c.buf, kBufSize, 0);

        if (n <= 0)
        {
            if (0 == n || EAGAIN != errno)
            {
                free_conn(c);
                return;
            }
        }
        else
        {
            ssize_t off = 0;

            while (off < n)
            {
                const ssize_t m = send(w.fd, c.buf + off, static_cast<size_t>(n - off), 0);

                if (m < 0)
                {
                    if (EAGAIN == errno) { continue; }
                    free_conn(c);
                    return;
                }

                off += m;
            }

            std::printf("[lwip-echo] echoed %d bytes\n", static_cast<int>(n));
        }
    }
}

void EchoServer::accept_cb(ev_io& w, uint32_t revents) noexcept
{
    (void)revents;

    for (;;)
    {
        const int cfd = accept(w.fd, nullptr, nullptr);

        if (cfd < 0)
        {
            if (EAGAIN == errno || EWOULDBLOCK == errno) { break; }
            if (EINTR == errno) { continue; }
            break;
        }

        set_nonblock(cfd);
        {
            const int one = 1;
            static_cast<void>(setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)));
        }

        const uint16_t idx = alloc_conn();
        if (kNoConn == idx)
        {
            close(cfd);
            continue;
        }

        Conn& c = conns_[idx];
        ev_io_init(&c.io, conn_thunk, cfd, EV_READ);
        c.io.data = this;
        ev_io_start(loop_.raw(), &c.io);

        std::printf("[lwip-echo] accepted fd=%d\n", cfd);
    }
}

void EchoServer::timeout_cb(ev_timer&, uint32_t) noexcept
{
    loop_.break_loop();
}

void* EchoServer::client_main(void* arg) noexcept
{
    EchoServer* self = static_cast<EchoServer*>(arg);
    const char msg[] = "hello lwip";
    char buf[64] = {0};

    usleep(200000);   /* let the echo server arm */

    const int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0)
    {
        return nullptr;
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(static_cast<uint16_t>(kListenPort));

    if (connect(cfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        close(cfd);
        return nullptr;
    }

    static_cast<void>(send(cfd, msg, sizeof(msg) - 1U, 0));
    const ssize_t n = recv(cfd, buf, sizeof(buf) - 1U, 0);

    self->client_pass_ = (n > 0) && (0 == std::strcmp(buf, msg));
    std::printf("[lwip-echo] client got \"%s\"\n", buf);

    close(cfd);
    return nullptr;
}

int EchoServer::run() noexcept
{
    const int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
    {
        std::printf("socket failed\n");
        return 1;
    }
    {
        const int one = 1;
        static_cast<void>(setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)));
    }

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(kListenPort));

    if (bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::printf("bind failed\n");
        close(lfd);
        return 1;
    }
    if (listen(lfd, kMaxConns) < 0)
    {
        std::printf("listen failed\n");
        close(lfd);
        return 1;
    }
    set_nonblock(lfd);

    listen_.start(lfd, EV_READ);
    timeout_.start(2.0, 0.0);

    pthread_t client;
    static_cast<void>(pthread_create(&client, nullptr, client_main, this));

    std::printf("=== libev C++17 lwip-echo demo ===\n");
    std::printf("[lwip-echo] echo server on port %d, backend 0x%x\n",
                kListenPort, loop_.backend());

    loop_.run(0);

    static_cast<void>(pthread_join(client, nullptr));

    const bool pass = client_pass_;
    std::printf("LWIP_ECHO_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close(lfd);
    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    EchoServer server;
    return server.run();
}

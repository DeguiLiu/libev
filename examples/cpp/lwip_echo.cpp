// libev C++17 example: TCP echo server over BSD sockets.
//
// Mirrors examples/c/lwip-echo.c in C++: one ev_io on the listening fd
// (accept), one ev_io per accepted connection (echo). Connections live in a
// fixed-capacity pool (zero heap), unlike the C version's calloc/free. A
// detached loopback client self-checks the echo path end-to-end.
// SPDX-License-Identifier: MIT

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

struct Conn {
    ev_io io;
    bool used;
    char buf[kBufSize];
};

Conn g_conns[kMaxConns];
evx::Loop* g_loop = nullptr;
bool g_client_pass = false;

void set_nonblock(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    static_cast<void>(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

Conn* alloc_conn()
{
    for (uint16_t i = 0U; i < kMaxConns; ++i)
    {
        if (!g_conns[i].used)
        {
            g_conns[i].used = true;
            return &g_conns[i];
        }
    }
    return nullptr;
}

void free_conn(Conn* c)
{
    ev_io_stop(g_loop->raw(), &c->io);
    close(c->io.fd);
    c->used = false;
}

void conn_cb(struct ev_loop*, ev_io* w, int revents)
{
    Conn* c = static_cast<Conn*>(w->data);

    if (revents & EV_READ)
    {
        const ssize_t n = recv(w->fd, c->buf, kBufSize, 0);

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
                const ssize_t m = send(w->fd, c->buf + off, static_cast<size_t>(n - off), 0);

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

void accept_cb(ev_io* w, int revents)
{
    (void)revents;

    for (;;)
    {
        const int cfd = accept(w->fd, nullptr, nullptr);

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

        Conn* c = alloc_conn();
        if (nullptr == c)
        {
            close(cfd);
            continue;
        }

        ev_io_init(&c->io, conn_cb, cfd, EV_READ);
        c->io.data = c;
        ev_io_start(g_loop->raw(), &c->io);

        std::printf("[lwip-echo] accepted fd=%d\n", cfd);
    }
}

void* client_main(void*)
{
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

    g_client_pass = (n > 0) && (0 == std::strcmp(buf, msg));
    std::printf("[lwip-echo] client got \"%s\"\n", buf);

    close(cfd);
    return nullptr;
}

void timeout_cb(ev_timer*, int)
{
    g_loop->break_loop();
}

}  // namespace

int main()
{
    evx::Loop loop;
    if (!loop.valid())
    {
        std::printf("ev_loop_new failed\n");
        return 1;
    }
    g_loop = &loop;

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

    evx::Io<accept_cb> listen_w(loop);
    listen_w.start(lfd, EV_READ);

    evx::Timer<timeout_cb> timeout(loop);
    timeout.start(2.0, 0.0);

    pthread_t client;
    static_cast<void>(pthread_create(&client, nullptr, client_main, nullptr));

    std::printf("=== libev C++17 lwip-echo demo ===\n");
    std::printf("[lwip-echo] echo server on port %d, backend 0x%x\n",
                kListenPort, loop.backend());

    loop.run(0);

    static_cast<void>(pthread_join(client, nullptr));

    const bool pass = g_client_pass;
    std::printf("LWIP_ECHO_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close(lfd);
    return pass ? 0 : 1;
}

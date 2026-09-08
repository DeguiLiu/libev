// libev C++17 example: HSM connection lifecycle + echo socket.
//
// Mirrors examples/c/hsm-echo.c in C++. The C file's embedded HSM engine is
// replaced by hsm.hpp. State topology: TOP -> ESTABLISHED -> CONNECTED ->
// RECEIVING / FLUSHING, plus CLOSING / FAILED siblings under ESTABLISHED.
// Connections live in a fixed-capacity pool (zero heap). A kSettle signal
// re-resolves RECEIVING vs FLUSHING from the send-queue state after each I/O.
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
#include "hsm.hpp"

namespace {

constexpr int kListenPort = 7701;
constexpr uint16_t kBufSize = 1024U;
constexpr uint16_t kQueueCap = 8192U;
constexpr uint16_t kMaxConns = 16U;
constexpr uint16_t kNoConn = kMaxConns;

enum Signal : uint16_t {
    kAccepted = 1U,
    kData = 2U,
    kWritable = 3U,
    kEof = 4U,
    kError = 5U,
    kSettle = 6U
};

constexpr int8_t kTop = 0;
constexpr int8_t kEstablished = 1;
constexpr int8_t kConnected = 2;
constexpr int8_t kReceiving = 3;
constexpr int8_t kFlushing = 4;
constexpr int8_t kClosing = 5;
constexpr int8_t kFailed = 6;

class HsmEchoServer;

struct Conn;

/* ---- HSM entry/exit/action/guard: table data, reach the loop via Conn::owner ---- */

void entry_connected(Conn&);
void entry_rw(Conn&);
void entry_failed(Conn&);
void exit_established(Conn&);
void act_echo(Conn&, uint16_t);
void act_try_drain(Conn&, uint16_t);
bool guard_queue_empty(const Conn&, uint16_t);
bool guard_queue_nonempty(const Conn&, uint16_t);

const hsm::StateDef<Conn> kStates[] = {
    { -1, nullptr, nullptr, "TOP" },
    { kTop, nullptr, exit_established, "ESTABLISHED" },
    { kEstablished, entry_connected, nullptr, "CONNECTED" },
    { kConnected, nullptr, nullptr, "RECEIVING" },
    { kConnected, entry_rw, nullptr, "FLUSHING" },
    { kEstablished, entry_rw, nullptr, "CLOSING" },
    { kEstablished, entry_failed, nullptr, "FAILED" },
};

const hsm::TransitionDef<Conn> kTransitions[] = {
    { kReceiving, kData, -1, hsm::TransitionKind::Internal, nullptr, act_echo },
    { kReceiving, kSettle, kFlushing, hsm::TransitionKind::External, guard_queue_nonempty, nullptr },
    { kReceiving, kSettle, -1, hsm::TransitionKind::Internal, guard_queue_empty, nullptr },

    { kFlushing, kWritable, -1, hsm::TransitionKind::Internal, nullptr, act_try_drain },
    { kFlushing, kSettle, kReceiving, hsm::TransitionKind::External, guard_queue_empty, nullptr },
    { kFlushing, kSettle, -1, hsm::TransitionKind::Internal, guard_queue_nonempty, nullptr },

    { kConnected, kEof, kClosing, hsm::TransitionKind::External, guard_queue_empty, nullptr },

    { kClosing, kWritable, -1, hsm::TransitionKind::Internal, nullptr, act_try_drain },
    { kClosing, kEof, -1, hsm::TransitionKind::Internal, nullptr, act_try_drain },

    { kEstablished, kError, kFailed, hsm::TransitionKind::External, nullptr, nullptr },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

struct Conn {
    HsmEchoServer* owner = nullptr;
    hsm::Hsm<Conn> hsm;
    ev_io io;
    char queue[kQueueCap];
    size_t q_len = 0U;
    char buf[kBufSize];
    uint64_t rx = 0U;
    uint64_t tx = 0U;
    ssize_t last_read = 0;
    bool used = false;

    Conn() noexcept
        : hsm(kStates, kNumStates, kTransitions, kNumTransitions, kReceiving, /*max_depth=*/4U)
    {
    }
};

class HsmEchoServer {
public:
    HsmEchoServer() noexcept : listen_(loop_, this), timeout_(loop_, this) {}

    int run() noexcept;

    struct ev_loop* raw() noexcept { return loop_.raw(); }

    static void arm(Conn& c, int events) noexcept;

private:
    static void set_nonblock(int fd) noexcept;
    static void conn_thunk(struct ev_loop* loop, ev_io* w, int revents) noexcept;
    static void* client_main(void* arg) noexcept;

    void accept_cb(ev_io& w, uint32_t revents) noexcept;
    void timeout_cb(ev_timer& w, uint32_t revents) noexcept;
    void handle_conn(Conn& c, ev_io& w, uint32_t revents) noexcept;

    uint16_t alloc_conn() noexcept;
    void teardown(Conn& c) noexcept;

    evx::Loop loop_;
    std::array<Conn, kMaxConns> conns_;
    evx::Io<HsmEchoServer, &HsmEchoServer::accept_cb> listen_;
    evx::Timer<HsmEchoServer, &HsmEchoServer::timeout_cb> timeout_;
    bool client_pass_ = false;
};

void HsmEchoServer::arm(Conn& c, int events) noexcept
{
    struct ev_loop* loop = c.owner->raw();
    ev_io_stop(loop, &c.io);
    ev_io_set(&c.io, c.io.fd, events);
    ev_io_start(loop, &c.io);
}

void HsmEchoServer::set_nonblock(int fd) noexcept
{
    const int flags = fcntl(fd, F_GETFL, 0);
    static_cast<void>(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

uint16_t HsmEchoServer::alloc_conn() noexcept
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

void HsmEchoServer::teardown(Conn& c) noexcept
{
    ev_io_stop(loop_.raw(), &c.io);
    close(c.io.fd);
    std::printf("[hsm] connection done: rx=%lu tx=%lu\n",
                static_cast<unsigned long>(c.rx), static_cast<unsigned long>(c.tx));
    c.used = false;
}

/* ---- HSM entry/exit/action/guard (table data, reach loop via Conn::owner) ---- */

void entry_connected(Conn& ctx)
{
    HsmEchoServer::arm(ctx, EV_READ);
}

void entry_rw(Conn& ctx)
{
    HsmEchoServer::arm(ctx, EV_READ | EV_WRITE);
}

void exit_established(Conn& ctx)
{
    ev_io_stop(ctx.owner->raw(), &ctx.io);
    close(ctx.io.fd);
}

void entry_failed(Conn& ctx)
{
    std::printf("[hsm] FAILED state entered (errno=%d)\n", errno);
    ev_io_stop(ctx.owner->raw(), &ctx.io);
    close(ctx.io.fd);
}

bool try_flush(Conn& c)
{
    size_t off = 0U;

    while (off < c.q_len)
    {
        const ssize_t m = send(c.io.fd, c.queue + off, c.q_len - off, 0);

        if (m < 0)
        {
            if (EAGAIN == errno) { break; }
            return false;
        }

        off += static_cast<size_t>(m);
        c.tx += static_cast<uint64_t>(m);
    }

    if (off == c.q_len)
    {
        c.q_len = 0U;
    }
    else if (off > 0U)
    {
        std::memmove(c.queue, c.queue + off, c.q_len - off);
        c.q_len -= off;
    }

    return true;
}

void act_echo(Conn& ctx, uint16_t)
{
    const size_t len = static_cast<size_t>(ctx.last_read);

    if (len > kQueueCap - ctx.q_len)
    {
        std::printf("[hsm] queue full, dropping %zu bytes (backpressure)\n", len);
        return;
    }

    std::memcpy(ctx.queue + ctx.q_len, ctx.buf, len);
    ctx.q_len += len;
    ctx.rx += static_cast<uint64_t>(len);
}

void act_try_drain(Conn& ctx, uint16_t)
{
    if (!try_flush(ctx))
    {
        ctx.hsm.dispatch(ctx, kError);
    }
}

bool guard_queue_empty(const Conn& ctx, uint16_t)
{
    return 0U == ctx.q_len;
}

bool guard_queue_nonempty(const Conn& ctx, uint16_t)
{
    return 0U < ctx.q_len;
}

/* ---- libev callbacks ---- */

void HsmEchoServer::conn_thunk(struct ev_loop*, ev_io* w, int revents) noexcept
{
    HsmEchoServer* self = static_cast<HsmEchoServer*>(w->data);

    for (Conn& c : self->conns_)
    {
        if (&c.io == w)
        {
            self->handle_conn(c, *w, static_cast<uint32_t>(revents));
            return;
        }
    }
}

void HsmEchoServer::handle_conn(Conn& c, ev_io& w, uint32_t revents) noexcept
{
    if (revents & EV_ERROR)
    {
        c.hsm.dispatch(c, kError);
        return;
    }

    if (revents & EV_READ)
    {
        const ssize_t n = recv(w.fd, c.buf, kBufSize, 0);

        if (n > 0)
        {
            c.last_read = n;
            c.hsm.dispatch(c, kData);

            if (!try_flush(c))
            {
                c.hsm.dispatch(c, kError);
                teardown(c);
                return;
            }

            c.hsm.dispatch(c, kSettle);
        }
        else if (0 == n)
        {
            c.hsm.dispatch(c, kEof);

            if (c.hsm.current_state() == kClosing && 0U == c.q_len)
            {
                teardown(c);
                return;
            }
        }
        else if (EAGAIN != errno && EINTR != errno)
        {
            c.hsm.dispatch(c, kError);
            teardown(c);
            return;
        }
    }

    if (revents & EV_WRITE)
    {
        c.hsm.dispatch(c, kWritable);

        if (c.hsm.current_state() == kClosing && 0U == c.q_len)
        {
            teardown(c);
            return;
        }

        c.hsm.dispatch(c, kSettle);
    }
}

void HsmEchoServer::accept_cb(ev_io& w, uint32_t revents) noexcept
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
        c.owner = this;
        c.io.fd = cfd;
        c.q_len = 0U;
        c.rx = 0U;
        c.tx = 0U;
        ev_io_init(&c.io, conn_thunk, cfd, EV_READ);
        c.io.data = this;
        c.hsm.init(c);

        std::printf("[hsm] accepted fd=%d state=%s\n", cfd, c.hsm.current_state_name());
    }
}

void HsmEchoServer::timeout_cb(ev_timer&, uint32_t) noexcept
{
    loop_.break_loop();
}

void* HsmEchoServer::client_main(void* arg) noexcept
{
    HsmEchoServer* self = static_cast<HsmEchoServer*>(arg);
    const char msg[] = "hello hsm";
    char buf[64] = {0};

    usleep(200000);

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

    close(cfd);
    return nullptr;
}

int HsmEchoServer::run() noexcept
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

    std::printf("=== libev C++17 hsm-echo demo ===\n");
    std::printf("hsm echo server on port %d (backend 0x%x)\n", kListenPort, loop_.backend());

    loop_.run(0);

    static_cast<void>(pthread_join(client, nullptr));

    const bool pass = client_pass_;
    std::printf("HSM_ECHO_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close(lfd);
    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    HsmEchoServer server;
    return server.run();
}

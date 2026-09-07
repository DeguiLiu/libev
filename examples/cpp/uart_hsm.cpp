// libev C++17 example: UART protocol parsing driven by ev_io + HSM.
//
// Mirrors examples/c/uart-hsm/uart-hsm.c in C++. The C state_machine engine
// is replaced by hsm.hpp (via hsm_parser.hpp), the C hsm_parser/uart_protocol
// by uart_protocol.hpp + hsm_parser.hpp. A pipe stands in for the UART fd;
// ev_io feeds bytes into the parser, a test timer drives 4 good frames then
// garbage frames, and a self-check asserts the parser survives them.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "ev_raii.hpp"
#include "hsm_parser.hpp"

namespace {

constexpr int kRd = 0;
constexpr int kWr = 1;

uart::HsmParser g_parser;
evx::Loop* g_loop = nullptr;
int g_pipe[2] = {-1, -1};
uint32_t g_frames_rx = 0U;
uint32_t g_frames_sent = 0U;
uint32_t g_round = 0U;

struct TestCase {
    const char* name;
    uint8_t cmd_class;
    uint8_t cmd;
    uint8_t data[8];
    uint16_t data_len;
};

const TestCase kTests[] = {
    { "sys get info", uart::kClassSys, uart::kSysGetInfo, { 0x01, 0x00 }, 2U },
    { "spi read id", uart::kClassSpi, uart::kSpiReadId, { 0x00, 0x00 }, 2U },
    { "spi read", uart::kClassSpi, uart::kSpiRead, { 0, 0, 0, 0, 0, 0, 16, 0 }, 8U },
    { "ota start", uart::kClassOta, uart::kOtaStart, { 0, 0, 0, 16, 0, 0, 0, 0 }, 8U },
};

void frame_cb(const uart::Frame& frame, void*)
{
    std::printf("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
                static_cast<unsigned>(frame.cmd_class),
                static_cast<unsigned>(frame.cmd),
                static_cast<unsigned>(frame.data_len));
    ++g_frames_rx;
}

void uart_rx_cb(ev_io* w, int)
{
    uint8_t buf[128];

    for (;;)
    {
        const ssize_t n = read(w->fd, buf, sizeof(buf));

        if (n > 0)
        {
            g_parser.put_data(buf, static_cast<uint32_t>(n));
        }
        else if (n < 0 && EAGAIN == errno)
        {
            break;
        }
        else if (n < 0 && EINTR == errno)
        {
            continue;
        }
        else
        {
            ev_io_stop(g_loop->raw(), w);
            break;
        }
    }
}

void send_frame(const TestCase& t)
{
    uint8_t buf[64];
    const uint32_t len = uart::build_frame(buf, t.cmd_class, t.cmd, t.data, t.data_len);

    std::printf("[TEST] %-14s -> %u bytes\n", t.name, static_cast<unsigned>(len));
    static_cast<void>(write(g_pipe[kWr], buf, len));
    ++g_frames_sent;
}

void send_garbage()
{
    static const uint8_t bad1[] = { 0xBB, 0xCC };
    uint8_t bad2[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0xFF, 0xFF, 0x55 };
    uint8_t bad3[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0, 0, 0x66 };
    const uint16_t crc = uart::crc16(&bad3[3], 2U);

    bad3[5] = static_cast<uint8_t>(crc & 0xFFU);
    bad3[6] = static_cast<uint8_t>((crc >> 8) & 0xFFU);

    std::printf("[TEST] garbage frames (bad hdr / bad crc / bad tail)\n");
    static_cast<void>(write(g_pipe[kWr], bad1, sizeof(bad1)));
    static_cast<void>(write(g_pipe[kWr], bad2, sizeof(bad2)));
    static_cast<void>(write(g_pipe[kWr], bad3, sizeof(bad3)));
}

void stop_cb(ev_timer*, int)
{
    g_loop->break_loop();
}

evx::Timer<stop_cb>* g_stop = nullptr;

void test_timer_cb(ev_timer* w, int)
{
    if (g_round < 4U)
    {
        send_frame(kTests[g_round]);
    }
    else if (4U == g_round)
    {
        send_garbage();
    }

    ++g_round;

    if (g_round > 5U)
    {
        ev_timer_stop(g_loop->raw(), w);
        if (nullptr != g_stop)
        {
            g_stop->start(0.3, 0.0);
        }
    }
}

}  // namespace

int main()
{
    if (pipe(g_pipe) < 0)
    {
        std::printf("pipe failed\n");
        return 1;
    }
    {
        const int flags = fcntl(g_pipe[kRd], F_GETFL, 0);
        static_cast<void>(fcntl(g_pipe[kRd], F_SETFL, flags | O_NONBLOCK));
    }

    evx::Loop loop;
    if (!loop.valid())
    {
        std::printf("ev_loop_new failed\n");
        return 1;
    }
    g_loop = &loop;

    g_parser.init(frame_cb, nullptr);

    evx::Io<uart_rx_cb> uart_w(loop);
    uart_w.start(g_pipe[kRd], EV_READ);

    evx::Timer<test_timer_cb> test_w(loop);
    test_w.start(0.05, 0.1);

    evx::Timer<stop_cb> stop(loop);
    g_stop = &stop;

    std::printf("=== libev C++17 uart-hsm demo ===\n");
    loop.run(0);

    const uart::Stats& stats = g_parser.stats();

    std::printf("\n--- results ---\n");
    std::printf("frames sent (good)      : %u\n", static_cast<unsigned>(g_frames_sent));
    std::printf("frames parsed           : %u\n", static_cast<unsigned>(stats.frames_received));
    std::printf("bytes received          : %u\n", static_cast<unsigned>(stats.bytes_received));
    std::printf("sync/crc/tail errors    : %u / %u / %u\n",
                static_cast<unsigned>(stats.sync_errors),
                static_cast<unsigned>(stats.crc_errors),
                static_cast<unsigned>(stats.tail_errors));

    const bool pass =
        (g_frames_rx == g_frames_sent) &&
        (stats.crc_errors >= 1U) &&
        (stats.tail_errors >= 1U) &&
        (stats.sync_errors >= 1U);

    std::printf("UART_HSM_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close(g_pipe[kRd]);
    close(g_pipe[kWr]);

    return pass ? 0 : 1;
}

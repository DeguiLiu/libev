// libev C++17 example: UART protocol parsing driven by ev_io + HSM.
//
// Mirrors examples/c/uart-hsm/uart-hsm.c in C++. The C state_machine engine
// is replaced by hsm.hpp (via hsm_parser.hpp), the C hsm_parser/uart_protocol
// by uart_protocol.hpp + hsm_parser.hpp. A pipe stands in for the UART fd;
// ev_io feeds bytes into the parser, a test timer drives 4 good frames then
// garbage frames, and a self-check asserts the parser survives them.
// SPDX-License-Identifier: MIT

#include <array>
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

class UartHsmDemo {
public:
    UartHsmDemo() noexcept
        : uart_(loop_, this), test_(loop_, this), stop_(loop_, this)
    {
    }

    int run() noexcept;

private:
    static void frame_cb(const uart::Frame& frame, void* user_data) noexcept;
    void uart_rx_cb(ev_io& w, int revents) noexcept;
    void test_timer_cb(ev_timer& w, int revents) noexcept;
    void stop_cb(ev_timer& w, int revents) noexcept;
    void send_frame(const TestCase& t) noexcept;
    void send_garbage() noexcept;

    uart::HsmParser parser_;
    evx::Loop loop_;
    std::array<int, 2> pipe_ = {-1, -1};
    uint32_t frames_rx_ = 0U;
    uint32_t frames_sent_ = 0U;
    uint32_t round_ = 0U;
    evx::Io<UartHsmDemo, &UartHsmDemo::uart_rx_cb> uart_;
    evx::Timer<UartHsmDemo, &UartHsmDemo::test_timer_cb> test_;
    evx::Timer<UartHsmDemo, &UartHsmDemo::stop_cb> stop_;
};

void UartHsmDemo::frame_cb(const uart::Frame& frame, void* user_data) noexcept
{
    UartHsmDemo* self = static_cast<UartHsmDemo*>(user_data);

    std::printf("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
                static_cast<unsigned>(frame.cmd_class),
                static_cast<unsigned>(frame.cmd),
                static_cast<unsigned>(frame.data_len));
    ++self->frames_rx_;
}

void UartHsmDemo::uart_rx_cb(ev_io& w, int) noexcept
{
    uint8_t buf[128];

    for (;;)
    {
        const ssize_t n = read(w.fd, buf, sizeof(buf));

        if (n > 0)
        {
            parser_.put_data(buf, static_cast<uint32_t>(n));
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
            ev_io_stop(loop_.raw(), &w);
            break;
        }
    }
}

void UartHsmDemo::send_frame(const TestCase& t) noexcept
{
    uint8_t buf[64];
    const uint32_t len = uart::build_frame(buf, t.cmd_class, t.cmd, t.data, t.data_len);

    std::printf("[TEST] %-14s -> %u bytes\n", t.name, static_cast<unsigned>(len));
    static_cast<void>(write(pipe_[kWr], buf, len));
    ++frames_sent_;
}

void UartHsmDemo::send_garbage() noexcept
{
    static const uint8_t bad1[] = { 0xBB, 0xCC };
    uint8_t bad2[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0xFF, 0xFF, 0x55 };
    uint8_t bad3[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0, 0, 0x66 };
    const uint16_t crc = uart::crc16(&bad3[3], 2U);

    bad3[5] = static_cast<uint8_t>(crc & 0xFFU);
    bad3[6] = static_cast<uint8_t>((crc >> 8) & 0xFFU);

    std::printf("[TEST] garbage frames (bad hdr / bad crc / bad tail)\n");
    static_cast<void>(write(pipe_[kWr], bad1, sizeof(bad1)));
    static_cast<void>(write(pipe_[kWr], bad2, sizeof(bad2)));
    static_cast<void>(write(pipe_[kWr], bad3, sizeof(bad3)));
}

void UartHsmDemo::test_timer_cb(ev_timer& w, int) noexcept
{
    if (round_ < 4U)
    {
        send_frame(kTests[round_]);
    }
    else if (4U == round_)
    {
        send_garbage();
    }

    ++round_;

    if (round_ > 5U)
    {
        ev_timer_stop(loop_.raw(), &w);
        stop_.start(0.3, 0.0);
    }
}

void UartHsmDemo::stop_cb(ev_timer&, int) noexcept
{
    loop_.break_loop();
}

int UartHsmDemo::run() noexcept
{
    if (pipe(pipe_.data()) < 0)
    {
        std::printf("pipe failed\n");
        return 1;
    }
    {
        const int flags = fcntl(pipe_[kRd], F_GETFL, 0);
        static_cast<void>(fcntl(pipe_[kRd], F_SETFL, flags | O_NONBLOCK));
    }

    parser_.init(frame_cb, this);

    uart_.start(pipe_[kRd], EV_READ);
    test_.start(0.05, 0.1);

    std::printf("=== libev C++17 uart-hsm demo ===\n");
    loop_.run(0);

    const uart::Stats& stats = parser_.stats();

    std::printf("\n--- results ---\n");
    std::printf("frames sent (good)      : %u\n", static_cast<unsigned>(frames_sent_));
    std::printf("frames parsed           : %u\n", static_cast<unsigned>(stats.frames_received));
    std::printf("bytes received          : %u\n", static_cast<unsigned>(stats.bytes_received));
    std::printf("sync/crc/tail errors    : %u / %u / %u\n",
                static_cast<unsigned>(stats.sync_errors),
                static_cast<unsigned>(stats.crc_errors),
                static_cast<unsigned>(stats.tail_errors));

    const bool pass =
        (frames_rx_ == frames_sent_) &&
        (stats.crc_errors >= 1U) &&
        (stats.tail_errors >= 1U) &&
        (stats.sync_errors >= 1U);

    std::printf("UART_HSM_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close(pipe_[kRd]);
    close(pipe_[kWr]);

    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    UartHsmDemo demo;
    return demo.run();
}

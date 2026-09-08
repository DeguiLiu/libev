// libev C++17 example: UART parsing with an upstream ring buffer.
//
// Mirrors examples/c/uart-hsm/uart-ring-hsm.c in C++: a simulated ISR thread
// (producer) pushes frame bytes into an spsc ring in small "interrupt" chunks,
// then pokes the event loop with ev_async; the loop drains the ring into the
// shared HSM parser. The C spsc_queue (malloc + volatile) becomes a
// compile-time-capacity spsc::Ring (std::atomic, zero heap).
// SPDX-License-Identifier: MIT

#include <atomic>
#include <cstdint>
#include <cstdio>

#include <pthread.h>
#include <unistd.h>

#include "ev_raii.hpp"
#include "hsm_parser.hpp"
#include "spsc_ring.hpp"
#include "uart_protocol.hpp"

namespace {

constexpr uint32_t kRingSize = 4096U;   /* power of two */
constexpr uint32_t kPokeCount = 4U;     /* one frame in several interrupts */

struct TestCase {
    const char* name;
    uint8_t cls;
    uint8_t cmd;
};

class UartRingHsmDemo {
public:
    UartRingHsmDemo() noexcept : wake_(loop_, this), drain_(loop_, this) {}

    int run() noexcept;

private:
    static void frame_cb(const uart::Frame& frame, void* user_data) noexcept;
    static void* isr_main(void* arg) noexcept;

    void wake_cb(ev_async& w, uint32_t revents) noexcept;
    void drain_cb(ev_timer& w, uint32_t revents) noexcept;
    void drain_ring() noexcept;

    uart::HsmParser parser_;
    spsc::Ring<kRingSize> ring_;
    evx::Loop loop_;
    evx::Async<UartRingHsmDemo, &UartRingHsmDemo::wake_cb> wake_;
    evx::Timer<UartRingHsmDemo, &UartRingHsmDemo::drain_cb> drain_;
    uint32_t frames_rx_ = 0U;
    uint32_t dropped_ = 0U;              /* ring-full overflow count */
    std::atomic<bool> running_{true};
};

void UartRingHsmDemo::frame_cb(const uart::Frame& frame, void* user_data) noexcept
{
    UartRingHsmDemo* self = static_cast<UartRingHsmDemo*>(user_data);

    std::printf("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
                static_cast<unsigned>(frame.cmd_class),
                static_cast<unsigned>(frame.cmd),
                static_cast<unsigned>(frame.data_len));
    ++self->frames_rx_;
}

void* UartRingHsmDemo::isr_main(void* arg) noexcept
{
    UartRingHsmDemo* self = static_cast<UartRingHsmDemo*>(arg);
    const TestCase tests[] = {
        { "sys", uart::kClassSys, uart::kSysGetInfo },
        { "spi", uart::kClassSpi, uart::kSpiRead },
    };

    usleep(50000);   /* let the loop arm first */

    for (uint32_t i = 0U; i < 2U; ++i)
    {
        uint8_t frame[64];
        const uint32_t len = uart::build_frame(frame, tests[i].cls, tests[i].cmd, nullptr, 0U);
        uint32_t off = 0U;

        /* deliver the frame in kPokeCount chunks, one "interrupt" each */
        while (off < len)
        {
            const uint32_t n = (len - off > kPokeCount) ? kPokeCount : (len - off);

            if (!self->ring_.push(&frame[off], n))
            {
                self->dropped_ += n;
            }

            off += n;
            self->wake_.send();     /* wake the loop */
            usleep(1000);           /* next interrupt arrives later */
        }
    }

    self->running_.store(false);
    self->wake_.send();
    return nullptr;
}

void UartRingHsmDemo::drain_ring() noexcept
{
    uint8_t buf[64];
    uint32_t got;

    while (0U != (got = ring_.pop(buf, sizeof(buf))))
    {
        parser_.put_data(buf, got);
    }
}

void UartRingHsmDemo::wake_cb(ev_async&, uint32_t) noexcept
{
    drain_ring();

    if (!running_.load() && (0U == ring_.data_len()))
    {
        loop_.break_loop();
    }
}

void UartRingHsmDemo::drain_cb(ev_timer&, uint32_t) noexcept
{
    drain_ring();

    if (!running_.load() && (0U == ring_.data_len()))
    {
        loop_.break_loop();
    }
}

int UartRingHsmDemo::run() noexcept
{
    parser_.init(frame_cb, this);

    wake_.start();
    drain_.start(0.005, 0.005);

    pthread_t isr;
    static_cast<void>(pthread_create(&isr, nullptr, isr_main, this));

    std::printf("=== libev C++17 uart-ring-hsm demo ===\n");
    std::printf("[test] ISR -> ring(%uB) -> ev_async -> HSM\n", kRingSize);

    loop_.run(0);

    static_cast<void>(pthread_join(isr, nullptr));

    const uart::Stats& stats = parser_.stats();

    std::printf("\n--- results ---\n");
    std::printf("frames parsed       : %u\n", static_cast<unsigned>(stats.frames_received));
    std::printf("bytes received      : %u\n", static_cast<unsigned>(stats.bytes_received));
    std::printf("ring overflow bytes : %u\n", static_cast<unsigned>(dropped_));

    const bool pass = (2U == frames_rx_) && (0U == dropped_);
    std::printf("RING_HSM_CHECK: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    UartRingHsmDemo demo;
    return demo.run();
}

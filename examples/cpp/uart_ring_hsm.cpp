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

void wake_cb(ev_async*, int);
void drain_cb(ev_timer*, int);
void frame_cb(const uart::Frame&, void*);

uart::HsmParser g_parser;
spsc::Ring<kRingSize> g_ring;
evx::Loop* g_loop = nullptr;
evx::Async<wake_cb>* g_wake = nullptr;

uint32_t g_frames_rx = 0U;
uint32_t g_dropped = 0U;                 /* ring-full overflow count */
std::atomic<bool> g_running{true};

/* producer: simulate the UART ISR running on another thread/vector */
void* isr_main(void*)
{
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

            if (!g_ring.push(&frame[off], n))
            {
                g_dropped += n;
            }

            off += n;
            g_wake->send();     /* wake the loop */
            usleep(1000);       /* next interrupt arrives later */
        }
    }

    g_running.store(false);
    g_wake->send();
    return nullptr;
}

/* consumer: loop side -- drain the ring straight into the HSM */
void drain_ring()
{
    uint8_t buf[64];
    uint32_t got;

    while (0U != (got = g_ring.pop(buf, sizeof(buf))))
    {
        g_parser.put_data(buf, got);
    }
}

void wake_cb(ev_async*, int)
{
    drain_ring();

    if (!g_running.load() && (0U == g_ring.data_len()))
    {
        g_loop->break_loop();
    }
}

/* periodic drain: belt-and-suspenders for dropped wakeups */
void drain_cb(ev_timer*, int)
{
    drain_ring();

    if (!g_running.load() && (0U == g_ring.data_len()))
    {
        g_loop->break_loop();
    }
}

void frame_cb(const uart::Frame& frame, void*)
{
    std::printf("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
                static_cast<unsigned>(frame.cmd_class),
                static_cast<unsigned>(frame.cmd),
                static_cast<unsigned>(frame.data_len));
    ++g_frames_rx;
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

    g_parser.init(frame_cb, nullptr);

    evx::Async<wake_cb> wake(loop);
    g_wake = &wake;
    wake.start();

    evx::Timer<drain_cb> drain(loop);
    drain.start(0.005, 0.005);

    pthread_t isr;
    static_cast<void>(pthread_create(&isr, nullptr, isr_main, nullptr));

    std::printf("=== libev C++17 uart-ring-hsm demo ===\n");
    std::printf("[test] ISR -> ring(%uB) -> ev_async -> HSM\n", kRingSize);

    loop.run(0);

    static_cast<void>(pthread_join(isr, nullptr));

    const uart::Stats& stats = g_parser.stats();

    std::printf("\n--- results ---\n");
    std::printf("frames parsed       : %u\n", static_cast<unsigned>(stats.frames_received));
    std::printf("bytes received      : %u\n", static_cast<unsigned>(stats.bytes_received));
    std::printf("ring overflow bytes : %u\n", static_cast<unsigned>(g_dropped));

    const bool pass = (2U == g_frames_rx) && (0U == g_dropped);
    std::printf("RING_HSM_CHECK: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

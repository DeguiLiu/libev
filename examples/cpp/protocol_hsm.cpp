// libev C++17 example: hierarchical protocol HSM driven by an ev_timer.
//
// A connection protocol HSM (Operational -> Disconnected/Connecting/
// Connected -> Idle/Active, Disconnecting) where the Connected parent handles
// DISCONNECT for both the Idle and Active children via parent-state event
// inheritance. A repeating ev_timer steps a scripted signal sequence through
// the HSM, one signal per tick.
// SPDX-License-Identifier: MIT

#include <array>
#include <cstdint>
#include <cstdio>

#include "ev_raii.hpp"
#include "hsm.hpp"

namespace {

enum Signal : uint16_t {
    kConnect = 1U,
    kSynAck = 2U,
    kDisconnect = 3U,
    kFinAck = 4U,
    kTimeout = 5U,
    kDataReady = 6U,
    kDataSent = 7U,
    kError = 8U
};

struct ProtocolContext {
    uint32_t syn_count = 0U;
    uint32_t ack_count = 0U;
    uint32_t data_sent_count = 0U;
    uint32_t error_count = 0U;
    bool connected = false;
};

constexpr int8_t kOperational = 0;
constexpr int8_t kDisconnected = 1;
constexpr int8_t kConnecting = 2;
constexpr int8_t kConnected = 3;
constexpr int8_t kIdle = 4;
constexpr int8_t kActive = 5;
constexpr int8_t kDisconnecting = 6;

const hsm::StateDef<ProtocolContext> kStates[] = {
    { -1, nullptr, nullptr, "Operational" },
    { kOperational,
      [](ProtocolContext& ctx) {
          ctx.connected = false;
          std::printf("  [Disconnected] entry: connection closed\n");
      },
      nullptr, "Disconnected" },
    { kOperational,
      [](ProtocolContext& ctx) {
          ++ctx.syn_count;
          std::printf("  [Connecting] entry: sending SYN...\n");
      },
      nullptr, "Connecting" },
    { kOperational,
      [](ProtocolContext& ctx) {
          ctx.connected = true;
          std::printf("  [Connected] entry: connection established\n");
      },
      [](ProtocolContext&) { std::printf("  [Connected] exit: leaving connected state\n"); },
      "Connected" },
    { kConnected,
      [](ProtocolContext&) { std::printf("  [Idle] entry: waiting for data\n"); },
      nullptr, "Idle" },
    { kConnected,
      [](ProtocolContext&) { std::printf("  [Active] entry: processing data\n"); },
      nullptr, "Active" },
    { kOperational,
      [](ProtocolContext&) { std::printf("  [Disconnecting] entry: sending FIN...\n"); },
      nullptr, "Disconnecting" },
};

const hsm::TransitionDef<ProtocolContext> kTransitions[] = {
    { kDisconnected, kConnect, kConnecting, hsm::TransitionKind::External, nullptr, nullptr },
    { kConnecting, kSynAck, kIdle, hsm::TransitionKind::External, nullptr,
      [](ProtocolContext& ctx, uint16_t) { ++ctx.ack_count; } },
    { kConnecting, kTimeout, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
    /* Connected handles DISCONNECT for the Idle/Active children. */
    { kConnected, kDisconnect, kDisconnecting, hsm::TransitionKind::External, nullptr, nullptr },
    { kIdle, kDataReady, kActive, hsm::TransitionKind::External, nullptr, nullptr },
    { kActive, kDataSent, kIdle, hsm::TransitionKind::External, nullptr,
      [](ProtocolContext& ctx, uint16_t) { ++ctx.data_sent_count; } },
    { kActive, kError, kIdle, hsm::TransitionKind::External, nullptr,
      [](ProtocolContext& ctx, uint16_t) { ++ctx.error_count; } },
    { kDisconnecting, kFinAck, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
    { kDisconnecting, kTimeout, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

// Compile-time policy injected into evx::Timer: logs every timer event before
// on_tick runs, keeping the cross-cutting trace out of the business callback.
struct TimerTrace {
    static void on_event(ev_timer&, uint32_t revents) noexcept
    {
        std::printf("[trace] timer fired revents=0x%x\n", static_cast<unsigned>(revents));
    }
};

// Compile-time policy injected into hsm::Hsm: logs the structural
// enter/exit/transition trajectory, separate from the entry/exit business logs.
struct HsmTrace {
    static void on_enter(const char* name) noexcept
    {
        if (nullptr != name) { std::printf("  [hsm-trace] enter %s\n", name); }
    }
    static void on_exit(const char* name) noexcept
    {
        if (nullptr != name) { std::printf("  [hsm-trace] exit %s\n", name); }
    }
    static void on_transition(uint16_t signal, int8_t source, int8_t target) noexcept
    {
        std::printf("[hsm-trace] sig=%u %d->%d\n",
                    static_cast<unsigned>(signal),
                    static_cast<int>(source),
                    static_cast<int>(target));
    }
};

class ProtocolDemo {
public:
    ProtocolDemo() noexcept
        : hsm_(kStates, kNumStates, kTransitions, kNumTransitions, kDisconnected, /*max_depth=*/3U),
          timer_(loop_, this)
    {
    }

    int run() noexcept;

private:
    static const char* signal_name(uint16_t s) noexcept;
    void on_tick(ev_timer& w, uint32_t revents) noexcept;

    ProtocolContext ctx_;
    evx::Loop loop_;
    hsm::Hsm<ProtocolContext, HsmTrace> hsm_;
    evx::Timer<ProtocolDemo, &ProtocolDemo::on_tick, TimerTrace> timer_;
    std::array<uint16_t, 14U> script_;
    uint16_t idx_ = 0U;
};

const char* ProtocolDemo::signal_name(uint16_t s) noexcept
{
    switch (s)
    {
    case kConnect: return "CONNECT";
    case kSynAck: return "SYN_ACK";
    case kDisconnect: return "DISCONNECT";
    case kFinAck: return "FIN_ACK";
    case kTimeout: return "TIMEOUT";
    case kDataReady: return "DATA_READY";
    case kDataSent: return "DATA_SENT";
    case kError: return "ERROR";
    default: return "?";
    }
}

void ProtocolDemo::on_tick(ev_timer&, uint32_t) noexcept
{
    if (idx_ >= script_.size())
    {
        loop_.break_loop();
        return;
    }
    const uint16_t sig = script_[idx_];
    ++idx_;
    std::printf(">> %s\n", signal_name(sig));
    hsm_.dispatch(ctx_, sig);
}

int ProtocolDemo::run() noexcept
{
    script_ = {
        kConnect, kSynAck,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kError,
        kDataReady, kDataSent,
        kDisconnect, kFinAck,
    };

    hsm_.init(ctx_);

    std::printf("=== libev C++17 protocol HSM demo ===\n");
    timer_.start(0.0, 0.001);

    loop_.run(0);

    const bool pass =
        (ctx_.syn_count == 1U) &&
        (ctx_.ack_count == 1U) &&
        (ctx_.data_sent_count == 4U) &&
        (ctx_.error_count == 1U) &&
        (!ctx_.connected) &&
        (hsm_.current_state() == kDisconnected);

    std::printf("\n=== final context ===\n");
    std::printf("syn_count:       %u\n", static_cast<unsigned>(ctx_.syn_count));
    std::printf("ack_count:       %u\n", static_cast<unsigned>(ctx_.ack_count));
    std::printf("data_sent_count: %u\n", static_cast<unsigned>(ctx_.data_sent_count));
    std::printf("error_count:     %u\n", static_cast<unsigned>(ctx_.error_count));
    std::printf("connected:       %s\n", ctx_.connected ? "true" : "false");
    std::printf("hsm state:       %s\n", hsm_.current_state_name());
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    ProtocolDemo demo;
    return demo.run();
}

// libev C++17 example: hierarchical protocol HSM driven by an ev_timer.
//
// A connection protocol HSM (Operational -> Disconnected/Connecting/
// Connected -> Idle/Active, Disconnecting) where the Connected parent handles
// DISCONNECT for both the Idle and Active children via parent-state event
// inheritance. A repeating ev_timer steps a scripted signal sequence through
// the HSM, one signal per tick.
// SPDX-License-Identifier: MIT

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

void disconnected_entry(ProtocolContext& ctx)
{
    ctx.connected = false;
    std::printf("  [Disconnected] entry: connection closed\n");
}

void connecting_entry(ProtocolContext& ctx)
{
    ++ctx.syn_count;
    std::printf("  [Connecting] entry: sending SYN...\n");
}

void connected_entry(ProtocolContext& ctx)
{
    ctx.connected = true;
    std::printf("  [Connected] entry: connection established\n");
}

void connected_exit(ProtocolContext&)
{
    std::printf("  [Connected] exit: leaving connected state\n");
}

void idle_entry(ProtocolContext&)
{
    std::printf("  [Idle] entry: waiting for data\n");
}

void active_entry(ProtocolContext&)
{
    std::printf("  [Active] entry: processing data\n");
}

void disconnecting_entry(ProtocolContext&)
{
    std::printf("  [Disconnecting] entry: sending FIN...\n");
}

void ack_action(ProtocolContext& ctx, uint16_t)
{
    ++ctx.ack_count;
}

void sent_action(ProtocolContext& ctx, uint16_t)
{
    ++ctx.data_sent_count;
}

void error_action(ProtocolContext& ctx, uint16_t)
{
    ++ctx.error_count;
}

const hsm::StateDef<ProtocolContext> kStates[] = {
    { -1, nullptr, nullptr, "Operational" },
    { kOperational, disconnected_entry, nullptr, "Disconnected" },
    { kOperational, connecting_entry, nullptr, "Connecting" },
    { kOperational, connected_entry, connected_exit, "Connected" },
    { kConnected, idle_entry, nullptr, "Idle" },
    { kConnected, active_entry, nullptr, "Active" },
    { kOperational, disconnecting_entry, nullptr, "Disconnecting" },
};

const hsm::TransitionDef<ProtocolContext> kTransitions[] = {
    { kDisconnected, kConnect, kConnecting, hsm::TransitionKind::External, nullptr, nullptr },
    { kConnecting, kSynAck, kIdle, hsm::TransitionKind::External, nullptr, ack_action },
    { kConnecting, kTimeout, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
    /* Connected handles DISCONNECT for the Idle/Active children. */
    { kConnected, kDisconnect, kDisconnecting, hsm::TransitionKind::External, nullptr, nullptr },
    { kIdle, kDataReady, kActive, hsm::TransitionKind::External, nullptr, nullptr },
    { kActive, kDataSent, kIdle, hsm::TransitionKind::External, nullptr, sent_action },
    { kActive, kError, kIdle, hsm::TransitionKind::External, nullptr, error_action },
    { kDisconnecting, kFinAck, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
    { kDisconnecting, kTimeout, kDisconnected, hsm::TransitionKind::External, nullptr, nullptr },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

struct Driver {
    const uint16_t* script;
    uint16_t len;
    uint16_t idx;
    hsm::Hsm<ProtocolContext>* hsm;
    ProtocolContext* ctx;
    evx::Loop* loop;
};

Driver g_drv{nullptr, 0U, 0U, nullptr, nullptr, nullptr};

const char* signal_name(uint16_t s)
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

void on_tick(ev_timer*, int)
{
    if (g_drv.idx >= g_drv.len)
    {
        g_drv.loop->break_loop();
        return;
    }
    const uint16_t sig = g_drv.script[g_drv.idx];
    ++g_drv.idx;
    std::printf(">> %s\n", signal_name(sig));
    g_drv.hsm->dispatch(*g_drv.ctx, sig);
}

}  // namespace

int main()
{
    const uint16_t script[] = {
        kConnect, kSynAck,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kDataSent,
        kDataReady, kError,
        kDataReady, kDataSent,
        kDisconnect, kFinAck,
    };

    ProtocolContext ctx;
    hsm::Hsm<ProtocolContext> hsm(kStates, kNumStates, kTransitions, kNumTransitions,
                                  kDisconnected, /*max_depth=*/3U);
    hsm.init(ctx);

    evx::Loop loop;
    if (!loop.valid())
    {
        std::printf("ev_loop_new failed\n");
        return 1;
    }

    g_drv.script = script;
    g_drv.len = static_cast<uint16_t>(sizeof(script) / sizeof(script[0]));
    g_drv.idx = 0U;
    g_drv.hsm = &hsm;
    g_drv.ctx = &ctx;
    g_drv.loop = &loop;

    std::printf("=== libev C++17 protocol HSM demo ===\n");
    evx::Timer<on_tick> timer(loop);
    timer.start(0.0, 0.001);

    loop.run(0);

    const bool pass =
        (ctx.syn_count == 1U) &&
        (ctx.ack_count == 1U) &&
        (ctx.data_sent_count == 4U) &&
        (ctx.error_count == 1U) &&
        (!ctx.connected) &&
        (hsm.current_state() == kDisconnected);

    std::printf("\n=== final context ===\n");
    std::printf("syn_count:       %u\n", static_cast<unsigned>(ctx.syn_count));
    std::printf("ack_count:       %u\n", static_cast<unsigned>(ctx.ack_count));
    std::printf("data_sent_count: %u\n", static_cast<unsigned>(ctx.data_sent_count));
    std::printf("error_count:     %u\n", static_cast<unsigned>(ctx.error_count));
    std::printf("connected:       %s\n", ctx.connected ? "true" : "false");
    std::printf("hsm state:       %s\n", hsm.current_state_name());
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

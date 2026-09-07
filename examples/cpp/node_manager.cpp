// libev C++17 example: four heartbeat-driven node HSMs under one ev_timer.
//
// Each node runs an independent three-state HSM (Connected -> Suspect ->
// Disconnected) driven by a heartbeat signal. A single repeating ev_timer
// steps a scripted per-node heartbeat/miss sequence, one tick at a time,
// demonstrating multiple Hsm instances + multiple watcher callbacks sharing
// one event loop.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>

#include "ev_raii.hpp"
#include "hsm.hpp"

namespace {

enum Signal : uint16_t {
    kHeartbeat = 1U,
    kMiss = 2U
};

struct NodeContext {
    uint32_t total_hb = 0U;    // heartbeats received
    uint32_t total_miss = 0U;  // misses seen (cumulative)
    uint32_t missed = 0U;      // consecutive misses (current run)
    bool connected = false;
};

constexpr int8_t kRoot = 0;
constexpr int8_t kConnected = 1;
constexpr int8_t kSuspect = 2;
constexpr int8_t kDisconnected = 3;

void connected_entry(NodeContext& ctx)
{
    ctx.connected = true;
    std::printf("  [Connected] entry\n");
}

void suspect_entry(NodeContext&)
{
    std::printf("  [Suspect] entry: missed heartbeat\n");
}

void disconnected_entry(NodeContext& ctx)
{
    ctx.connected = false;
    std::printf("  [Disconnected] entry: link down\n");
}

void hb_action(NodeContext& ctx, uint16_t)
{
    ++ctx.total_hb;
    ctx.missed = 0U;
}

void miss_action(NodeContext& ctx, uint16_t)
{
    ++ctx.total_miss;
    ++ctx.missed;
}

const hsm::StateDef<NodeContext> kStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kRoot, connected_entry, nullptr, "Connected" },
    { kRoot, suspect_entry, nullptr, "Suspect" },
    { kRoot, disconnected_entry, nullptr, "Disconnected" },
};

const hsm::TransitionDef<NodeContext> kTransitions[] = {
    { kConnected, kHeartbeat, kConnected, hsm::TransitionKind::Internal, nullptr, hb_action },
    { kConnected, kMiss, kSuspect, hsm::TransitionKind::External, nullptr, miss_action },
    { kSuspect, kHeartbeat, kConnected, hsm::TransitionKind::External, nullptr, hb_action },
    { kSuspect, kMiss, kDisconnected, hsm::TransitionKind::External, nullptr, miss_action },
    { kDisconnected, kHeartbeat, kConnected, hsm::TransitionKind::External, nullptr, hb_action },
    { kDisconnected, kMiss, kDisconnected, hsm::TransitionKind::Internal, nullptr, miss_action },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

constexpr uint16_t kNumNodes = 4U;
constexpr uint16_t kNumTicks = 4U;

/* per-node heartbeat script, one signal per tick:
 * node 0: all heartbeats            -> stays Connected
 * node 1: one miss, then recover    -> Suspect -> Connected
 * node 2: two misses, then recover  -> Suspect -> Disconnected -> Connected
 * node 3: two misses up front       -> Disconnected -> Connected
 */
const uint16_t kScript[kNumNodes][kNumTicks] = {
    { kHeartbeat, kHeartbeat, kHeartbeat, kHeartbeat },
    { kHeartbeat, kMiss, kHeartbeat, kHeartbeat },
    { kHeartbeat, kMiss, kMiss, kHeartbeat },
    { kMiss, kMiss, kHeartbeat, kHeartbeat },
};

struct Node {
    NodeContext ctx;
    hsm::Hsm<NodeContext> hsm;

    Node() noexcept
        : hsm(kStates, kNumStates, kTransitions, kNumTransitions, kConnected, /*max_depth=*/2U)
    {
    }
};

struct Driver {
    Node* nodes;
    uint16_t tick;
    evx::Loop* loop;
};

Driver g_drv{nullptr, 0U, nullptr};

void on_tick(ev_timer*, int)
{
    if (g_drv.tick >= kNumTicks)
    {
        g_drv.loop->break_loop();
        return;
    }
    std::printf("-- tick %u --\n", static_cast<unsigned>(g_drv.tick));
    for (uint16_t i = 0U; i < kNumNodes; ++i)
    {
        const uint16_t sig = kScript[i][g_drv.tick];
        g_drv.nodes[i].hsm.dispatch(g_drv.nodes[i].ctx, sig);
    }
    ++g_drv.tick;
}

}  // namespace

int main()
{
    Node nodes[kNumNodes];

    for (uint16_t i = 0U; i < kNumNodes; ++i)
    {
        nodes[i].hsm.init(nodes[i].ctx);
    }

    evx::Loop loop;
    if (!loop.valid())
    {
        std::printf("ev_loop_new failed\n");
        return 1;
    }

    g_drv.nodes = nodes;
    g_drv.tick = 0U;
    g_drv.loop = &loop;

    std::printf("=== libev C++17 node manager demo ===\n");
    evx::Timer<on_tick> timer(loop);
    timer.start(0.0, 0.001);

    loop.run(0);

    const uint32_t expected_hb[kNumNodes] = { 4U, 3U, 2U, 2U };
    const uint32_t expected_miss[kNumNodes] = { 0U, 1U, 2U, 2U };

    bool pass = true;
    std::printf("\n=== final node states ===\n");
    for (uint16_t i = 0U; i < kNumNodes; ++i)
    {
        const NodeContext& ctx = nodes[i].ctx;
        const char* name = nodes[i].hsm.current_state_name();
        std::printf("Node %u: hb=%u miss=%u missed=%u [%s]\n",
                    static_cast<unsigned>(i + 1U),
                    static_cast<unsigned>(ctx.total_hb),
                    static_cast<unsigned>(ctx.total_miss),
                    static_cast<unsigned>(ctx.missed),
                    (nullptr != name) ? name : "?");
        if ((ctx.total_hb != expected_hb[i]) ||
            (ctx.total_miss != expected_miss[i]) ||
            (nodes[i].hsm.current_state() != kConnected))
        {
            pass = false;
        }
    }

    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

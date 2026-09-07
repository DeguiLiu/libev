// libev C++17 example: four heartbeat-driven node HSMs under one ev_timer.
//
// Each node runs an independent three-state HSM (Connected -> Suspect ->
// Disconnected) driven by a heartbeat signal. A single repeating ev_timer
// steps a scripted per-node heartbeat/miss sequence, one tick at a time,
// demonstrating multiple Hsm instances sharing one event loop.
// SPDX-License-Identifier: MIT

#include <array>
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

/* shared by three transitions each, kept as named functions */
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
    { kRoot,
      [](NodeContext& ctx) {
          ctx.connected = true;
          std::printf("  [Connected] entry\n");
      },
      nullptr, "Connected" },
    { kRoot,
      [](NodeContext&) { std::printf("  [Suspect] entry: missed heartbeat\n"); },
      nullptr, "Suspect" },
    { kRoot,
      [](NodeContext& ctx) {
          ctx.connected = false;
          std::printf("  [Disconnected] entry: link down\n");
      },
      nullptr, "Disconnected" },
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

class NodeManager {
public:
    NodeManager() noexcept : timer_(loop_, this) {}

    int run() noexcept;

private:
    struct Node {
        NodeContext ctx;
        hsm::Hsm<NodeContext> hsm;

        Node() noexcept
            : hsm(kStates, kNumStates, kTransitions, kNumTransitions, kConnected, /*max_depth=*/2U)
        {
        }
    };

    void on_tick(ev_timer& w, int revents) noexcept;

    evx::Loop loop_;
    std::array<Node, kNumNodes> nodes_;
    evx::Timer<NodeManager, &NodeManager::on_tick> timer_;
    uint16_t tick_ = 0U;
};

void NodeManager::on_tick(ev_timer&, int) noexcept
{
    if (tick_ >= kNumTicks)
    {
        loop_.break_loop();
        return;
    }
    std::printf("-- tick %u --\n", static_cast<unsigned>(tick_));
    for (uint16_t i = 0U; i < kNumNodes; ++i)
    {
        nodes_[i].hsm.dispatch(nodes_[i].ctx, kScript[i][tick_]);
    }
    ++tick_;
}

int NodeManager::run() noexcept
{
    for (Node& n : nodes_)
    {
        n.hsm.init(n.ctx);
    }

    std::printf("=== libev C++17 node manager demo ===\n");
    timer_.start(0.0, 0.001);

    loop_.run(0);

    const std::array<uint32_t, kNumNodes> expected_hb = { 4U, 3U, 2U, 2U };
    const std::array<uint32_t, kNumNodes> expected_miss = { 0U, 1U, 2U, 2U };

    bool pass = true;
    std::printf("\n=== final node states ===\n");
    for (uint16_t i = 0U; i < kNumNodes; ++i)
    {
        const NodeContext& ctx = nodes_[i].ctx;
        const char* name = nodes_[i].hsm.current_state_name();
        std::printf("Node %u: hb=%u miss=%u missed=%u [%s]\n",
                    static_cast<unsigned>(i + 1U),
                    static_cast<unsigned>(ctx.total_hb),
                    static_cast<unsigned>(ctx.total_miss),
                    static_cast<unsigned>(ctx.missed),
                    (nullptr != name) ? name : "?");
        if ((ctx.total_hb != expected_hb[i]) ||
            (ctx.total_miss != expected_miss[i]) ||
            (nodes_[i].hsm.current_state() != kConnected))
        {
            pass = false;
        }
    }

    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    NodeManager mgr;
    return mgr.run();
}

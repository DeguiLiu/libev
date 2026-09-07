// libev C++17 example: async write proxy with an ev_async + worker thread.
//
// The event loop thread owns the single-flight write state machine; a
// detached pthread worker performs the blocking I/O (simulated NAND erase/
// program latency) and signals completion back to the loop via ev_async.
// ev_async_send is the only thread-safe libev entry point, so the worker
// never touches loop-owned state directly.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>

#include <pthread.h>
#include <unistd.h>

#include "ev_raii.hpp"

namespace {

constexpr uint32_t kNumWrites = 3U;
constexpr useconds_t kIoLatencyUs = 10000U;  // simulated erase/program latency

struct ProxyState {
    uint32_t submitted = 0U;   // writes started
    uint32_t completed = 0U;   // writes finished
};

void on_done(ev_async*, int);

ProxyState g_state;
evx::Loop* g_loop = nullptr;
evx::Async<on_done>* g_async = nullptr;

void submit_next();

void* worker_main(void*)
{
    usleep(kIoLatencyUs);   // blocking I/O, never on the event loop thread
    g_async->send();        // wake the loop: this write is done
    return nullptr;
}

void on_done(ev_async*, int)
{
    ++g_state.completed;
    std::printf("[proxy] write %u done\n", static_cast<unsigned>(g_state.completed));
    if (g_state.completed < kNumWrites)
    {
        submit_next();
    }
    else
    {
        g_loop->break_loop();
    }
}

void submit_next()
{
    ++g_state.submitted;
    std::printf("[proxy] write %u started\n", static_cast<unsigned>(g_state.submitted));
    pthread_t tid;
    pthread_create(&tid, nullptr, worker_main, nullptr);
    pthread_detach(tid);
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

    evx::Async<on_done> async(loop);
    g_loop = &loop;
    g_async = &async;
    async.start();

    std::printf("=== libev C++17 async proxy demo ===\n");
    submit_next();
    loop.run(0);

    const bool pass =
        (g_state.submitted == kNumWrites) &&
        (g_state.completed == kNumWrites);

    std::printf("submitted: %u  completed: %u\n",
                static_cast<unsigned>(g_state.submitted),
                static_cast<unsigned>(g_state.completed));
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

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

class AsyncProxy {
public:
    AsyncProxy() noexcept : async_(loop_, this) {}

    int run() noexcept;

private:
    void on_done(ev_async& w, int revents) noexcept;
    void submit_next() noexcept;
    static void* worker_main(void* arg) noexcept;

    evx::Loop loop_;
    evx::Async<AsyncProxy, &AsyncProxy::on_done> async_;
    uint32_t submitted_ = 0U;   // writes started
    uint32_t completed_ = 0U;   // writes finished
};

void* AsyncProxy::worker_main(void* arg) noexcept
{
    AsyncProxy* self = static_cast<AsyncProxy*>(arg);

    usleep(kIoLatencyUs);       // blocking I/O, never on the event loop thread
    self->async_.send();        // wake the loop: this write is done
    return nullptr;
}

void AsyncProxy::on_done(ev_async&, int) noexcept
{
    ++completed_;
    std::printf("[proxy] write %u done\n", static_cast<unsigned>(completed_));
    if (completed_ < kNumWrites)
    {
        submit_next();
    }
    else
    {
        loop_.break_loop();
    }
}

void AsyncProxy::submit_next() noexcept
{
    ++submitted_;
    std::printf("[proxy] write %u started\n", static_cast<unsigned>(submitted_));
    pthread_t tid;
    static_cast<void>(pthread_create(&tid, nullptr, worker_main, this));
    static_cast<void>(pthread_detach(tid));
}

int AsyncProxy::run() noexcept
{
    async_.start();

    std::printf("=== libev C++17 async proxy demo ===\n");
    submit_next();
    loop_.run(0);

    const bool pass =
        (submitted_ == kNumWrites) &&
        (completed_ == kNumWrites);

    std::printf("submitted: %u  completed: %u\n",
                static_cast<unsigned>(submitted_),
                static_cast<unsigned>(completed_));
    std::printf("RESULT: %s\n", pass ? "PASS" : "FAIL");

    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    AsyncProxy proxy;
    return proxy.run();
}

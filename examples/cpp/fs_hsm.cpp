// libev C++17 example: async file write with ev_async + worker thread + HSM.
//
// Mirrors examples/c/fs-hsm.c in C++. The C file's embedded HSM engine is
// replaced by hsm.hpp. State topology: TOP -> IDLE / BUSY -> WRITING /
// SYNCING / DONE / FAILED. Blocking file I/O runs on a pthread worker and
// signals completion via ev_async; a heartbeat timer proves the loop stays
// responsive while the worker is busy.
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <cstdio>
#include <cstring>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include "ev_raii.hpp"
#include "hsm.hpp"

namespace {

enum Signal : uint16_t {
    kWriteReq = 1U,
    kWriteDone = 2U,
    kSyncDone = 3U,
    kWorkerFail = 4U
};

constexpr uint32_t kChunkSize = 64U * 1024U;
constexpr uint16_t kChunks = 64U;         // 4 MiB total
constexpr double kHeartbeatSec = 0.010;   // 10 ms

constexpr int8_t kTop = 0;
constexpr int8_t kIdle = 1;
constexpr int8_t kBusy = 2;
constexpr int8_t kWriting = 3;
constexpr int8_t kSyncing = 4;
constexpr int8_t kDone = 5;
constexpr int8_t kFailed = 6;

struct FileWriter {
    int fd = -1;
    int result = 0;
    uint32_t heartbeats = 0U;
    bool worker_busy = false;
    int worker_phase = 0;   // 0 = write data, 1 = fsync only
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    bool quit = false;
};

void on_done(ev_async*, int);

FileWriter g_fw;
evx::Loop* g_loop = nullptr;
evx::Async<on_done>* g_async = nullptr;

hsm::Hsm<FileWriter>* g_hsm = nullptr;

/* ---- HSM actions (loop thread only) ---- */

void act_start_write(FileWriter& ctx, uint16_t)
{
    ctx.heartbeats = 0U;
    pthread_mutex_lock(&ctx.lock);
    ctx.worker_phase = 0;
    ctx.worker_busy = true;
    pthread_cond_signal(&ctx.cond);
    pthread_mutex_unlock(&ctx.lock);
    std::printf("[hsm] write started (4 MiB in background)\n");
}

void act_report_done(FileWriter& ctx, uint16_t)
{
    std::printf("[hsm] file written+synced, %u heartbeats observed during write\n",
                static_cast<unsigned>(ctx.heartbeats));
}

void act_report_fail(FileWriter& ctx, uint16_t)
{
    std::printf("[hsm] write failed (errno=%d)\n", ctx.result);
}

/* ---- HSM tables ---- */

const hsm::StateDef<FileWriter> kStates[] = {
    { -1, nullptr, nullptr, "TOP" },
    { kTop, nullptr, nullptr, "IDLE" },
    { kTop, nullptr, nullptr, "BUSY" },
    { kBusy, nullptr, nullptr, "WRITING" },
    { kBusy, nullptr, nullptr, "SYNCING" },
    { kIdle, nullptr, nullptr, "DONE" },
    { kIdle, nullptr, nullptr, "FAILED" },
};

const hsm::TransitionDef<FileWriter> kTransitions[] = {
    { kIdle, kWriteReq, kWriting, hsm::TransitionKind::External, nullptr, act_start_write },
    { kBusy, kWorkerFail, kFailed, hsm::TransitionKind::External, nullptr, act_report_fail },
    { kWriting, kWriteDone, kSyncing, hsm::TransitionKind::External, nullptr, nullptr },
    { kSyncing, kSyncDone, kDone, hsm::TransitionKind::External, nullptr, act_report_done },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

/* ---- worker thread: blocking file I/O lives here, never in the loop ---- */

void* worker_main(void*)
{
    for (;;)
    {
        bool do_write = false;
        bool do_sync = false;

        pthread_mutex_lock(&g_fw.lock);
        while (!g_fw.worker_busy && !g_fw.quit)
        {
            pthread_cond_wait(&g_fw.cond, &g_fw.lock);
        }
        if (g_fw.quit)
        {
            pthread_mutex_unlock(&g_fw.lock);
            break;
        }
        do_write = (0 == g_fw.worker_phase);
        do_sync = true;
        pthread_mutex_unlock(&g_fw.lock);

        if (do_write)
        {
            static uint8_t chunk[kChunkSize];
            bool ok = true;
            std::memset(chunk, 0xA5, sizeof(chunk));

            for (uint16_t i = 0U; i < kChunks && ok; ++i)
            {
                const ssize_t n = write(g_fw.fd, chunk, sizeof(chunk));
                if (n != static_cast<ssize_t>(sizeof(chunk)))
                {
                    ok = false;
                    break;
                }
                usleep(2000);
            }

            g_fw.result = ok ? 0 : -1;
            do_sync = ok;
        }

        if (do_sync)
        {
            g_fw.result = (0 == fsync(g_fw.fd)) ? 0 : -1;
        }

        g_async->send();

        pthread_mutex_lock(&g_fw.lock);
        g_fw.worker_busy = false;
        pthread_mutex_unlock(&g_fw.lock);
    }

    return nullptr;
}

/* ---- ev_async callback: worker -> loop, translate to HSM events ---- */

void on_done(ev_async*, int)
{
    if (0 != g_fw.result)
    {
        g_hsm->dispatch(g_fw, kWorkerFail);
        g_loop->break_loop();
        return;
    }

    if (g_hsm->current_state() == kWriting)
    {
        g_hsm->dispatch(g_fw, kWriteDone);

        pthread_mutex_lock(&g_fw.lock);
        g_fw.worker_phase = 1;
        g_fw.worker_busy = true;
        pthread_cond_signal(&g_fw.cond);
        pthread_mutex_unlock(&g_fw.lock);
    }
    else
    {
        g_hsm->dispatch(g_fw, kSyncDone);
        g_loop->break_loop();
    }
}

/* ---- heartbeat + test sequencer ---- */

void heartbeat_cb(ev_timer*, int)
{
    if (g_hsm->current_state() == kWriting || g_hsm->current_state() == kSyncing)
    {
        ++g_fw.heartbeats;
    }
}

void step_cb(ev_timer* w, int)
{
    ev_timer_stop(g_loop->raw(), w);
    g_hsm->dispatch(g_fw, kWriteReq);
}

}  // namespace

int main()
{
    const char path[] = "/tmp/fs-hsm-test.bin";
    bool pass = false;
    struct stat st;

    g_fw.fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (g_fw.fd < 0)
    {
        std::printf("open failed\n");
        return 1;
    }

    evx::Loop loop;
    if (!loop.valid())
    {
        std::printf("ev_loop_new failed\n");
        close(g_fw.fd);
        return 1;
    }
    g_loop = &loop;

    hsm::Hsm<FileWriter> hsm(kStates, kNumStates, kTransitions, kNumTransitions,
                             kIdle, /*max_depth=*/3U);
    g_hsm = &hsm;
    hsm.init(g_fw);

    pthread_t worker;
    static_cast<void>(pthread_create(&worker, nullptr, worker_main, nullptr));

    evx::Async<on_done> async(loop);
    g_async = &async;
    async.start();

    evx::Timer<heartbeat_cb> heartbeat(loop);
    heartbeat.start(kHeartbeatSec, kHeartbeatSec);

    evx::Timer<step_cb> step(loop);
    step.start(0.02, 0.0);

    std::printf("[test] writing 4 MiB asynchronously, heartbeat every 10 ms\n");
    loop.run(0);

    off_t size = 0;
    if (0 == fstat(g_fw.fd, &st))
    {
        size = st.st_size;
    }

    const char* state_name = hsm.current_state_name();
    const bool is_done = (nullptr != state_name) && (0 == std::strcmp(state_name, "DONE"));
    pass = is_done &&
           (size == static_cast<off_t>(kChunks * kChunkSize)) &&
           (g_fw.heartbeats >= 10U);

    std::printf("\n--- results ---\n");
    std::printf("final state              : %s\n", (nullptr != state_name) ? state_name : "?");
    std::printf("file size                : %ld bytes\n", static_cast<long>(size));
    std::printf("heartbeats during write  : %u\n", static_cast<unsigned>(g_fw.heartbeats));
    std::printf("ASYNC_CHECK: %s\n", pass ? "PASS" : "FAIL");

    pthread_mutex_lock(&g_fw.lock);
    g_fw.quit = true;
    pthread_cond_signal(&g_fw.cond);
    pthread_mutex_unlock(&g_fw.lock);
    static_cast<void>(pthread_join(worker, nullptr));

    close(g_fw.fd);
    unlink(path);

    return pass ? 0 : 1;
}

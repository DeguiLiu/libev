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
    { kIdle, kWriteReq, kWriting, hsm::TransitionKind::External, nullptr,
      [](FileWriter& ctx, uint16_t) {
          ctx.heartbeats = 0U;
          pthread_mutex_lock(&ctx.lock);
          ctx.worker_phase = 0;
          ctx.worker_busy = true;
          pthread_cond_signal(&ctx.cond);
          pthread_mutex_unlock(&ctx.lock);
          std::printf("[hsm] write started (4 MiB in background)\n");
      } },
    { kBusy, kWorkerFail, kFailed, hsm::TransitionKind::External, nullptr,
      [](FileWriter& ctx, uint16_t) { std::printf("[hsm] write failed (errno=%d)\n", ctx.result); } },
    { kWriting, kWriteDone, kSyncing, hsm::TransitionKind::External, nullptr, nullptr },
    { kSyncing, kSyncDone, kDone, hsm::TransitionKind::External, nullptr,
      [](FileWriter& ctx, uint16_t) {
          std::printf("[hsm] file written+synced, %u heartbeats observed during write\n",
                      static_cast<unsigned>(ctx.heartbeats));
      } },
};

constexpr uint16_t kNumStates = static_cast<uint16_t>(sizeof(kStates) / sizeof(kStates[0]));
constexpr uint16_t kNumTransitions = static_cast<uint16_t>(sizeof(kTransitions) / sizeof(kTransitions[0]));

class FsHsmDemo {
public:
    FsHsmDemo() noexcept
        : hsm_(kStates, kNumStates, kTransitions, kNumTransitions, kIdle, /*max_depth=*/3U),
          async_(loop_, this),
          heartbeat_(loop_, this),
          step_(loop_, this)
    {
    }

    int run() noexcept;

private:
    void on_done(ev_async& w, int revents) noexcept;
    void heartbeat_cb(ev_timer& w, int revents) noexcept;
    void step_cb(ev_timer& w, int revents) noexcept;
    static void* worker_main(void* arg) noexcept;

    FileWriter fw_;
    evx::Loop loop_;
    hsm::Hsm<FileWriter> hsm_;
    evx::Async<FsHsmDemo, &FsHsmDemo::on_done> async_;
    evx::Timer<FsHsmDemo, &FsHsmDemo::heartbeat_cb> heartbeat_;
    evx::Timer<FsHsmDemo, &FsHsmDemo::step_cb> step_;
};

void* FsHsmDemo::worker_main(void* arg) noexcept
{
    FsHsmDemo* self = static_cast<FsHsmDemo*>(arg);
    FileWriter& fw = self->fw_;

    for (;;)
    {
        bool do_write = false;
        bool do_sync = false;

        pthread_mutex_lock(&fw.lock);
        while (!fw.worker_busy && !fw.quit)
        {
            pthread_cond_wait(&fw.cond, &fw.lock);
        }
        if (fw.quit)
        {
            pthread_mutex_unlock(&fw.lock);
            break;
        }
        do_write = (0 == fw.worker_phase);
        do_sync = true;
        pthread_mutex_unlock(&fw.lock);

        if (do_write)
        {
            static uint8_t chunk[kChunkSize];
            bool ok = true;
            std::memset(chunk, 0xA5, sizeof(chunk));

            for (uint16_t i = 0U; i < kChunks && ok; ++i)
            {
                const ssize_t n = write(fw.fd, chunk, sizeof(chunk));
                if (n != static_cast<ssize_t>(sizeof(chunk)))
                {
                    ok = false;
                    break;
                }
                usleep(2000);
            }

            fw.result = ok ? 0 : -1;
            do_sync = ok;
        }

        if (do_sync)
        {
            fw.result = (0 == fsync(fw.fd)) ? 0 : -1;
        }

        self->async_.send();

        pthread_mutex_lock(&fw.lock);
        fw.worker_busy = false;
        pthread_mutex_unlock(&fw.lock);
    }

    return nullptr;
}

void FsHsmDemo::on_done(ev_async&, int) noexcept
{
    if (0 != fw_.result)
    {
        hsm_.dispatch(fw_, kWorkerFail);
        loop_.break_loop();
        return;
    }

    if (hsm_.current_state() == kWriting)
    {
        hsm_.dispatch(fw_, kWriteDone);

        pthread_mutex_lock(&fw_.lock);
        fw_.worker_phase = 1;
        fw_.worker_busy = true;
        pthread_cond_signal(&fw_.cond);
        pthread_mutex_unlock(&fw_.lock);
    }
    else
    {
        hsm_.dispatch(fw_, kSyncDone);
        loop_.break_loop();
    }
}

void FsHsmDemo::heartbeat_cb(ev_timer&, int) noexcept
{
    if (hsm_.current_state() == kWriting || hsm_.current_state() == kSyncing)
    {
        ++fw_.heartbeats;
    }
}

void FsHsmDemo::step_cb(ev_timer& w, int) noexcept
{
    ev_timer_stop(loop_.raw(), &w);
    hsm_.dispatch(fw_, kWriteReq);
}

int FsHsmDemo::run() noexcept
{
    const char path[] = "/tmp/fs-hsm-test.bin";
    bool pass = false;
    struct stat st;

    fw_.fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fw_.fd < 0)
    {
        std::printf("open failed\n");
        return 1;
    }

    hsm_.init(fw_);

    pthread_t worker;
    static_cast<void>(pthread_create(&worker, nullptr, worker_main, this));

    async_.start();
    heartbeat_.start(kHeartbeatSec, kHeartbeatSec);
    step_.start(0.02, 0.0);

    std::printf("[test] writing 4 MiB asynchronously, heartbeat every 10 ms\n");
    loop_.run(0);

    off_t size = 0;
    if (0 == fstat(fw_.fd, &st))
    {
        size = st.st_size;
    }

    const char* state_name = hsm_.current_state_name();
    const bool is_done = (nullptr != state_name) && (0 == std::strcmp(state_name, "DONE"));
    pass = is_done &&
           (size == static_cast<off_t>(kChunks * kChunkSize)) &&
           (fw_.heartbeats >= 10U);

    std::printf("\n--- results ---\n");
    std::printf("final state              : %s\n", (nullptr != state_name) ? state_name : "?");
    std::printf("file size                : %ld bytes\n", static_cast<long>(size));
    std::printf("heartbeats during write  : %u\n", static_cast<unsigned>(fw_.heartbeats));
    std::printf("ASYNC_CHECK: %s\n", pass ? "PASS" : "FAIL");

    pthread_mutex_lock(&fw_.lock);
    fw_.quit = true;
    pthread_cond_signal(&fw_.cond);
    pthread_mutex_unlock(&fw_.lock);
    static_cast<void>(pthread_join(worker, nullptr));

    close(fw_.fd);
    unlink(path);

    return pass ? 0 : 1;
}

}  // namespace

int main()
{
    FsHsmDemo demo;
    return demo.run();
}

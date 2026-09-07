/*
 * Async file write driven by libev + HSM (ev_async + worker thread).
 *
 * libev cannot event-drive regular files: a regular-file fd is always
 * "ready" for select/epoll, so reads/writes would block the loop. The
 * standard pattern (what libuv does internally with its threadpool) is:
 * do the blocking file I/O on a worker thread and signal completion back
 * to the event loop with ev_async -- the only thread-safe libev entry
 * point. This mirrors the datamanage wholedata write path shape:
 *
 *   IDLE -> WRITING (worker: write chunks) -> SYNCING (worker: fsync)
 *        -> DONE / FAILED -> IDLE
 *
 * Asynchrony is observable and asserted by the self-test: while the
 * worker is busy writing a multi-megabyte file, the event loop keeps
 * firing a heartbeat timer (~every 10ms). If the write had blocked the
 * loop, the heartbeat would stall and the test fails.
 *
 * Build:
 *   gcc examples/c/fs-hsm.c examples/c/hsm.c -I include -lev -o fs-hsm -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>

#include <ev.h>

#include "hsm.h"

/* ================================================================== */
/* async file writer: HSM on top of ev_async + worker thread           */
/* ================================================================== */

enum {
    EVT_WRITE_REQ,      /* request a file write (loop thread)          */
    EVT_WRITE_DONE,     /* worker finished writing data (via ev_async) */
    EVT_SYNC_DONE,      /* worker finished fsync (via ev_async)        */
    EVT_WORKER_FAIL     /* worker hit an I/O error                     */
};

#define FW_CHUNK_SIZE   (64 * 1024)
#define FW_CHUNKS       64                     /* 4 MiB total         */
#define FW_HEARTBEAT_MS 10.0

typedef struct {
    /* worker-thread side (owned by worker while busy) */
    int32_t fd;
    int32_t result;

    /* loop-thread side */
    hsm_t hsm;
    ev_async done_async;         /* worker -> loop wakeup */
    pthread_t worker;
    int32_t worker_busy;         /* 0 idle, 1 busy */
    int32_t worker_phase;        /* 0 = write data, 1 = fsync only */
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int32_t quit;                /* worker shutdown flag */

    /* observability */
    uint32_t heartbeats_during_write;
    const char *path;
} file_writer_t;

static file_writer_t g_fw;
static struct ev_loop *g_loop;
static ev_timer g_heartbeat;
static ev_timer g_test_step;

/* state forwards */
static hsm_state_t s_top, s_idle, s_writing, s_syncing, s_done, s_failed;

static const char *state_name (void) { return g_fw.hsm.current->name; }

/* ---- worker thread: blocking file I/O lives here, never in the loop ---- */

static void *
worker_main (void *arg)
{
    file_writer_t *fw = arg;

    for (;;)
    {
        int32_t do_write = 0;
        int32_t do_sync = 0;

        pthread_mutex_lock (&fw->lock);

        while (0 == fw->worker_busy && 0 == fw->quit)
            pthread_cond_wait (&fw->cond, &fw->lock);

        if (fw->quit)
        {
            pthread_mutex_unlock (&fw->lock);
            break;
        }

        /* phase is set by the loop before signalling the worker */
        do_write = (0 == fw->worker_phase);
        do_sync = 1;
        pthread_mutex_unlock (&fw->lock);

        if (do_write)
        {
            static uint8_t chunk[FW_CHUNK_SIZE];
            uint32_t i;
            int32_t ok = 1;

            memset (chunk, 0xA5, sizeof (chunk));

            for (i = 0; i < FW_CHUNKS && ok; i++)
            {
                ssize_t n = write (fw->fd, chunk, sizeof (chunk));

                if (n != (ssize_t)sizeof (chunk))
                {
                    ok = 0;
                    break;
                }

                /* simulate slow NAND-ish media: real async work for the
                 * loop to overlap with */
                usleep (2000);
            }

            fw->result = ok ? 0 : -1;
            do_sync = ok;
        }

        if (do_sync)
        {
            /* flush the page cache for this fd -- real durable-media cost */
            fw->result = (0 == fsync (fw->fd)) ? 0 : -1;
        }

        /* signal the loop; ev_async_send is thread-safe */
        ev_async_send (g_loop, &fw->done_async);

        pthread_mutex_lock (&fw->lock);
        fw->worker_busy = 0;
        pthread_mutex_unlock (&fw->lock);
    }

    return NULL;
}

/* ---- HSM actions (loop thread only) ---- */

static void
act_start_write (hsm_t *sm, const hsm_event_t *e)
{
    file_writer_t *fw = sm->user_data;

    (void) e;
    fw->heartbeats_during_write = 0;
    pthread_mutex_lock (&fw->lock);
    fw->worker_phase = 0;         /* phase 0: write data */
    fw->worker_busy = 1;
    pthread_cond_signal (&fw->cond);
    pthread_mutex_unlock (&fw->lock);
    printf ("[hsm] %s: write started (4 MiB in background)\n", state_name ());
}

static void
act_report_done (hsm_t *sm, const hsm_event_t *e)
{
    file_writer_t *fw = sm->user_data;

    (void) e;
    printf ("[hsm] %s: file written+synced, %u heartbeats observed during write\n",
            state_name (), fw->heartbeats_during_write);
}

static void
act_report_fail (hsm_t *sm, const hsm_event_t *e)
{
    file_writer_t *fw = sm->user_data;

    (void) e;
    printf ("[hsm] %s: write failed (errno=%d)\n", state_name (), fw->result);
}

/* transitions: WRITING and SYNCING share the completion/fail policy
 * via their parent (BUSY); IDLE accepts new requests */
static const hsm_transition_t t_idle[] = {
    { EVT_WRITE_REQ, &s_writing, NULL, act_start_write, HSM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t t_busy[] = {
    /* parent of WRITING/SYNCING/DONE/FAILED: failure always lands FAILED */
    { EVT_WORKER_FAIL, &s_failed, NULL, act_report_fail, HSM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t t_writing[] = {
    { EVT_WRITE_DONE, &s_syncing, NULL, NULL, HSM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t t_syncing[] = {
    { EVT_SYNC_DONE, &s_done, NULL, act_report_done, HSM_TRANSITION_EXTERNAL },
};

static hsm_state_t s_top = { NULL, NULL, NULL, NULL, 0, "TOP" };
static hsm_state_t s_idle = { &s_top, NULL, NULL, t_idle, 1, "IDLE" };
static hsm_state_t s_busy = { &s_top, NULL, NULL, t_busy, 1, "BUSY" };
static hsm_state_t s_writing = { &s_busy, NULL, NULL, t_writing, 1, "WRITING" };
static hsm_state_t s_syncing = { &s_busy, NULL, NULL, t_syncing, 1, "SYNCING" };
static hsm_state_t s_done = { &s_idle, NULL, NULL, NULL, 0, "DONE" };
static hsm_state_t s_failed = { &s_idle, NULL, NULL, NULL, 0, "FAILED" };

/* ---- ev_async callback: worker -> loop, translate to HSM events ---- */

static void
done_async_cb (EV_P_ ev_async *a, int revents)
{
    hsm_event_t e;

    (void) a; (void) revents;

    if (0 != g_fw.result)
    {
        e.id = EVT_WORKER_FAIL;
        hsm_dispatch (&g_fw.hsm, &e);
        ev_break (EV_A_ EVBREAK_ALL);
        return;
    }

    if (hsm_is_in (&g_fw.hsm, &s_writing))
    {
        e.id = EVT_WRITE_DONE;   /* data written, ask for sync next */
        hsm_dispatch (&g_fw.hsm, &e);

        /* kick the worker again, phase 1: fsync only */
        pthread_mutex_lock (&g_fw.lock);
        g_fw.worker_phase = 1;
        g_fw.worker_busy = 1;
        pthread_cond_signal (&g_fw.cond);
        pthread_mutex_unlock (&g_fw.lock);
    }
    else
    {
        e.id = EVT_SYNC_DONE;
        hsm_dispatch (&g_fw.hsm, &e);
        ev_break (EV_A_ EVBREAK_ALL);
    }
}

/* heartbeat: proof the loop stays responsive during the background write */
static void
heartbeat_cb (EV_P_ ev_timer *t, int revents)
{
    (void) t; (void) revents;

    if (hsm_is_in (&g_fw.hsm, &s_writing) || hsm_is_in (&g_fw.hsm, &s_syncing))
        g_fw.heartbeats_during_write++;
}

/* test sequencer: fire the write request shortly after start */
static void
step_cb (EV_P_ ev_timer *t, int revents)
{
    hsm_event_t e = { EVT_WRITE_REQ, NULL };

    (void) revents;
    ev_timer_stop (EV_A_ t);
    hsm_dispatch (&g_fw.hsm, &e);
}

int
main (void)
{
    const char *path = "/tmp/fs-hsm-test.bin";
    int32_t pass;
    struct stat st;

    g_loop = EV_DEFAULT;

    g_fw.fd = open (path, O_CREAT | O_TRUNC | O_WRONLY, 0644);

    if (g_fw.fd < 0)
    {
        perror ("open");
        return 1;
    }

    g_fw.path = path;
    pthread_mutex_init (&g_fw.lock, NULL);
    pthread_cond_init (&g_fw.cond, NULL);

    hsm_init (&g_fw.hsm, &s_idle, &g_fw);

    pthread_create (&g_fw.worker, NULL, worker_main, &g_fw);

    ev_async_init (&g_fw.done_async, done_async_cb);
    ev_async_start (g_loop, &g_fw.done_async);

    /* repeating heartbeat: fires every FW_HEARTBEAT_MS while loop runs */
    ev_timer_init (&g_heartbeat, heartbeat_cb, FW_HEARTBEAT_MS / 1000.0, FW_HEARTBEAT_MS / 1000.0);
    ev_timer_start (g_loop, &g_heartbeat);

    /* kick the test after 20ms */
    ev_timer_init (&g_test_step, step_cb, 0.02, 0.);
    ev_timer_start (g_loop, &g_test_step);

    printf ("[test] writing 4 MiB asynchronously, heartbeat every %.0f ms\n",
            FW_HEARTBEAT_MS);
    ev_run (g_loop, 0);

    /* ---- self-check ---- */
    {
        off_t size = 0;

        if (0 == fstat (g_fw.fd, &st))
            size = st.st_size;

        /* async proof: heartbeat kept firing during the write.
         * The worker sleeps 2ms per 64 KiB chunk (64 chunks = ~128ms),
         * so at 10ms per heartbeat we expect well over 10 ticks. */
        printf ("\n--- results ---\n");
        printf ("final state              : %s\n", state_name ());
        printf ("file size                : %ld bytes\n", (long)size);
        printf ("heartbeats during write  : %u\n", g_fw.heartbeats_during_write);

        pass = (0 == strcmp (state_name (), "DONE")) &&
               (size == (off_t)(FW_CHUNKS * FW_CHUNK_SIZE)) &&
               (g_fw.heartbeats_during_write >= 10U);

        printf ("ASYNC_CHECK: %s\n", pass ? "PASS" : "FAIL");
    }

    /* shutdown worker */
    pthread_mutex_lock (&g_fw.lock);
    g_fw.quit = 1;
    pthread_cond_signal (&g_fw.cond);
    pthread_mutex_unlock (&g_fw.lock);
    pthread_join (g_fw.worker, NULL);

    close (g_fw.fd);
    unlink (path);

    pthread_mutex_destroy (&g_fw.lock);
    pthread_cond_destroy (&g_fw.cond);

    return pass ? 0 : 1;
}

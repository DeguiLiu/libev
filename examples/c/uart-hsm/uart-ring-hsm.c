/*
 * UART parsing with an upstream ring buffer: "ISR" -> ring -> libev -> HSM.
 *
 * This is the MCU-realistic shape the plain uart-hsm.c omits: a real
 * UART interrupt handler must not run the state machine; it only drains
 * the FIFO into a ring buffer as fast as possible. The event loop then
 * wakes (here via ev_async, matching the "enqueue then poke the loop"
 * idiom ev_idle does on one-shot edges) and drains the ring into the HSM
 * parser.
 *
 *   [simulated ISR thread] --spsc_queue push--> [ ev_async poke ]
 *        -- ev_async callback --> spsc_queue pop --> hsm_parser_put_data
 *
 * The ISR thread is the producer, the ev_async callback is the consumer:
 * exactly spsc_queue's single-producer single-consumer contract. All
 * bytes land in the same hsm_parser used by uart-hsm.c, zero parser
 * changes.
 *
 * Build:
 *   gcc examples/c/uart-hsm/uart-ring-hsm.c examples/c/uart-hsm/hsm_parser.c \
 *       examples/c/uart-hsm/state_machine.c -I examples/c/uart-hsm \
 *       -I include -lev -o uart-ring-hsm -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>

#include <ev.h>

#include "types.h"
#include "uart_protocol.h"
#include "hsm_parser.h"
#include "spsc_queue.h"

#define RING_SIZE     4096U    /* power of two */
#define ISR_POKE_COUNT 4U      /* one frame in several interrupts */

typedef struct {
    struct ev_loop *loop;
    hsm_parser_t parser;
    spsc_queue_t *ring;
    ev_async wake;
    ev_timer drain;
    volatile bool running;
    uint32_t frames_rx;
    uint32_t dropped;
} app_t;

_Static_assert ((RING_SIZE & (RING_SIZE - 1U)) == 0U, "RING_SIZE must be power of two");

typedef struct {
    const char *name;
    uint8_t cls, cmd;
} isr_test_t;

/* producer: simulate the UART ISR running on another thread/vector */
static void *
isr_thread (void *arg)
{
    app_t *app = (app_t *)arg;
    static const isr_test_t tests[] = {
        { "sys", VDCMD_CLASS_SYS, VDCMD_SYS_GET_INFO },
        { "spi", VDCMD_CLASS_SPI, VDCMD_SPI_READ     },
    };
    uint32_t i;

    usleep (50000); /* let the loop arm first */

    for (i = 0; i < 2; i++)
    {
        uint8_t frame[64];
        uint32_t len = uart_build_simple_frame (frame, tests[i].cls, tests[i].cmd,
                                                NULL, 0);
        uint32_t off = 0;

        /* deliver the frame in ISR_POKE_COUNT chunks, one "interrupt" each */
        while (off < len)
        {
            uint32_t n = (len - off > ISR_POKE_COUNT) ? ISR_POKE_COUNT : (len - off);

            if (SPSC_QUEUE_OK != spsc_queue_push (app->ring, &frame[off], n))
                app->dropped += n;

            off += n;
            ev_async_send (app->loop, &app->wake); /* wake the loop */
            usleep (1000);                          /* next interrupt arrives later */
        }
    }

    app->running = false;
    ev_async_send (app->loop, &app->wake);
    return NULL;
}

/* consumer: loop side -- drain the ring straight into the HSM */
static void
drain_ring (app_t *app)
{
    uint8_t buf[64];
    uint32_t got;

    while (0 != (got = spsc_queue_pop (app->ring, buf, sizeof (buf))))
        hsm_parser_put_data (&app->parser, buf, got);
}

static void
wake_cb (EV_P_ ev_async *w, int revents)
{
    app_t *app = (app_t *)w->data;

    (void) revents;

    drain_ring (app);

    if (false == app->running && 0U == spsc_queue_data_len (app->ring))
        ev_break (EV_A_ EVBREAK_ALL);
}

/* periodic drain: belt-and-suspenders for dropped wakeups (real UARTs
 * also have a character-timeout polling fallback) */
static void
drain_cb (EV_P_ ev_timer *w, int revents)
{
    app_t *app = (app_t *)w->data;

    (void) revents;

    drain_ring (app);

    if (false == app->running && 0U == spsc_queue_data_len (app->ring))
        ev_break (EV_A_ EVBREAK_ALL);
}

static void
frame_cb (const uart_frame_t *frame, void *user_data)
{
    app_t *app = (app_t *)user_data;

    printf ("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
            frame->cmd_class, frame->cmd, frame->data_len);
    app->frames_rx++;
}

int
main (void)
{
    app_t app = { 0 };
    pthread_t isr;
    const hsm_parser_stats_t *stats;
    int32_t pass;

    app.loop = EV_DEFAULT;
    app.running = true;

    app.ring = spsc_queue_create (RING_SIZE);

    if (NULL == app.ring)
    {
        fprintf (stderr, "ring create failed\n");
        return 1;
    }

    if (FALSE == hsm_parser_init (&app.parser, frame_cb, &app))
    {
        fprintf (stderr, "hsm_parser_init failed\n");
        return 1;
    }

    ev_async_init (&app.wake, wake_cb);
    app.wake.data = &app;
    ev_async_start (app.loop, &app.wake);

    /* trailing-drain safety net every 5ms */
    ev_timer_init (&app.drain, drain_cb, 0.005, 0.005);
    app.drain.data = &app;
    ev_timer_start (app.loop, &app.drain);

    pthread_create (&isr, NULL, isr_thread, &app);

    printf ("[test] ISR -> ring(%uB) -> ev_async -> HSM\n", RING_SIZE);
    ev_run (app.loop, 0);

    pthread_join (isr, NULL);

    stats = hsm_parser_get_stats (&app.parser);

    printf ("\n--- results ---\n");
    printf ("frames parsed       : %u\n", (unsigned)stats->frames_received);
    printf ("bytes received      : %u\n", (unsigned)stats->bytes_received);
    printf ("ring overflow bytes : %u\n", (unsigned)app.dropped);

    pass = (2U == app.frames_rx) && (0U == app.dropped);

    printf ("RING_HSM_CHECK: %s\n", pass ? "PASS" : "FAIL");

    spsc_queue_destroy (app.ring);
    return pass ? 0 : 1;
}

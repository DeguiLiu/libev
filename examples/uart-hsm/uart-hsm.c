/*
 * UART protocol parsing driven by libev + HSM.
 *
 * This example ports the standalone demo at
 * gitee.com/liudegui/uart_statemachine_ringbuffer_linux to libev:
 * its "pthread simulating UART interrupts -> spsc queue -> main-loop
 * polling" driver is replaced by an event loop -- ev_io watches the
 * UART fd (a pipe here, /dev/ttyS* or an RT-Thread device fd on real
 * hardware) and every readable event feeds bytes straight into the
 * hierarchical state machine parser (hsm_parser, same engine lineage
 * as the datamanage wholedata HSM).
 *
 * Byte flow:
 *
 *   test writer --(pipe fd)--> ev_io callback --> hsm_parser_put_data
 *                                                        |
 *                                        frame complete: v
 *                                                frame callback
 *
 * The parser itself is untouched; only the driver layer changed.
 * On RT-Thread the same structure applies with the UART device fd
 * obtained through the RT-Thread device/DFS framework.
 *
 * Build:
 *   gcc examples/uart-hsm/uart-hsm.c examples/uart-hsm/hsm_parser.c \
 *       examples/uart-hsm/state_machine.c -I examples/uart-hsm \
 *       -I include -lev -o uart-hsm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include <ev.h>

#include "types.h"
#include "uart_protocol.h"
#include "hsm_parser.h"

/* pipe end indices */
#define RD 0
#define WR 1

static hsm_parser_t g_parser;
static ev_io g_uart_w;
static ev_timer g_test_w;
static ev_timer g_stop_w;
static int g_uart_pipe[2];

/* counters for the self-check at the end */
static uint32_t g_frames_rx;
static uint32_t g_frames_sent;

/* ------------------------------------------------------------------ */
/* HSM frame callback: a complete frame made it through the machine    */

static void
frame_cb (const uart_frame_t *frame, void *user_data)
{
    (void) user_data;

    printf ("[HSM] frame: class=0x%02X cmd=0x%02X data_len=%u\n",
            frame->cmd_class, frame->cmd, frame->data_len);
    g_frames_rx++;
}

/* ------------------------------------------------------------------ */
/* libev callback: UART fd readable -> drain and feed the HSM          */

static void
uart_rx_cb (EV_P_ ev_io *w, int revents)
{
    uint8_t buf[128];

    (void) revents;

    for (;;)
    {
        ssize_t n = read (w->fd, buf, sizeof (buf));

        if (n > 0)
        {
            hsm_parser_put_data (&g_parser, buf, (uint32_t)n);
        }
        else if (n < 0 && EAGAIN == errno)
        {
            break; /* drained */
        }
        else if (n < 0 && EINTR == errno)
        {
            continue;
        }
        else
        {
            /* 0 (EOF) or hard error: stop watching */
            ev_io_stop (EV_A_ w);
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* test driver: write protocol frames into the pipe                   */

typedef struct {
    const char *name;
    uint8_t cmd_class;
    uint8_t cmd;
    uint8_t data[8];
    uint16_t data_len;
} test_case_t;

static const test_case_t g_tests[] = {
    { "sys get info",     VDCMD_CLASS_SYS, VDCMD_SYS_GET_INFO, { 0x01, 0x00 }, 2 },
    { "spi read id",      VDCMD_CLASS_SPI, VDCMD_SPI_READ_ID,  { 0x00, 0x00 }, 2 },
    { "spi read",         VDCMD_CLASS_SPI, VDCMD_SPI_READ,     { 0, 0, 0, 0, 0, 0, 16, 0 }, 8 },
    { "ota start",        VDCMD_CLASS_OTA, VDCMD_OTA_START,    { 0, 0, 0, 16, 0, 0, 0, 0 }, 8 },
};

static void
send_frame (const test_case_t *t)
{
    uint8_t buf[64];
    uint32_t len = uart_build_simple_frame (buf, t->cmd_class, t->cmd,
                                            t->data, t->data_len);

    printf ("[TEST] %-14s -> %u bytes\n", t->name, len);
    write (g_uart_pipe[WR], buf, len);
    g_frames_sent++;
}

/* garbage: bad header, wrong CRC, wrong tail -- the HSM must survive */
static void
send_garbage (void)
{
    static const uint8_t bad1[] = { 0xBB, 0xCC };
    uint8_t bad2[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0xFF, 0xFF, 0x55 };
    uint8_t bad3[8] = { 0xAA, 0x02, 0x00, 0x01, 0x01, 0, 0, 0x66 };
    uint16_t crc = uart_calc_crc16 (&bad3[3], 2);

    bad3[5] = (uint8_t)(crc & 0xFFU);
    bad3[6] = (uint8_t)((crc >> 8) & 0xFFU);

    printf ("[TEST] garbage frames (bad hdr / bad crc / bad tail)\n");
    write (g_uart_pipe[WR], bad1, sizeof (bad1));
    write (g_uart_pipe[WR], bad2, sizeof (bad2));
    write (g_uart_pipe[WR], bad3, sizeof (bad3));
}

static void test_timer_cb (EV_P_ ev_timer *w, int revents);
static void stop_timer_cb (EV_P_ ev_timer *w, int revents);

/* one test round every 100ms, then garbage, then stop */
static void
test_timer_cb (EV_P_ ev_timer *w, int revents)
{
    static uint32_t round_no;

    (void) revents;

    if (round_no < 4)
    {
        send_frame (&g_tests[round_no]);
    }
    else if (4 == round_no)
    {
        send_garbage ();
    }

    round_no++;

    if (round_no > 5)
    {
        ev_timer_stop (EV_A_ w);
        /* let the last round drain, then stop the loop */
        ev_timer_stop (EV_A_ &g_stop_w);
        ev_timer_init (&g_stop_w, stop_timer_cb, 0.3, 0.);
        ev_timer_start (EV_A_ &g_stop_w);
    }
}

static void
stop_timer_cb (EV_P_ ev_timer *w, int revents)
{
    (void) w; (void) revents;
    ev_break (EV_A_ EVBREAK_ALL);
}

int
main (void)
{
    struct ev_loop *loop = EV_DEFAULT;
    const hsm_parser_stats_t *stats;
    int32_t pass;

    /* the pipe plays the role of the UART fd; O_NONBLOCK on our read end */
    if (pipe (g_uart_pipe) < 0)
    {
        perror ("pipe");
        return 1;
    }

    {
        int flags = fcntl (g_uart_pipe[RD], F_GETFL, 0);
        fcntl (g_uart_pipe[RD], F_SETFL, flags | O_NONBLOCK);
    }

    if (FALSE == hsm_parser_init (&g_parser, frame_cb, NULL))
    {
        fprintf (stderr, "hsm_parser_init failed\n");
        return 1;
    }

    ev_io_init (&g_uart_w, uart_rx_cb, g_uart_pipe[RD], EV_READ);
    ev_io_start (loop, &g_uart_w);

    /* first round after 50ms, then every 100ms */
    ev_timer_init (&g_test_w, test_timer_cb, 0.05, 0.1);
    ev_timer_start (loop, &g_test_w);

    ev_run (loop, 0);

    /* self-check: 4 good frames in, 4 frames out, garbage rejected */
    stats = hsm_parser_get_stats (&g_parser);

    printf ("\n--- results ---\n");
    printf ("frames sent (good)      : %u\n", (unsigned)g_frames_sent);
    printf ("frames parsed           : %u\n", (unsigned)stats->frames_received);
    printf ("bytes received          : %u\n", (unsigned)stats->bytes_received);
    printf ("sync/crc/tail errors    : %u / %u / %u\n",
            stats->sync_errors, stats->crc_errors, stats->tail_errors);

    pass = (g_frames_rx == g_frames_sent) &&
           (stats->crc_errors >= 1U) &&
           (stats->tail_errors >= 1U) &&
           (stats->sync_errors >= 1U);

    printf ("SELF_CHECK: %s\n", pass ? "PASS" : "FAIL");

    close (g_uart_pipe[RD]);
    close (g_uart_pipe[WR]);

    return pass ? 0 : 1;
}

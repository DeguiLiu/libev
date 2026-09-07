/*
 * Hierarchical State Machine + libev connection lifecycle demo.
 *
 * The HSM engine here mirrors the datamanage wholedata implementation
 * (dm_whd_hsm_engine.c, lineage: Andreas Misje's state_machine): state
 * objects with transition tables, guard/action functions, external vs
 * internal transitions, LCA-based entry/exit execution. The RT-Thread
 * types are swapped for standard C so the example builds anywhere libev
 * does.
 *
 * Connection lifecycle (7 states, hierarchical):
 *
 *   TOP
 *   └── ESTABLISHED                    connection exists
 *       ├── CONNECTED                  peer still sending
 *       │   ├── RECEIVING              waiting for data (EV_READ armed)
 *       │   └── FLUSHING               send queue non-empty (EV_READ|WRITE)
 *       └── CLOSING                    peer EOF seen: drain queue, then die
 *
 * DRAINING and FAILED would be flat siblings in a naive design; here
 * they fall out of ESTABLISHED's exit actions instead.
 *
 * Events (posted from libev callbacks):
 *   EVT_ACCEPTED, EVT_DATA, EVT_WRITABLE, EVT_EOF, EVT_ERROR
 *
 * Guard example: CONNECTED only handles EVT_EOF when the send queue is
 * empty (guard_queue_empty); otherwise the event bubbles up to
 * ESTABLISHED which defers the close -- the same pattern DM uses for
 * "degrade only when not exclusive".
 *
 * Build: gcc examples/c/hsm-echo.c examples/c/hsm.c -I include -lev -o hsm-echo
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <ev.h>

#include "hsm.h"

#define LISTEN_PORT 7701
#define BUF_SIZE    1024
#define QUEUE_CAP   8192

/* ================================================================== */
/* connection: HSM instance + libev watcher + echo bookkeeping        */
/* ================================================================== */

enum {
    EVT_ACCEPTED,    /* accept() returned this fd          */
    EVT_DATA,        /* readable, payload arrived          */
    EVT_WRITABLE,    /* send buffer has room               */
    EVT_EOF,         /* recv() == 0                        */
    EVT_ERROR        /* hard error                         */
};

struct conn {
    hsm_t hsm;
    int fd;
    ev_io io;
    char queue[QUEUE_CAP];
    size_t q_len;
    char buf[BUF_SIZE];
    uint64_t rx, tx;
    struct conn *next, **pp_self;
};

static struct ev_loop *loop;
static struct conn *conn_head;

/* state forwards */
static hsm_state_t s_top, s_established, s_connected, s_receiving,
                   s_flushing, s_closing, s_failed;

static void
arm (struct conn *c, int events)
{
    ev_io_stop (loop, &c->io);
    ev_io_set (&c->io, c->fd, events);
    ev_io_start (loop, &c->io);
}

static void
teardown (struct conn *c)
{
    ev_io_stop (loop, &c->io);
    close (c->fd);

    if (c->pp_self)
        *c->pp_self = c->next;

    if (c->next)
        c->next->pp_self = c->pp_self;

    printf ("[hsm] connection done: rx=%lu tx=%lu\n", c->rx, c->tx);
    free (c);
}

/* send as much as the kernel takes; returns remaining queue length */
static size_t
try_flush (struct conn *c)
{
    size_t off = 0;

    while (off < c->q_len)
    {
        ssize_t m = send (c->fd, c->queue + off, c->q_len - off, 0);

        if (m < 0)
        {
            if (EAGAIN == errno)
                break;

            return (size_t)-1;
        }

        off += (size_t)m;
        c->tx += (size_t)m;
    }

    if (off == c->q_len)
        c->q_len = 0;
    else if (off > 0)
    {
        memmove (c->queue, c->queue + off, c->q_len - off);
        c->q_len -= off;
    }

    return c->q_len;
}

/* ------------------------------------------------------------------ */
/* guards                                                              */

/* close only once our own queue is drained */
static int guard_queue_empty (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;
    return 0 == c->q_len;
}

/* ------------------------------------------------------------------ */
/* actions                                                             */

static void act_arm_read (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;
    arm (c, EV_READ);
}

static void act_arm_rw (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;
    arm (c, EV_READ | EV_WRITE);
}

/* push received data into the queue, then try to flush */
static void act_echo (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;
    size_t len = (size_t)(long)e->context;

    /* backpressure: if the queue cannot take this read, drop the whole
     * read instead of partially truncating (partial truncation silently
     * corrupts the byte stream). In practice this only fires when the
     * peer outruns us by more than QUEUE_CAP between flush attempts. */
    if (len > QUEUE_CAP - c->q_len)
    {
        printf ("[hsm] queue full, dropping %zu bytes (backpressure)\n", len);
        return;
    }

    memcpy (c->queue + c->q_len, c->buf, len);
    c->q_len += len;
    c->rx += len;
}

static void act_try_drain (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;

    if ((size_t)-1 == try_flush (c))
    {
        hsm_event_t err = { EVT_ERROR, NULL };
        hsm_dispatch (sm, &err);
    }
}

/* ESTABLISHED exit: stop both watcher interest and the socket */
static void act_exit_established (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;
    ev_io_stop (loop, &c->io);
    close (c->fd);
    printf ("[hsm] socket closed on exit\n");
}

/* FAILED entry: leave the fd open just long enough to log, then drop */
static void act_enter_failed (hsm_t *sm, const hsm_event_t *e)
{
    struct conn *c = sm->user_data;

    (void) e;
    printf ("[hsm] FAILED state entered (errno=%d)\n", errno);
    ev_io_stop (loop, &c->io);
    close (c->fd);
}

/* ------------------------------------------------------------------ */
/* transition tables                                                   */

static const hsm_transition_t t_receiving[] = {
    /* EVT_DATA: queue it and (re)attempt a flush; if the kernel buffer
     * still holds bytes afterwards the FLUSHING transition follows */
    { EVT_DATA, NULL, NULL, act_echo, HSM_TRANSITION_INTERNAL },
};

static const hsm_transition_t t_flushing[] = {
    { EVT_WRITABLE, NULL, NULL, act_try_drain, HSM_TRANSITION_INTERNAL },
};

/* CONNECTED: EOF only when nothing pending; otherwise bubble up to
 * ESTABLISHED, which parks the connection in CLOSING instead */
static const hsm_transition_t t_connected[] = {
    { EVT_EOF, &s_closing, guard_queue_empty, NULL, HSM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t t_closing[] = {
    { EVT_WRITABLE, NULL, NULL, act_try_drain, HSM_TRANSITION_INTERNAL },
    { EVT_EOF,     NULL, NULL, act_try_drain, HSM_TRANSITION_INTERNAL },
};

/* ESTABLISHED: the error policy for everything below it */
static const hsm_transition_t t_established[] = {
    { EVT_ERROR, &s_failed, NULL, NULL, HSM_TRANSITION_EXTERNAL },
};

/* ------------------------------------------------------------------ */
/* state hierarchy                                                     */

static hsm_state_t s_top = {
    NULL, NULL, NULL, NULL, 0, "TOP"
};

static hsm_state_t s_established = {
    &s_top, NULL, act_exit_established, t_established, 1, "ESTABLISHED"
};

static hsm_state_t s_connected = {
    &s_established, act_arm_read, NULL, t_connected, 1, "CONNECTED"
};

static hsm_state_t s_receiving = {
    &s_connected, NULL, NULL, t_receiving, 1, "RECEIVING"
};

static hsm_state_t s_flushing = {
    &s_connected, act_arm_rw, NULL, t_flushing, 1, "FLUSHING"
};

static hsm_state_t s_closing = {
    &s_established, act_arm_rw, NULL, t_closing, 2, "CLOSING"
};

static hsm_state_t s_failed = {
    &s_established, act_enter_failed, NULL, NULL, 0, "FAILED"
};

/* ================================================================== */
/* libev callbacks: fd events -> HSM events                           */
/* ================================================================== */

static void
conn_cb (EV_P_ ev_io *w, int revents)
{
    struct conn *c = (struct conn *)(((char *)w) - offsetof (struct conn, io));

    if (revents & EV_ERROR)
    {
        hsm_event_t e = { EVT_ERROR, NULL };
        hsm_dispatch (&c->hsm, &e);
        return;
    }

    if (revents & EV_READ)
    {
        ssize_t n = recv (w->fd, c->buf, BUF_SIZE, 0);

        if (n > 0)
        {
            hsm_event_t e = { EVT_DATA, (void *)(long)n };
            hsm_dispatch (&c->hsm, &e);

            /* echo out what we just queued (I/O side effect lives in the
             * callback boundary, not inside an HSM action) */
            if ((size_t)-1 == try_flush (c))
            {
                hsm_event_t err = { EVT_ERROR, NULL };
                hsm_dispatch (&c->hsm, &err);
                teardown (c);
                return;
            }

            /* choose RECEIVING or FLUSHING based on queue after the flush */
            if (!hsm_is_in (&c->hsm, &s_closing))
            {
                if (c->q_len > 0)
                    hsm_perform_transition (&c->hsm, &s_flushing, NULL);
                else
                    hsm_perform_transition (&c->hsm, &s_receiving, NULL);
            }
        }
        else if (0 == n)
        {
            hsm_event_t e = { EVT_EOF, NULL };
            hsm_dispatch (&c->hsm, &e);

            /* closing + queue drained? finish */
            if (hsm_is_in (&c->hsm, &s_closing) && 0 == c->q_len)
            {
                teardown (c);
                return;   /* c is freed; do not touch it again this callback */
            }
        }
        else if (EAGAIN != errno && EINTR != errno)
        {
            hsm_event_t e = { EVT_ERROR, NULL };
            hsm_dispatch (&c->hsm, &e);
            teardown (c);
            return;
        }
    }

    if (revents & EV_WRITE)
    {
        hsm_event_t e = { EVT_WRITABLE, NULL };
        hsm_dispatch (&c->hsm, &e);

        if (hsm_is_in (&c->hsm, &s_closing) && 0 == c->q_len)
        {
            teardown (c);
            return;
        }

        if (hsm_is_in (&c->hsm, &s_connected) && 0 == c->q_len)
            hsm_perform_transition (&c->hsm, &s_receiving, NULL);
    }
}

static ev_io listen_w;
static ev_signal sig_w;

static void
set_nonblock (int fd)
{
    int flags = fcntl (fd, F_GETFL, 0);
    fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

static void
accept_cb (EV_P_ ev_io *w, int revents)
{
    (void) revents;

    for (;;)
    {
        int cfd = accept (w->fd, NULL, NULL);

        if (cfd < 0)
        {
            if (EAGAIN == errno || EWOULDBLOCK == errno)
                break;

            if (EINTR == errno)
                continue;

            break;
        }

        set_nonblock (cfd);

        {
            uint32_t one = 1;
            setsockopt (cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof (one));
        }

        {
            struct conn *c = calloc (1, sizeof (struct conn));

            if (0 == c)
            {
                close (cfd);
                continue;
            }

            c->fd = cfd;
            ev_init (&c->io, conn_cb);

            c->next = conn_head;
            c->pp_self = &conn_head;

            if (conn_head)
                conn_head->pp_self = &c->next;

            conn_head = c;

            /* enter the machine: full entry path TOP->ESTABLISHED->CONNECTED->RECEIVING */
            hsm_init (&c->hsm, &s_receiving, c);

            printf ("[hsm] accepted fd=%d state=%s\n", cfd, c->hsm.current->name);
        }
    }
}

static void
sig_cb (EV_P_ ev_signal *w, int revents)
{
    (void) w; (void) revents;

    while (conn_head)
        teardown (conn_head);

    ev_break (EV_A_ EVBREAK_ALL);
}

int
main (void)
{
    struct ev_loop *lp = EV_DEFAULT;
    int lfd = socket (AF_INET, SOCK_STREAM, 0);
    uint32_t one = 1;

    loop = lp;

    if (lfd < 0)
    {
        perror ("socket");
        return 1;
    }

    setsockopt (lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));

    {
        struct sockaddr_in addr;
        memset (&addr, 0, sizeof (addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl (INADDR_ANY);
        addr.sin_port = htons (LISTEN_PORT);

        if (bind (lfd, (struct sockaddr *)&addr, sizeof (addr)) < 0)
        {
            perror ("bind");
            return 1;
        }
    }

    if (listen (lfd, 16) < 0)
    {
        perror ("listen");
        return 1;
    }

    set_nonblock (lfd);

    ev_io_init (&listen_w, accept_cb, lfd, EV_READ);
    ev_io_start (lp, &listen_w);

    ev_signal_init (&sig_w, sig_cb, SIGINT);
    ev_signal_start (lp, &sig_w);

    printf ("hsm echo server on port %d (backend 0x%x)\n", LISTEN_PORT, ev_backend (lp));
    ev_run (lp, 0);

    ev_signal_stop (lp, &sig_w);
    ev_io_stop (lp, &listen_w);
    close (lfd);

    return 0;
}

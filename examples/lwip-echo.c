/*
 * libev + LwIP TCP echo server example.
 *
 * Structured after libuv's echo-server (accept callback -> per-connection
 * state -> read/write callbacks), but libev is a plain event loop: it has no
 * socket abstraction, so sockets use the native BSD API. On RT-Thread the
 * LwIP stack is reached through the SAL layer, which exposes the same BSD
 * socket names (accept/bind/listen/...), so this file builds unmodified on:
 *
 *   - Linux (epoll backend)
 *   - RT-Thread + LwIP via SAL (select backend)
 *
 * One client connection = one ev_io watcher on its accepted fd. Non-blocking
 * accept is used because the listening fd is registered for EV_READ only.
 *
 * Build (Linux):  gcc examples/lwip-echo.c -I include -lev -o echo
 * Build (RT-Thread): compile with the libev sources, see configs/rt-thread.h
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

#define LISTEN_PORT 7700
#define BUF_SIZE    1024

/* per-connection state, mirrors libuv's connection handle role */
typedef struct conn {
  struct conn *next;
  ev_io io;             /* watches EV_READ on the accepted fd */
  char buf[BUF_SIZE];
} conn_t;

static conn_t *conn_head;
static ev_io listen_w;
static ev_signal sig_w;

static void
set_nonblock (int fd)
{
  int flags = fcntl (fd, F_GETFL, 0);
  fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

static void
conn_free (EV_P_ conn_t *c)
{
  conn_t **pp = &conn_head;

  ev_io_stop (EV_A_ &c->io);
  close (c->io.fd);

  while (*pp && *pp != c)
    pp = &(*pp)->next;

  if (*pp)
    *pp = c->next;

  free (c);
}

/* read whatever arrived, write it straight back (echo) */
static void
conn_cb (EV_P_ ev_io *w, int revents)
{
  conn_t *c = (conn_t *)(((char *)w) - offsetof (conn_t, io));

  if (revents & EV_READ)
    {
      ssize_t n = recv (w->fd, c->buf, BUF_SIZE, 0);

      if (n <= 0)
        {
          /* 0 = peer closed; < 0 with EAGAIN = spurious, anything else = error */
          if (0 == n || EAGAIN != errno)
            {
              conn_free (EV_A_ c);
              return;
            }
        }
      else
        {
          /* echo back; small messages fit the socket buffer, a real server
           * would add an EV_WRITE watcher and a send queue on partial send */
          ssize_t off = 0;

          while (off < n)
            {
              ssize_t m = send (w->fd, c->buf + off, (size_t)(n - off), 0);

              if (m < 0)
                {
                  if (EAGAIN == errno)
                    continue; /* busy; retry (ok for an example, see note above) */

                  conn_free (EV_A_ c);
                  return;
                }

              off += m;
            }
        }
    }
}

/* accept loop: drain pending connections while accept keeps succeeding */
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
            break; /* drained */

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
        conn_t *c = (conn_t *)calloc (1, sizeof (conn_t));

        if (0 == c)
          {
            close (cfd);
            continue;
          }

        c->next = conn_head;
        conn_head = c;

        ev_io_init (&c->io, conn_cb, cfd, EV_READ);
        ev_io_start (EV_A_ &c->io);
      }
    }
}

static void
sig_cb (EV_P_ ev_signal *w, int revents)
{
  (void) w; (void) revents;

  while (conn_head)
    conn_free (EV_A_ conn_head);

  ev_break (EV_A_ EVBREAK_ALL);
}

int
main (void)
{
  struct ev_loop *loop = EV_DEFAULT;
  int lfd = socket (AF_INET, SOCK_STREAM, 0);
  uint32_t one = 1;

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
  ev_io_start (loop, &listen_w);

  /* Ctrl-C exits cleanly (on RT-Thread replace with your own stop event) */
  ev_signal_init (&sig_w, sig_cb, SIGINT);
  ev_signal_start (loop, &sig_w);

  printf ("echo server on port %d, backend 0x%x\n", LISTEN_PORT, ev_backend (loop));
  ev_run (loop, 0);

  ev_signal_stop (loop, &sig_w);
  ev_io_stop (loop, &listen_w);
  close (lfd);

  return 0;
}

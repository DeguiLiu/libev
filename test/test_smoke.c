/*
 * libev smoke test: version assertions, timer loop, and libevent
 * compatibility layer link coverage.
 */
#include <stdio.h>
#include <stdlib.h>

#include <ev.h>
#include <event.h>

static int timer_count;

static void
timer_cb (EV_P_ ev_timer *w, int revents)
{
  (void) loop; (void) w; (void) revents;

  if (++timer_count >= 3)
    ev_break (EV_A_ EVBREAK_ONE);
}

int
main (void)
{
  if (ev_version_major () != 4 || ev_version_minor () != 33)
    return 1;

  if (0 == ev_supported_backends ())
    return 2;

  struct ev_loop *loop = EV_DEFAULT;
  ev_timer timer;
  ev_timer_init (&timer, timer_cb, 0.001, 0.001);
  ev_timer_start (loop, &timer);
  ev_run (loop, 0);

  if (timer_count < 3)
    return 3;

  struct event_base *base = event_base_new ();
  if (0 == base)
    return 4;
  event_base_free (base);

  return 0;
}

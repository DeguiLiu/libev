/*
 * libev configuration for RT-Thread MCU targets.
 *
 * Select is the only backend (RT-Thread POSIX layer provides select).
 * Linux/BSD/Solaris backends and features are compiled out entirely.
 *
 * Usage: compile src/ev.c with -DEV_CONFIG_H='"rt-thread.h"' and this
 * file on the include path. Verified by local build + smoke test
 * (backends=0x1, select-only, timer loop OK).
 */
#ifndef EV_CONFIG_RT_THREAD_H_
#define EV_CONFIG_RT_THREAD_H_

/* backend: select only */
#define EV_USE_SELECT      1
#define EV_USE_POLL        0
#define EV_USE_EPOLL       0
#define EV_USE_KQUEUE      0
#define EV_USE_PORT        0
#define EV_USE_LINUXAIO    0
#define EV_USE_IOURING     0

/* linux-specific kernel features off */
#define EV_USE_INOTIFY     0
#define EV_USE_EVENTFD     0
#define EV_USE_SIGNALFD    0
#define EV_USE_TIMERFD     0

/* platform capability macros required by ev.c */
#define HAVE_SYS_SELECT_H  1
#define HAVE_SELECT        1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_NANOSLEEP     1
#define HAVE_FLOOR         1

#endif // EV_CONFIG_RT_THREAD_H_

# libev

libev is a high-performance event loop/event model with lots of features.
(see benchmark at http://libev.schmorp.de/bench.html)

## About

Homepage: http://software.schmorp.de/pkg/libev

Mailinglist: libev@lists.schmorp.de
http://lists.schmorp.de/cgi-bin/mailman/listinfo/libev

Library Documentation: http://pod.tst.eu/http://cvs.schmorp.de/libev/ev.pod

Libev is modelled (very losely) after libevent and the Event perl
module, but is faster, scales better and is more correct, and also more
featureful. And also smaller. Yay.

Some of the specialties of libev not commonly found elsewhere are:

- extensive and detailed, readable documentation (not doxygen garbage).
- fully supports fork, can detect fork in various ways and automatically
  re-arms kernel mechanisms that do not support fork.
- highly optimised select, poll, linux epoll, linux aio, bsd kqueue
  and solaris event ports backends.
- filesystem object (path) watching (with optional linux inotify support).
- wallclock-based times (using absolute time, cron-like).
- relative timers/timeouts (handle time jumps).
- fast intra-thread communication between multiple
  event loops (with optional fast linux eventfd backend).
- extremely easy to embed (fully documented, no dependencies,
  configuration supported but optional).
- very small codebase, no bloated library, simple code.
- fully extensible by being able to plug into the event loop,
  integrate other event loops, integrate other event loop users.
- very little memory use (small watchers, small event loop data).
- optional C++ interface allowing method and function callbacks
  at no extra memory or runtime overhead.
- optional Perl interface with similar characteristics (capable of
  running Glib/Gtk2 on libev).
- support for other languages (multiple C++ interfaces, D, Ruby,
  Python) available from third-parties.

Examples of programs that embed libev: the EV perl module, node.js,
auditd, rxvt-unicode, gvpe (GNU Virtual Private Ethernet), the
Deliantra MMORPG server (http://www.deliantra.net/), Rubinius (a
next-generation Ruby VM), the Ebb web server, the Rev event toolkit.

## Building with CMake

```
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Options:

- `BUILD_SHARED_LIBS` (default `ON`): build `libev.so`
- `BUILD_STATIC_LIBS` (default `ON`): build `libev.a`
- `BUILD_TESTING` (default `ON`): build and register the smoke test

Install:

```
cmake --install build --prefix /usr/local
```

This installs `ev.h`, `ev++.h`, `event.h` into `include/`, the
libraries into `lib/`, `libev.pc` into `lib/pkgconfig/`, and the man
page into `share/man/man3/`.

## Repository layout

```
include/   public headers: ev.h, ev++.h, event.h
src/       core sources: ev.c, event.c, ev_vars.h, ev_wrap.h
src/unix/  unix backends: epoll, kqueue, poll, port, select, linuxaio, iouring
src/win/   windows backend: ev_win32.c
test/      smoke tests
docs/      documentation: ev.3 (man page), ev.pod (source)
Symbols.ev, Symbols.event   exported symbol lists (ABI reference)
```

`ev.c` compiles as a single translation unit: it `#include`s the
backend sources from `src/unix/` and `src/win/` directly, which is why
those files are not compiled separately.

## Contributors

libev was written and designed by Marc Lehmann and Emanuele Giaquinta.

The following people sent in patches or made other noteworthy
contributions to the design (for minor patches, see the Changes
file. If I forgot to include you, please shout at me, it was an
accident):

W.C.A. Wijngaards
Christopher Layne
Chris Brody

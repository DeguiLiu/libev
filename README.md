# libev 4.33 — RT-Thread port

[English](README.md) · [简体中文](README_zh.md)

A fork of [libev](http://software.schmorp.de/pkg/libev) 4.33 that adds an
RT-Thread MCU port and verifies the event loop on embedded targets, while
keeping the original Linux/BSD/Windows support intact.

libev is a high-performance, full-featured event loop implementing the
Reactor pattern. It unifies OS I/O multiplexing mechanisms behind one API
and covers I/O, timer, signal, child-process, and filesystem events.

This fork adds on top of upstream 4.33:

- `configs/rt-thread.h` — a select-only backend config for RT-Thread: the
  RT-Thread POSIX layer provides `select`, every other backend and the
  Linux-specific kernel features are compiled out.
- Four runnable examples, each verified on an STM32F407 in Renode
  full-system simulation.
- Architecture design documents (HLD / LLD) under `docs/`.

## Backends

| Platform | Backend |
|---|---|
| Linux | epoll (default), select, poll, linuxaio, iouring |
| BSD / macOS | kqueue |
| Solaris | port |
| Windows | select |
| RT-Thread MCU | select (via `configs/rt-thread.h`) |

## Repository layout

```
include/    public headers: ev.h, ev++.h, event.h
src/        core: ev.c, event.c, ev_vars.h, ev_wrap.h
src/unix/   unix backends: epoll, kqueue, poll, port, select, linuxaio, iouring
src/win/    windows backend: ev_win32.c
configs/    rt-thread.h (RT-Thread select-only config)
examples/c/    C examples (Linux + RT-Thread)
examples/cpp/  C++17 examples (Linux host)
test/       smoke tests
docs/       ev.3 / ev.pod (API reference), HLD / LLD design docs
```

`ev.c` compiles as a single translation unit: it `#include`s the backend
sources from `src/unix/` and `src/win/` directly.

## Examples

| Example | Watchers | What it demonstrates | RT-Thread |
|---|---|---|---|
| `uart-hsm` | ev_io + ev_timer | UART protocol parsing driven by an HSM parser | verified |
| `uart-ring-hsm` | ev_async + spsc ring | ISR -> ring -> libev -> HSM (MCU-realistic) | covered |
| `fs-hsm` | ev_async + worker | async file write; heartbeat proves the loop stays live | verified |
| `lwip-echo` | ev_io | TCP echo server over LwIP via the SAL socket layer | verified |
| `hsm-echo` | ev_io | HSM connection lifecycle + socket | covered |

"verified" = self-check passed on RT-Thread STM32F407 in Renode;
"covered" = exercised through another example (same HSM engine or socket
path).

C++17 ports of the C examples live under `examples/cpp/` (CMake-built on
Linux host): `lwip_echo`, `fs_hsm`, `hsm_echo`, `uart_hsm`, `uart_ring_hsm`
mirror the five above, reusing the shared `hsm.hpp` (C++17 template HSM) and
`ev_raii.hpp` (thin RAII watchers). `protocol_hsm`, `node_manager`,
`async_proxy` are additional examples modeled after the coact framework.
Shared headers: `hsm_parser.hpp`, `uart_protocol.hpp`, `spsc_ring.hpp`.

## Building

### Linux / macOS / BSD

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake options: `BUILD_SHARED_LIBS`, `BUILD_STATIC_LIBS`, `BUILD_TESTING`,
`EV_SANITIZER` (`address`/`thread`/`undefined`), `EV_WERROR`.

### RT-Thread MCU

Compile `src/ev.c` with `-DEV_CONFIG_H='"rt-thread.h"'` and `configs/` on
the include path. See `configs/rt-thread.h` for the full feature set.
Verified on RT-Thread 5.2.2 / STM32F407 in Renode 1.16.1.

## Verification

### Linux host (CMake + ctest)

9/9 tests pass: `smoke` + 8 C++17 examples, each ending in a self-check.

| Example | Check |
|---|---|
| `protocol_hsm` | final state `Disconnected`, counts match |
| `node_manager` | 4 nodes `Connected`, heartbeat/miss counts match |
| `async_proxy` | submitted == completed == 3 |
| `lwip_echo` | loopback echo "hello lwip" |
| `fs_hsm` | 4 MiB async write, 14 heartbeats during write |
| `hsm_echo` | connection rx == tx |
| `uart_hsm` | 4 frames parsed, 3 error classes rejected |
| `uart_ring_hsm` | 2 frames via ring, 0 overflow |

### RT-Thread STM32F407 (Renode)

The C examples were verified on RT-Thread 5.2.2 / STM32F407 in Renode
1.16.1 (select backend): `uart-hsm` PASS, `fs-hsm` PASS, `lwip-echo` PASS
(loopback echo), `hsm-echo` covered by the HSM + socket paths of the others.

## Pitfalls

1. `ev_default_loop` is a global singleton; independent loops must use
   `ev_loop_new` (sharing the default loop across threads trips a
   "recursion during release" assertion).
2. RT-Thread `errno` is a negative kernel error code (`-EAGAIN == -11`), so
   non-blocking drain checks must compare `-EAGAIN == errno`.
3. Anonymous `pipe()` needs `RT_USING_POSIX_PIPE` + `RT_USING_RESOURCE_ID`.
4. lwIP static memory (~28 KB) does not fit the F407 128 KB SRAM alongside
   product code; verify standalone.
5. RAMFS has no fsync (flush is NULL); fs-hsm drops the SYNCING phase.

## Documentation

- [docs/libev-HLD-Design.md](docs/libev-HLD-Design.md) — architecture overview
- [docs/libev-LLD-Design.md](docs/libev-LLD-Design.md) — detailed design
- [docs/ev.3](docs/ev.3) — API reference (man page)

## License

BSD 2-clause, also available under GPLv2+. See [LICENSE](LICENSE).

## Upstream

Based on [libev 4.33](http://software.schmorp.de/pkg/libev) by Marc Lehmann
and Emanuele Giaquinta.

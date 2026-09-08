// Minimal C++17 RAII wrappers over the libev C API. No exceptions: Loop
// reports failure via valid(). Watchers bind a member-function callback via a
// compile-time pointer-to-member template (static polymorphism, zero vtable,
// zero heap) and hold a Loop& + the bound object. Mirrors ev++.h's
// method_thunk but keeps the watcher non-copyable and the callback typed as
// a reference (void (K::*)(ev_io&, uint32_t)).
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include <ev.h>

namespace evx {

class Loop {
public:
    explicit Loop(uint32_t flags = 0U) noexcept : raw_(ev_loop_new(static_cast<unsigned int>(flags))) {}

    ~Loop() noexcept
    {
        if (nullptr != raw_) { ev_loop_destroy(raw_); }
    }

    Loop(Loop&& other) noexcept : raw_(other.raw_) { other.raw_ = nullptr; }

    Loop& operator=(Loop&& other) noexcept
    {
        if (this != &other)
        {
            if (nullptr != raw_) { ev_loop_destroy(raw_); }
            raw_ = other.raw_;
            other.raw_ = nullptr;
        }
        return *this;
    }

    Loop(const Loop&) = delete;
    Loop& operator=(const Loop&) = delete;

    bool valid() const noexcept { return nullptr != raw_; }
    struct ev_loop* raw() const noexcept { return raw_; }
    int32_t run(uint32_t flags = 0U) noexcept
    {
        return static_cast<int32_t>(ev_run(raw_, static_cast<int>(flags)));
    }
    void break_loop() noexcept { ev_break(raw_, EVBREAK_ONE); }
    ev_tstamp now() const noexcept { return ev_now(raw_); }
    uint32_t backend() const noexcept { return static_cast<uint32_t>(ev_backend(raw_)); }

private:
    struct ev_loop* raw_;
};

// Compile-time policy (AOP-style cross-cutting hook) invoked before a watcher
// dispatches to its bound member. Default is a no-op; pass a custom Trace to
// inject logging/timing without touching the callback, at zero cost.
struct NullTrace {
    static void on_event(ev_io&, uint32_t) noexcept {}
    static void on_event(ev_timer&, uint32_t) noexcept {}
    static void on_event(ev_async&, uint32_t) noexcept {}
};

template <typename K, void (K::*Method)(ev_io&, uint32_t), typename Trace = NullTrace>
class Io {
public:
    Io(Loop& loop, K* obj) noexcept : loop_(loop), obj_(obj)
    {
        ev_init(&w_, thunk);
        w_.data = this;
    }

    ~Io() noexcept { stop(); }

    Io(const Io&) = delete;
    Io& operator=(const Io&) = delete;

    void set(int32_t fd, uint32_t events) noexcept
    {
        ev_io_set(&w_, static_cast<int>(fd), static_cast<int>(events));
    }

    void start(int32_t fd, uint32_t events) noexcept
    {
        set(fd, events);
        start();
    }

    void start() noexcept { ev_io_start(loop_.raw(), &w_); }
    void stop() noexcept { ev_io_stop(loop_.raw(), &w_); }
    bool is_active() const noexcept { return ev_is_active(&w_); }
    ev_io* watcher() noexcept { return &w_; }

private:
    static void thunk(struct ev_loop* loop, ev_io* w, int revents) noexcept
    {
        (void)loop;
        Io* self = static_cast<Io*>(w->data);
        const uint32_t events = static_cast<uint32_t>(revents);
        Trace::on_event(*w, events);
        (self->obj_->*Method)(*w, events);
    }

    ev_io w_;
    Loop& loop_;
    K* obj_;
};

template <typename K, void (K::*Method)(ev_timer&, uint32_t), typename Trace = NullTrace>
class Timer {
public:
    Timer(Loop& loop, K* obj) noexcept : loop_(loop), obj_(obj)
    {
        ev_init(&w_, thunk);
        w_.data = this;
    }

    ~Timer() noexcept { stop(); }

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void set(ev_tstamp after, ev_tstamp repeat) noexcept
    {
        ev_timer_set(&w_, after, repeat);
    }

    void start(ev_tstamp after, ev_tstamp repeat) noexcept
    {
        set(after, repeat);
        start();
    }

    void start() noexcept { ev_timer_start(loop_.raw(), &w_); }
    void stop() noexcept { ev_timer_stop(loop_.raw(), &w_); }
    void again() noexcept { ev_timer_again(loop_.raw(), &w_); }
    bool is_active() const noexcept { return ev_is_active(&w_); }
    ev_timer* watcher() noexcept { return &w_; }

private:
    static void thunk(struct ev_loop* loop, ev_timer* w, int revents) noexcept
    {
        (void)loop;
        Timer* self = static_cast<Timer*>(w->data);
        const uint32_t events = static_cast<uint32_t>(revents);
        Trace::on_event(*w, events);
        (self->obj_->*Method)(*w, events);
    }

    ev_timer w_;
    Loop& loop_;
    K* obj_;
};

template <typename K, void (K::*Method)(ev_async&, uint32_t), typename Trace = NullTrace>
class Async {
public:
    Async(Loop& loop, K* obj) noexcept : loop_(loop), obj_(obj)
    {
        ev_init(&w_, thunk);
        w_.data = this;
    }

    ~Async() noexcept { stop(); }

    Async(const Async&) = delete;
    Async& operator=(const Async&) = delete;

    void start() noexcept { ev_async_start(loop_.raw(), &w_); }
    void stop() noexcept { ev_async_stop(loop_.raw(), &w_); }
    void send() noexcept { ev_async_send(loop_.raw(), &w_); }
    bool is_active() const noexcept { return ev_is_active(&w_); }

private:
    static void thunk(struct ev_loop* loop, ev_async* w, int revents) noexcept
    {
        (void)loop;
        Async* self = static_cast<Async*>(w->data);
        const uint32_t events = static_cast<uint32_t>(revents);
        Trace::on_event(*w, events);
        (self->obj_->*Method)(*w, events);
    }

    ev_async w_;
    Loop& loop_;
    K* obj_;
};

}  // namespace evx

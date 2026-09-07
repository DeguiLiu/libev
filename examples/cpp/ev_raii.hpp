// Minimal C++17 RAII wrappers over the libev C API. No exceptions: Loop
// reports failure via valid(). Watchers bind a member-function callback via a
// compile-time pointer-to-member template (static polymorphism, zero vtable,
// zero heap) and hold a Loop& + the bound object. Mirrors ev++.h's
// method_thunk but keeps the watcher non-copyable and the callback typed as
// a reference (void (K::*)(ev_io&, int)).
// SPDX-License-Identifier: MIT
#pragma once

#include <ev.h>

namespace evx {

class Loop {
public:
    explicit Loop(unsigned int flags = 0U) noexcept : raw_(ev_loop_new(flags)) {}

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
    int run(int flags = 0) noexcept { return ev_run(raw_, flags); }
    void break_loop() noexcept { ev_break(raw_, EVBREAK_ONE); }
    ev_tstamp now() const noexcept { return ev_now(raw_); }
    unsigned int backend() const noexcept { return ev_backend(raw_); }

private:
    struct ev_loop* raw_;
};

template <typename K, void (K::*Method)(ev_io&, int)>
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

    void set(int fd, int events) noexcept { ev_io_set(&w_, fd, events); }
    void start(int fd, int events) noexcept { set(fd, events); start(); }
    void start() noexcept { ev_io_start(loop_.raw(), &w_); }
    void stop() noexcept { ev_io_stop(loop_.raw(), &w_); }
    bool is_active() const noexcept { return ev_is_active(&w_); }
    ev_io* watcher() noexcept { return &w_; }

private:
    static void thunk(struct ev_loop* loop, ev_io* w, int revents) noexcept
    {
        (void)loop;
        Io* self = static_cast<Io*>(w->data);
        (self->obj_->*Method)(*w, revents);
    }

    ev_io w_;
    Loop& loop_;
    K* obj_;
};

template <typename K, void (K::*Method)(ev_timer&, int)>
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
        (self->obj_->*Method)(*w, revents);
    }

    ev_timer w_;
    Loop& loop_;
    K* obj_;
};

template <typename K, void (K::*Method)(ev_async&, int)>
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
        (self->obj_->*Method)(*w, revents);
    }

    ev_async w_;
    Loop& loop_;
    K* obj_;
};

}  // namespace evx

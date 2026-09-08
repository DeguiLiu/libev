// C++17 lock-free single-producer single-consumer byte ring, fixed capacity
// (power of two), zero heap. Mirrors examples/c/uart-hsm/spsc_queue.h but as
// a compile-time-capacity template with std::atomic head/tail (the C version
// uses malloc + volatile). Shared by uart_ring_hsm.cpp.
// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace spsc {

template <uint32_t Capacity>
class Ring {
    static_assert(Capacity > 0U, "capacity must be non-zero");
    static_assert((Capacity & (Capacity - 1U)) == 0U, "capacity must be a power of two");
    static_assert(std::atomic<uint32_t>::is_always_lock_free, "uint32_t atomics must be lock-free");

public:
    // Producer only: append len bytes; false when the ring cannot hold them.
    bool push(const uint8_t* data, uint32_t len) noexcept
    {
        if ((nullptr == data) || (0U == len))
        {
            return false;
        }

        const uint32_t tail = tail_.load(std::memory_order_relaxed);
        const uint32_t head = head_.load(std::memory_order_acquire);

        if ((Capacity - (tail - head)) < len)
        {
            return false;   // full
        }

        const uint32_t pos = tail & (Capacity - 1U);
        const uint32_t first = (len < (Capacity - pos)) ? len : (Capacity - pos);

        std::memcpy(&buffer_[pos], data, first);
        if (first < len)
        {
            std::memcpy(buffer_, &data[first], len - first);
        }

        tail_.store(tail + len, std::memory_order_release);
        return true;
    }

    // Consumer only: remove up to len bytes; returns the number removed (0 = empty).
    uint32_t pop(uint8_t* data, uint32_t len) noexcept
    {
        if ((nullptr == data) || (0U == len))
        {
            return 0U;
        }

        const uint32_t tail = tail_.load(std::memory_order_acquire);
        const uint32_t head = head_.load(std::memory_order_relaxed);
        const uint32_t avail = tail - head;

        if (0U == avail)
        {
            return 0U;      // empty
        }
        if (len > avail)
        {
            len = avail;
        }

        const uint32_t pos = head & (Capacity - 1U);
        const uint32_t first = (len < (Capacity - pos)) ? len : (Capacity - pos);

        std::memcpy(data, &buffer_[pos], first);
        if (first < len)
        {
            std::memcpy(&data[first], buffer_, len - first);
        }

        head_.store(head + len, std::memory_order_release);
        return len;
    }

    uint32_t data_len() const noexcept
    {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }

private:
    uint8_t buffer_[Capacity];
    std::atomic<uint32_t> head_{0U};   // consumer index
    std::atomic<uint32_t> tail_{0U};   // producer index
};

}  // namespace spsc

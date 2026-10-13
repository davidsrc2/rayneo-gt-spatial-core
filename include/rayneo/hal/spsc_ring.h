#pragma once

#include <array>
#include <atomic>
#include <cstddef>

namespace rayneo::hal {

// Wait-free single-producer / single-consumer ring. Capacity must be a power of two.
template <typename T, size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool try_push(const T& v) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity) return false;
        buf_[head & (Capacity - 1)] = v;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false;
        out = buf_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    std::array<T, Capacity> buf_{};
};

}  // namespace rayneo::hal

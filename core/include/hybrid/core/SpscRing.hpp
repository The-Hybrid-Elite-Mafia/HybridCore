#pragma once
// =============================================================================
//  SpscRing — wait-free single-producer / single-consumer ring.
//  tryPush / tryPop complete in a bounded number of steps, never block,
//  never allocate. Producer and consumer state live on separate cache lines.
// =============================================================================
#include "hybrid/core/Config.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace hc::core {

template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing slots must be trivially copyable.");
    static_assert(Capacity >= 2 && std::has_single_bit(Capacity), "Capacity must be a power of two.");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

public:
    static constexpr std::size_t kCapacity = Capacity;

    SpscRing() noexcept = default;
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer thread only.
    [[nodiscard]] bool tryPush(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tailCache_ == Capacity) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (head - tailCache_ == Capacity) return false;
        }
        slots_[head & kMask] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer thread only.
    [[nodiscard]] bool tryPop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == headCache_) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (tail == headCache_) return false;
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Approximate; exact only when called from a quiescent state.
    [[nodiscard]] std::size_t sizeApprox() const noexcept {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    alignas(kCacheLine) std::atomic<std::size_t> head_{0};   // written by producer
    std::size_t                                  tailCache_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};   // written by consumer
    std::size_t                                  headCache_{0};
    alignas(kCacheLine) std::array<T, Capacity>  slots_{};
};

} // namespace hc::core

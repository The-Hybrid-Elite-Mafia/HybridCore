#pragma once
// =============================================================================
//  AlignedBuffer — 64-byte aligned, zero-initialised heap block.
//  Allocate on the message thread only; the RT thread just reads/writes it.
// =============================================================================
#include "hybrid/core/Config.hpp"

#include <cstddef>
#include <cstring>
#include <malloc.h>
#include <new>
#include <type_traits>
#include <utility>

namespace hc::core {

template <typename T>
    requires std::is_trivially_copyable_v<T>
class AlignedBuffer {
public:
    AlignedBuffer() noexcept = default;
    explicit AlignedBuffer(std::size_t count) { allocate(count); }
    ~AlignedBuffer() { release(); }

    AlignedBuffer(const AlignedBuffer&)            = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;

    AlignedBuffer(AlignedBuffer&& o) noexcept
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0)) {}

    AlignedBuffer& operator=(AlignedBuffer&& o) noexcept {
        if (this != &o) {
            release();
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }

    void allocate(std::size_t count) {
        release();
        if (count == 0) return;
        void* p = ::_aligned_malloc(count * sizeof(T), kCacheLine);
        if (!p) throw std::bad_alloc{};
        std::memset(p, 0, count * sizeof(T));
        data_ = static_cast<T*>(p);
        size_ = count;
    }

    void zero() noexcept {
        if (data_) std::memset(data_, 0, size_ * sizeof(T));
    }

    [[nodiscard]] T*          data() noexcept { return data_; }
    [[nodiscard]] const T*    data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    T&       operator[](std::size_t i) noexcept { return data_[i]; }
    const T& operator[](std::size_t i) const noexcept { return data_[i]; }

private:
    void release() noexcept {
        if (data_) ::_aligned_free(data_);
        data_ = nullptr;
        size_ = 0;
    }

    T*          data_ = nullptr;
    std::size_t size_ = 0;
};

} // namespace hc::core

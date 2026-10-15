#pragma once
// AVX2 sample-format kernels. Internal to hc::io. All functions are RT-safe.
#include <cstddef>
#include <cstdint>

namespace hc::io::convert {

// planar float -> interleaved float (AVX2 fast path for stereo)
void interleave(const float* const* src, std::uint32_t channels, std::uint32_t frames, float* dst) noexcept;

// float [-1,1] -> integer containers (clamped, NaN -> -FS, round-to-nearest)
void floatToS32(const float* src, std::int32_t* dst, std::size_t n) noexcept;
void floatToS24In32(const float* src, std::int32_t* dst, std::size_t n) noexcept;   // 24 valid bits, MSB-aligned
void floatToS16(const float* src, std::int16_t* dst, std::size_t n) noexcept;
void floatToS24Packed(const float* src, std::uint8_t* dst, std::size_t n) noexcept;  // 3-byte LE
void floatToF64(const float* src, double* dst, std::size_t n) noexcept;

// integer/double containers -> float
void s32ToFloat(const std::int32_t* src, float* dst, std::size_t n) noexcept;
void s16ToFloat(const std::int16_t* src, float* dst, std::size_t n) noexcept;
void s24PackedToFloat(const std::uint8_t* src, float* dst, std::size_t n) noexcept;
void f64ToFloat(const double* src, float* dst, std::size_t n) noexcept;

} // namespace hc::io::convert

#pragma once
// =============================================================================
//  hc::dsp — AVX2 audio array kernels. All functions are RT-safe:
//  no allocation, no locks, no branches on data except loop tails.
//  Buffers may be unaligned; 32-byte aligned buffers run at full speed.
//  `n` may be any length (tails handled with scalar code of identical maths).
// =============================================================================
#include <cstddef>

namespace hc::dsp {

void clear(float* dst, std::size_t n) noexcept;
void copy(float* dst, const float* src, std::size_t n) noexcept;

// dst *= g
void scale(float* dst, float g, std::size_t n) noexcept;
// dst[i] *= g0 + (g1 - g0) * i / n      (linear ramp across the block)
void scaleRamp(float* dst, float g0, float g1, std::size_t n) noexcept;

// dst += src * g
void mixAdd(float* dst, const float* src, float g, std::size_t n) noexcept;
// dst[i] += src[i] * (g0 + (g1 - g0) * i / n)
void mixAddRamp(float* dst, const float* src, float g0, float g1, std::size_t n) noexcept;

// max |src[i]|
[[nodiscard]] float peakAbs(const float* src, std::size_t n) noexcept;
// sum src[i]^2 (accumulated in double lanes for long blocks)
[[nodiscard]] double sumSquares(const float* src, std::size_t n) noexcept;

} // namespace hc::dsp

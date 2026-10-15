#include "SampleConvert.hpp"

#include <immintrin.h>
#include <cmath>
#include <cstring>

namespace hc::io::convert {
namespace {

constexpr float kMax32 = 1.0f - 1.0f / 16777216.0f;   // 0.99999994f: keeps x*2^31 < 2^31
constexpr float kMax24 = 8388607.0f / 8388608.0f;
constexpr float kMax16 = 32767.0f / 32768.0f;

constexpr float kScale32 = 2147483648.0f;
constexpr float kScale24 = 8388608.0f;
constexpr float kScale16 = 32768.0f;

// NaN fails both comparisons -> -1 (matches _mm256_max_ps(x, lo) semantics).
inline float clampSample(float x, float hi) noexcept {
    return x > hi ? hi : (x >= -1.0f ? x : -1.0f);
}

inline __m256 clampSample(__m256 x, __m256 hi) noexcept {
    return _mm256_min_ps(_mm256_max_ps(x, _mm256_set1_ps(-1.0f)), hi);
}

} // namespace

void interleave(const float* const* src, std::uint32_t channels, std::uint32_t frames, float* dst) noexcept {
    if (channels == 1) {
        std::memcpy(dst, src[0], frames * sizeof(float));
        return;
    }
    if (channels == 2) {
        const float* l = src[0];
        const float* r = src[1];
        std::uint32_t i = 0;
        for (; i + 8 <= frames; i += 8) {
            const __m256 vl = _mm256_loadu_ps(l + i);
            const __m256 vr = _mm256_loadu_ps(r + i);
            const __m256 lo = _mm256_unpacklo_ps(vl, vr);   // L0R0L1R1 | L4R4L5R5
            const __m256 hi = _mm256_unpackhi_ps(vl, vr);   // L2R2L3R3 | L6R6L7R7
            _mm256_storeu_ps(dst + 2 * i,     _mm256_permute2f128_ps(lo, hi, 0x20));
            _mm256_storeu_ps(dst + 2 * i + 8, _mm256_permute2f128_ps(lo, hi, 0x31));
        }
        for (; i < frames; ++i) {
            dst[2 * i]     = l[i];
            dst[2 * i + 1] = r[i];
        }
        return;
    }
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            dst[f * channels + c] = src[c][f];
}

void floatToS32(const float* src, std::int32_t* dst, std::size_t n) noexcept {
    const __m256 hi = _mm256_set1_ps(kMax32);
    const __m256 sc = _mm256_set1_ps(kScale32);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 x = clampSample(_mm256_loadu_ps(src + i), hi);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_cvtps_epi32(_mm256_mul_ps(x, sc)));
    }
    for (; i < n; ++i)
        dst[i] = static_cast<std::int32_t>(std::lrintf(clampSample(src[i], kMax32) * kScale32));
}

void floatToS24In32(const float* src, std::int32_t* dst, std::size_t n) noexcept {
    const __m256 hi = _mm256_set1_ps(kMax24);
    const __m256 sc = _mm256_set1_ps(kScale24);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256  x = clampSample(_mm256_loadu_ps(src + i), hi);
        const __m256i v = _mm256_cvtps_epi32(_mm256_mul_ps(x, sc));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_slli_epi32(v, 8));
    }
    for (; i < n; ++i) {
        const auto v = static_cast<std::int32_t>(std::lrintf(clampSample(src[i], kMax24) * kScale24));
        dst[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(v) << 8);
    }
}

void floatToS16(const float* src, std::int16_t* dst, std::size_t n) noexcept {
    const __m256 hi = _mm256_set1_ps(kMax16);
    const __m256 sc = _mm256_set1_ps(kScale16);
    std::size_t i = 0;
    for (; i + 16 <= n; i += 16) {
        const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(clampSample(_mm256_loadu_ps(src + i), hi), sc));
        const __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(clampSample(_mm256_loadu_ps(src + i + 8), hi), sc));
        const __m256i packed = _mm256_permute4x64_epi64(_mm256_packs_epi32(a, b), 0xD8);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), packed);
    }
    for (; i < n; ++i)
        dst[i] = static_cast<std::int16_t>(std::lrintf(clampSample(src[i], kMax16) * kScale16));
}

void floatToS24Packed(const float* src, std::uint8_t* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<std::uint32_t>(std::lrintf(clampSample(src[i], kMax24) * kScale24));
        dst[3 * i]     = static_cast<std::uint8_t>(v);
        dst[3 * i + 1] = static_cast<std::uint8_t>(v >> 8);
        dst[3 * i + 2] = static_cast<std::uint8_t>(v >> 16);
    }
}

void floatToF64(const float* src, double* dst, std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
        _mm256_storeu_pd(dst + i, _mm256_cvtps_pd(_mm_loadu_ps(src + i)));
    for (; i < n; ++i) dst[i] = static_cast<double>(src[i]);
}

void s32ToFloat(const std::int32_t* src, float* dst, std::size_t n) noexcept {
    const __m256 k = _mm256_set1_ps(1.0f / kScale32);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(v), k));
    }
    for (; i < n; ++i) dst[i] = static_cast<float>(src[i]) * (1.0f / kScale32);
}

void s16ToFloat(const std::int16_t* src, float* dst, std::size_t n) noexcept {
    const __m256 k = _mm256_set1_ps(1.0f / kScale16);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_cvtepi16_epi32(v)), k));
    }
    for (; i < n; ++i) dst[i] = static_cast<float>(src[i]) * (1.0f / kScale16);
}

void s24PackedToFloat(const std::uint8_t* src, float* dst, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t u = static_cast<std::uint32_t>(src[3 * i]) |
                                (static_cast<std::uint32_t>(src[3 * i + 1]) << 8) |
                                (static_cast<std::uint32_t>(src[3 * i + 2]) << 16);
        const std::int32_t v = static_cast<std::int32_t>(u << 8) >> 8;   // sign-extend
        dst[i] = static_cast<float>(v) * (1.0f / kScale24);
    }
}

void f64ToFloat(const double* src, float* dst, std::size_t n) noexcept {
    std::size_t i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_ps(dst + i, _mm256_cvtpd_ps(_mm256_loadu_pd(src + i)));
    for (; i < n; ++i) dst[i] = static_cast<float>(src[i]);
}

} // namespace hc::io::convert

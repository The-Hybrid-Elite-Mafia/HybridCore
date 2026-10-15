#include "hybrid/dsp/Kernels.hpp"

#include <immintrin.h>

#include <cmath>
#include <cstring>

namespace hc::dsp {

namespace {
// Lane indices 0..7. Gains are computed as g0 + step * index for every sample (no running
// sum), so long blocks carry no accumulated rounding drift and match the scalar tail exactly.
inline __m256 laneIndex() noexcept { return _mm256_setr_ps(0.f, 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f); }
} // namespace

void clear(float* dst, std::size_t n) noexcept {
    const __m256 z = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, z);
    for (; i < n; ++i) dst[i] = 0.0f;
}

void copy(float* dst, const float* src, std::size_t n) noexcept {
    if (dst != src && n) std::memcpy(dst, src, n * sizeof(float));
}

void scale(float* dst, float g, std::size_t n) noexcept {
    const __m256 vg = _mm256_set1_ps(g);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(dst + i), vg));
    for (; i < n; ++i) dst[i] *= g;
}

void scaleRamp(float* dst, float g0, float g1, std::size_t n) noexcept {
    if (n == 0) return;
    if (g0 == g1) { scale(dst, g0, n); return; }
    const float  step  = (g1 - g0) / static_cast<float>(n);
    const __m256 vStep = _mm256_set1_ps(step);
    const __m256 vG0   = _mm256_set1_ps(g0);
    const __m256 eight = _mm256_set1_ps(8.0f);
    __m256       idx   = laneIndex();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 g = _mm256_fmadd_ps(idx, vStep, vG0);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(_mm256_loadu_ps(dst + i), g));
        idx = _mm256_add_ps(idx, eight);
    }
    for (; i < n; ++i) dst[i] *= std::fma(static_cast<float>(i), step, g0);
}

void mixAdd(float* dst, const float* src, float g, std::size_t n) noexcept {
    const __m256 vg = _mm256_set1_ps(g);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8)
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(_mm256_loadu_ps(src + i), vg, _mm256_loadu_ps(dst + i)));
    for (; i < n; ++i) dst[i] = std::fma(src[i], g, dst[i]);
}

void mixAddRamp(float* dst, const float* src, float g0, float g1, std::size_t n) noexcept {
    if (n == 0) return;
    if (g0 == g1) { mixAdd(dst, src, g0, n); return; }
    const float  step  = (g1 - g0) / static_cast<float>(n);
    const __m256 vStep = _mm256_set1_ps(step);
    const __m256 vG0   = _mm256_set1_ps(g0);
    const __m256 eight = _mm256_set1_ps(8.0f);
    __m256       idx   = laneIndex();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256 g = _mm256_fmadd_ps(idx, vStep, vG0);
        _mm256_storeu_ps(dst + i, _mm256_fmadd_ps(_mm256_loadu_ps(src + i), g, _mm256_loadu_ps(dst + i)));
        idx = _mm256_add_ps(idx, eight);
    }
    for (; i < n; ++i) dst[i] = std::fma(src[i], std::fma(static_cast<float>(i), step, g0), dst[i]);
}

float peakAbs(const float* src, std::size_t n) noexcept {
    const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
    __m256 m = _mm256_setzero_ps();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) m = _mm256_max_ps(m, _mm256_and_ps(_mm256_loadu_ps(src + i), absMask));
    __m128 h = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
    h = _mm_max_ps(h, _mm_movehl_ps(h, h));
    h = _mm_max_ss(h, _mm_shuffle_ps(h, h, 0x55));
    float peak = _mm_cvtss_f32(h);
    for (; i < n; ++i) {
        const float a = std::fabs(src[i]);
        if (a > peak) peak = a;
    }
    return peak;
}

double sumSquares(const float* src, std::size_t n) noexcept {
    __m256d acc0 = _mm256_setzero_pd();
    __m256d acc1 = _mm256_setzero_pd();
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256  x  = _mm256_loadu_ps(src + i);
        const __m256d lo = _mm256_cvtps_pd(_mm256_castps256_ps128(x));
        const __m256d hi = _mm256_cvtps_pd(_mm256_extractf128_ps(x, 1));
        acc0 = _mm256_fmadd_pd(lo, lo, acc0);
        acc1 = _mm256_fmadd_pd(hi, hi, acc1);
    }
    const __m256d acc = _mm256_add_pd(acc0, acc1);
    __m128d s = _mm_add_pd(_mm256_castpd256_pd128(acc), _mm256_extractf128_pd(acc, 1));
    s = _mm_add_sd(s, _mm_unpackhi_pd(s, s));
    double sum = _mm_cvtsd_f64(s);
    for (; i < n; ++i) sum += static_cast<double>(src[i]) * src[i];
    return sum;
}

} // namespace hc::dsp

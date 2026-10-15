#include "hybrid/media/Resampler.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace hc::media {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kStopbandDb = 100.0;

double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

inline float hsum(__m256 v) {
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}
} // namespace

Resampler::Resampler(std::uint32_t inRate, std::uint32_t outRate) : inRate_(inRate), outRate_(outRate) {
    const std::uint64_t g = std::gcd<std::uint64_t>(inRate, outRate);
    in_  = inRate / g;
    out_ = outRate / g;
    if (in_ == out_) return;

    const double lower = std::min(inRate, outRate);
    passHz_ = 0.4535 * lower;
    stopHz_ = 0.5 * lower;
    const double fc = 0.5 * (passHz_ + stopHz_) / inRate;           // cutoff, cycles per INPUT sample
    const double df = (stopHz_ - passHz_) / inRate;                  // transition width, cycles per input sample
    const double beta = 0.1102 * (kStopbandDb - 8.7);
    int n = static_cast<int>(std::ceil((kStopbandDb - 7.95) / (2.285 * 2.0 * kPi * df)));
    taps_ = ((n + 7) / 8) * 8;
    const double half = taps_ * 0.5;
    const double i0b = besselI0(beta);

    // Row p holds the taps for fractional position p / kPhases; tap k multiplies input[i - (taps/2 - 1) + k].
    std::vector<double> rows(static_cast<std::size_t>(kPhases + 1) * taps_);
    for (int p = 0; p <= kPhases; ++p) {
        const double phi = static_cast<double>(p) / kPhases;
        double sum = 0;
        for (int k = 0; k < taps_; ++k) {
            const double t = (k - (taps_ / 2 - 1)) - phi;             // distance from the output position
            const double x = 2.0 * fc * t;
            const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double r = t / half;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            const double h = 2.0 * fc * sinc * w;
            rows[static_cast<std::size_t>(p) * taps_ + k] = h;
            sum += h;
        }
        for (int k = 0; k < taps_; ++k) rows[static_cast<std::size_t>(p) * taps_ + k] /= sum;   // exact unity gain at DC
    }
    table_.resize(static_cast<std::size_t>(kPhases) * taps_);
    delta_.resize(table_.size());
    for (int p = 0; p < kPhases; ++p)
        for (int k = 0; k < taps_; ++k) {
            const double a = rows[static_cast<std::size_t>(p) * taps_ + k], b = rows[static_cast<std::size_t>(p + 1) * taps_ + k];
            table_[static_cast<std::size_t>(p) * taps_ + k] = static_cast<float>(a);
            delta_[static_cast<std::size_t>(p) * taps_ + k] = static_cast<float>(b - a);
        }
}

std::uint64_t Resampler::outputLength(std::uint64_t n) const noexcept {
    return (n * out_ + in_ - 1) / in_;
}

bool Resampler::process(const float* inL, const float* inR, std::uint64_t n, float* outL, float* outR, const Progress& progress) const {
    const std::uint64_t m = outputLength(n);
    if (in_ == out_) {
        std::memcpy(outL, inL, sizeof(float) * n);
        std::memcpy(outR, inR, sizeof(float) * n);
        return !progress || progress(1.0f);
    }
    const int T = taps_, lead = T / 2 - 1;
    for (std::uint64_t o = 0; o < m; ++o) {
        const std::uint64_t num = o * in_;
        const std::uint64_t i = num / out_;
        const double phi = static_cast<double>(num % out_) / static_cast<double>(out_);
        const double fp = phi * kPhases;
        const int p = std::min(static_cast<int>(fp), kPhases - 1);
        const float t = static_cast<float>(fp - p);
        const float* a = &table_[static_cast<std::size_t>(p) * T];
        const float* d = &delta_[static_cast<std::size_t>(p) * T];
        const std::int64_t first = static_cast<std::int64_t>(i) - lead;
        if (first >= 0 && first + T <= static_cast<std::int64_t>(n)) {
            const float* l = inL + first;
            const float* r = inR + first;
            const __m256 vt = _mm256_set1_ps(t);
            __m256 al = _mm256_setzero_ps(), ar = _mm256_setzero_ps();
            for (int k = 0; k < T; k += 8) {
                const __m256 c = _mm256_fmadd_ps(vt, _mm256_loadu_ps(d + k), _mm256_loadu_ps(a + k));
                al = _mm256_fmadd_ps(_mm256_loadu_ps(l + k), c, al);
                ar = _mm256_fmadd_ps(_mm256_loadu_ps(r + k), c, ar);
            }
            outL[o] = hsum(al);
            outR[o] = hsum(ar);
        } else {   // file edges: samples outside the file are zero
            float sl = 0, sr = 0;
            for (int k = 0; k < T; ++k) {
                const std::int64_t j = first + k;
                if (j < 0 || j >= static_cast<std::int64_t>(n)) continue;
                const float c = a[k] + t * d[k];
                sl += inL[j] * c;
                sr += inR[j] * c;
            }
            outL[o] = sl;
            outR[o] = sr;
        }
        if (progress && (o & 0xFFFF) == 0xFFFF && !progress(static_cast<float>(o) / static_cast<float>(m))) return false;
    }
    return !progress || progress(1.0f);
}

bool resampleTo(DecodedAudio& a, std::uint32_t outRate, const Progress& progress) {
    if (a.sampleRate == outRate) return !progress || progress(1.0f);
    const Resampler rs(a.sampleRate, outRate);
    const std::uint64_t m = rs.outputLength(a.frames());
    std::vector<float> l(static_cast<std::size_t>(m)), r(static_cast<std::size_t>(m));
    if (!rs.process(a.left.data(), a.right.data(), a.frames(), l.data(), r.data(), progress)) return false;
    a.left = std::move(l);
    a.right = std::move(r);
    a.sampleRate = outRate;
    return true;
}

} // namespace hc::media

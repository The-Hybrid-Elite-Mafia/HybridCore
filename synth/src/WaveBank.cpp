#include "WaveBank.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>

namespace hc::synth::detail {

const char* const kTableNames[kTables] = {"BASIC", "PULSE", "VOCAL", "DIGITAL", "ORGAN", "FM BELL", "AIR", "HOLLOW"};

namespace {

using cd = std::complex<double>;
constexpr double kPi = std::numbers::pi;

// In-place radix-2 inverse FFT (no 1/N scaling).
void ifft(std::vector<cd>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * kPi / static_cast<double>(len);
        const cd wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            cd w(1);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

double besselJ(int n, double x) {   // (1/π) ∫0^π cos(nτ − x sin τ) dτ
    constexpr int kSteps = 256;
    double s = 0;
    for (int i = 0; i <= kSteps; ++i) {
        const double t = kPi * i / kSteps, w = (i == 0 || i == kSteps) ? 0.5 : 1.0;
        s += w * std::cos(n * t - x * std::sin(t));
    }
    return s / kSteps;
}

double lerp(double a, double b, double t) { return a + (b - a) * t; }

// Harmonic k (1-based) of `table` at morph t (0..1): waveform = Σ re·cos(kφ) + im·sin(kφ).
void spectrum(int table, double t, std::vector<double>& re, std::vector<double>& im) {
    std::fill(re.begin(), re.end(), 0.0);
    std::fill(im.begin(), im.end(), 0.0);
    const int H = kMaxHarmonics;
    switch (table) {
    case 0: {   // BASIC: sine → triangle → saw → square
        auto sine = [](int k) { return k == 1 ? 1.0 : 0.0; };
        auto tri = [](int k) { return (k & 1) ? 8 / (kPi * kPi) * (((k - 1) / 2) % 2 ? -1.0 : 1.0) / (k * k) : 0.0; };
        auto saw = [](int k) { return 2 / kPi * ((k & 1) ? 1.0 : -1.0) / k; };
        auto sqr = [](int k) { return (k & 1) ? 4 / (kPi * k) : 0.0; };
        const double seg = t * 3.0;
        for (int k = 1; k <= H; ++k) {
            double v;
            if (seg < 1) v = lerp(sine(k), tri(k), seg);
            else if (seg < 2) v = lerp(tri(k), saw(k), seg - 1);
            else v = lerp(saw(k), sqr(k), std::min(seg - 2, 1.0));
            im[static_cast<std::size_t>(k)] = v;
        }
        break;
    }
    case 1: {   // PULSE: width 50 % → 4 %
        const double w = lerp(0.5, 0.04, t);
        for (int k = 1; k <= H; ++k) {
            const double c = 2 / (kPi * k);
            re[static_cast<std::size_t>(k)] = c * std::sin(2 * kPi * k * w);
            im[static_cast<std::size_t>(k)] = c * (1 - std::cos(2 * kPi * k * w));
        }
        break;
    }
    case 2: {   // VOCAL: a buzzy source through vowel formants a → e → i → o → u (as if sung at ~130 Hz)
        static const double F[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
        static const double A[3] = {1.0, 0.6, 0.25};
        const double x = t * 4.0;
        const int v0 = std::min(static_cast<int>(x), 3);
        const double f = x - v0;
        for (int k = 1; k <= H; ++k) {
            const double hz = 130.0 * k;
            double g = 0.08;
            for (int j = 0; j < 3; ++j) {
                const double fj = lerp(F[v0][j], F[v0 + 1][j], f), d = std::log2(hz / fj) / 0.18;
                g += A[j] * std::exp(-0.5 * d * d);
            }
            im[static_cast<std::size_t>(k)] = g * std::pow(static_cast<double>(k), -0.7);
        }
        break;
    }
    case 3: {   // DIGITAL: a narrow band of harmonics sweeping up from the fundamental
        const double c = 1.0 + 63.0 * t * t, w = 1.2 + 2.0 * t;
        for (int k = 1; k <= H; ++k) {
            const double d = (k - c) / w;
            im[static_cast<std::size_t>(k)] = std::exp(-d * d) + (k == 1 ? 0.35 : 0.0);
            if (k % 2 == 0) im[static_cast<std::size_t>(k)] = -im[static_cast<std::size_t>(k)];
        }
        break;
    }
    case 4: {   // ORGAN: drawbar registrations on harmonics 1, 2, 3, 4, 6, 8, 10, 12
        static const int kH[8] = {1, 2, 3, 4, 6, 8, 10, 12};
        static const double R[4][8] = {{1, 0, 0, 0, 0, 0, 0, 0}, {1, 0.8, 0.5, 0, 0, 0, 0, 0}, {1, 1, 1, 0.8, 0.6, 0.4, 0, 0}, {0.6, 1, 0.8, 1, 1, 0.9, 0.7, 0.6}};
        const double x = t * 3.0;
        const int r0 = std::min(static_cast<int>(x), 2);
        for (int j = 0; j < 8; ++j) im[static_cast<std::size_t>(kH[j])] = lerp(R[r0][j], R[r0 + 1][j], x - r0) / std::sqrt(static_cast<double>(kH[j]));
        break;
    }
    case 5: {   // FM BELL: carrier 1, modulator 3, index 0.2 → 9 (partials 1 ± 3n folded onto harmonics)
        const double I = 0.2 + 8.8 * t;
        for (int n = -60; n <= 60; ++n) {
            const int p = 1 + 3 * n;
            const double a = besselJ(n, I);
            if (p == 0) continue;
            const int k = std::abs(p);
            if (k > H) continue;
            im[static_cast<std::size_t>(k)] += p > 0 ? a : -a;   // sin(−x) = −sin(x)
        }
        break;
    }
    case 6: {   // AIR: dense random-phase partials; the morph opens the top and thins the bottom
        std::uint32_t s = 12345u;
        auto rnd = [&s]() { s = s * 1664525u + 1013904223u; return static_cast<double>(s >> 8) / 16777216.0; };
        const double tilt = lerp(1.6, 0.6, t);
        for (int k = 1; k <= H; ++k) {
            const double a = (0.3 + 0.7 * rnd()) * std::pow(static_cast<double>(k), -tilt) * (k < 3 ? lerp(1.0, 0.2, t) : 1.0);
            const double ph = 2 * kPi * rnd();
            re[static_cast<std::size_t>(k)] = a * std::cos(ph);
            im[static_cast<std::size_t>(k)] = a * std::sin(ph);
        }
        break;
    }
    default: {   // HOLLOW: odd harmonics with a moving resonance; even ones fade in late
        const double c = 3.0 + 30.0 * t;
        for (int k = 1; k <= H; ++k) {
            const double d = (k - c) / 3.0, res = 1.0 + 3.0 * std::exp(-d * d);
            im[static_cast<std::size_t>(k)] = ((k & 1) ? 1.0 : 0.5 * t * t) / k * res;
        }
        break;
    }
    }
}

WaveBank build() {
    WaveBank b;
    int stride = 0;
    for (int m = 0; m < kMips; ++m) {
        b.len[static_cast<std::size_t>(m)] = std::clamp(16 * WaveBank::harmonics(m), 256, 2048);
        b.off[static_cast<std::size_t>(m)] = stride;
        stride += b.len[static_cast<std::size_t>(m)] + 1;   // + guard sample (= first sample)
    }
    b.frameStride = stride;
    b.data.assign(static_cast<std::size_t>(kTables) * kFrames * static_cast<std::size_t>(stride), 0.0f);
    std::vector<double> re(kMaxHarmonics + 1), im(kMaxHarmonics + 1);
    std::vector<cd> buf;
    for (int tb = 0; tb < kTables; ++tb)
        for (int f = 0; f < kFrames; ++f) {
            spectrum(tb, static_cast<double>(f) / (kFrames - 1), re, im);
            double scale = 1.0;
            for (int m = 0; m < kMips; ++m) {
                const int L = b.len[static_cast<std::size_t>(m)], H = std::min(WaveBank::harmonics(m), L / 2 - 1);
                buf.assign(static_cast<std::size_t>(L), cd(0, 0));
                for (int k = 1; k <= H; ++k) buf[static_cast<std::size_t>(k)] = cd(re[static_cast<std::size_t>(k)], -im[static_cast<std::size_t>(k)]);
                ifft(buf);
                if (m == 0) {   // every mip of a frame shares the full-band frame's loudness
                    double pk = 0;
                    for (const cd& v : buf) pk = std::max(pk, std::fabs(v.real()));
                    scale = pk > 1e-12 ? 1.0 / pk : 0.0;
                }
                float* w = b.data.data() + static_cast<std::size_t>(tb * kFrames + f) * static_cast<std::size_t>(stride) + static_cast<std::size_t>(b.off[static_cast<std::size_t>(m)]);
                for (int i = 0; i < L; ++i) w[i] = static_cast<float>(buf[static_cast<std::size_t>(i)].real() * scale);
                w[L] = w[0];
            }
        }
    return b;
}

} // namespace

const WaveBank& WaveBank::get() {
    static const WaveBank bank = build();
    return bank;
}

} // namespace hc::synth::detail

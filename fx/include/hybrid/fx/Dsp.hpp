#pragma once
// =============================================================================
//  Small building blocks shared by the HEM effects (header-only, allocation-free
//  after prepare): RBJ biquads, one-pole smoothers, envelope followers, 4x oversampler.
// =============================================================================
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace hc::fx {

// ---- biquad (RBJ Audio EQ Cookbook), transposed direct form II, double state -------
struct BiquadCoefs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    enum class Type : std::uint8_t { LowPass, HighPass, Bell, LowShelf, HighShelf, BandPass, Notch, AllPass };
    static BiquadCoefs make(Type t, double fs, double hz, double q, double gainDb = 0.0) noexcept {
        hz = std::clamp(hz, 5.0, fs * 0.49);
        q  = std::max(q, 0.025);
        const double A = std::pow(10.0, gainDb / 40.0), w = 2.0 * std::numbers::pi * hz / fs;
        const double cw = std::cos(w), sw = std::sin(w), al = sw / (2.0 * q);
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (t) {
        case Type::LowPass:  b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case Type::HighPass: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case Type::BandPass: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case Type::Notch:    b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case Type::AllPass:  b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;
        case Type::Bell:     b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;
        case Type::LowShelf: {
            const double s = 2 * std::sqrt(A) * al;
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s; a1 = -2 * ((A - 1) + (A + 1) * cw); a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case Type::HighShelf: {
            const double s = 2 * std::sqrt(A) * al;
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s; a1 = 2 * ((A - 1) - (A + 1) * cw); a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        }
        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }
    // |H| in dB at hz (tests, displays).
    [[nodiscard]] double magnitudeDb(double fs, double hz) const noexcept {
        const double w = 2.0 * std::numbers::pi * hz / fs;
        const double cr = std::cos(w), ci = -std::sin(w), c2r = std::cos(2 * w), c2i = -std::sin(2 * w);
        const double nr = b0 + b1 * cr + b2 * c2r, ni = b1 * ci + b2 * c2i;
        const double dr = 1 + a1 * cr + a2 * c2r, di = a1 * ci + a2 * c2i;
        return 10.0 * std::log10((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1e-30));
    }
};

struct Biquad {
    BiquadCoefs c;
    double z1 = 0, z2 = 0;
    void clear() noexcept { z1 = z2 = 0; }
    [[nodiscard]] float tick(float xf) noexcept {
        const double x = xf, y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return static_cast<float>(y);
    }
    void run(float* x, std::uint32_t n) noexcept { for (std::uint32_t i = 0; i < n; ++i) x[i] = tick(x[i]); }
};

// Stereo pair sharing coefficients.
struct Biquad2 {
    Biquad l, r;
    void set(const BiquadCoefs& c) noexcept { l.c = c; r.c = c; }
    void clear() noexcept { l.clear(); r.clear(); }
    void run(float* L, float* R, std::uint32_t n) noexcept { l.run(L, n); r.run(R, n); }
};

// ---- smoothing / envelopes ---------------------------------------------------------
[[nodiscard]] inline float onePoleCoef(double fs, double ms) noexcept {
    return ms <= 0.0 ? 0.0f : static_cast<float>(std::exp(-1.0 / (fs * ms * 0.001)));
}

// Linear-in-value smoother for a parameter, stepped per sample (20 ms by default).
struct Smoothed {
    float cur = 0, coef = 0;
    void prepare(double fs, double ms, float v) noexcept { coef = onePoleCoef(fs, ms); cur = v; }
    [[nodiscard]] float next(float target) noexcept { cur = target + coef * (cur - target); return cur; }
};

// Peak envelope follower with separate attack / release (dB-free, linear domain).
struct Envelope {
    float att = 0, rel = 0, env = 0;
    void set(double fs, double attackMs, double releaseMs) noexcept { att = onePoleCoef(fs, attackMs); rel = onePoleCoef(fs, releaseMs); }
    [[nodiscard]] float tick(float x) noexcept {
        const float c = x > env ? att : rel;
        env = x + c * (env - x);
        return env;
    }
};

// ---- 4x oversampler: linear-phase polyphase FIR (Kaiser-windowed sinc) -------------------
// up(): n base-rate samples → 4n; down(): 4n → n. Latency (round trip) = latency() base samples.
class Oversampler4 {
public:
    static constexpr int kFactor = 4, kTapsPerPhase = 24, kTaps = kFactor * kTapsPerPhase, kDesign = kTaps - 3;

    // cutoff: low-pass edge as a fraction of the base Nyquist. 0.9 (default) keeps images of
    // distortion products away; 1.0 is flat to 20 kHz (-0.1 dB) for peak detection.
    explicit Oversampler4(double cutoff = 0.9) noexcept {
        // Low-pass at 0.45 × the base Nyquist... expressed at the 4x rate: cutoff = 0.5/4 × 0.9.
        // Designed with kDesign (odd) taps so each filter delays a whole number of 4x samples
        // and the round trip a whole number of base samples; the last taps stay zero.
        const double fc = 0.5 / kFactor * cutoff, beta = 8.0;
        const double mid = (kDesign - 1) / 2.0;
        double sum = 0;
        for (int i = 0; i < kDesign; ++i) {
            const double x = i - mid, s = x == 0 ? 2 * fc : std::sin(2 * std::numbers::pi * fc * x) / (std::numbers::pi * x);
            const double r = 2.0 * i / (kDesign - 1) - 1.0;
            h_[static_cast<std::size_t>(i)] = s * bessel0(beta * std::sqrt(std::max(0.0, 1 - r * r))) / bessel0(beta);
            sum += h_[static_cast<std::size_t>(i)];
        }
        for (double& v : h_) v /= sum;
        clear();
    }
    void clear() noexcept { upHist_.fill(0.0f); downHist_.fill(0.0f); upPos_ = downPos_ = 0; }
    // Round-trip delay in base-rate samples: (kDesign-1)/2 4x-samples per filter, exactly.
    [[nodiscard]] static constexpr std::uint32_t latency() noexcept { return static_cast<std::uint32_t>((kDesign - 1) / kFactor); }

    void up(const float* in, float* out4, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            upPos_ = (upPos_ + kTapsPerPhase - 1) % kTapsPerPhase;
            upHist_[static_cast<std::size_t>(upPos_)] = in[i];
            upHist_[static_cast<std::size_t>(upPos_ + kTapsPerPhase)] = in[i];
            const float* hst = &upHist_[static_cast<std::size_t>(upPos_)];
            for (int ph = 0; ph < kFactor; ++ph) {
                double acc = 0;
                for (int k = 0; k < kTapsPerPhase; ++k) acc += h_[static_cast<std::size_t>(k * kFactor + ph)] * hst[k];
                out4[i * kFactor + static_cast<std::uint32_t>(ph)] = static_cast<float>(acc * kFactor);
            }
        }
    }
    void down(const float* in4, float* out, std::uint32_t n) noexcept {
        for (std::uint32_t i = 0; i < n; ++i) {
            // Keep the 4x sample that lines up with base sample i (phase 0): the output is then
            // exactly latency() base samples late, with no fractional offset.
            for (int ph = 0; ph < kFactor; ++ph) {
                downPos_ = (downPos_ + kTaps - 1) % kTaps;
                downHist_[static_cast<std::size_t>(downPos_)] = in4[i * kFactor + static_cast<std::uint32_t>(ph)];
                downHist_[static_cast<std::size_t>(downPos_ + kTaps)] = in4[i * kFactor + static_cast<std::uint32_t>(ph)];
                if (ph == 0) {
                    const float* hst = &downHist_[static_cast<std::size_t>(downPos_)];
                    double acc = 0;
                    for (int k = 0; k < kDesign; ++k) acc += h_[static_cast<std::size_t>(k)] * hst[k];
                    out[i] = static_cast<float>(acc);
                }
            }
        }
    }

private:
    static double bessel0(double x) noexcept {
        double s = 1, t = 1;
        for (int k = 1; k < 30; ++k) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
        return s;
    }
    std::array<double, kTaps> h_{};
    std::array<float, 2 * kTapsPerPhase> upHist_{};
    std::array<float, 2 * kTaps> downHist_{};
    int upPos_ = 0, downPos_ = 0;
};

// ---- fixed delay line (allocated in prepare) ---------------------------------------
struct DelayLine {
    std::vector<float> buf;
    std::uint32_t pos = 0;
    void prepare(std::uint32_t maxDelay) { buf.assign(maxDelay + 1, 0.0f); pos = 0; }
    void clear() noexcept { std::fill(buf.begin(), buf.end(), 0.0f); pos = 0; }
    // Writes x and returns the sample written `delay` samples ago (delay <= maxDelay).
    [[nodiscard]] float process(float x, std::uint32_t delay) noexcept {
        const auto n = static_cast<std::uint32_t>(buf.size());
        buf[pos] = x;
        const std::uint32_t r = (pos + n - delay) % n;
        pos = (pos + 1) % n;
        return buf[r];
    }
    // Fractional read (linear) `delay` samples behind the most recent write.
    [[nodiscard]] float read(float delay) const noexcept {
        const auto n = static_cast<std::uint32_t>(buf.size());
        const float d = std::clamp(delay, 0.0f, static_cast<float>(n - 2));
        const auto i = static_cast<std::uint32_t>(d);
        const float f = d - static_cast<float>(i);
        const std::uint32_t a = (pos + 2 * n - 1 - i) % n, b = (a + n - 1) % n;
        return buf[a] + f * (buf[b] - buf[a]);
    }
    void write(float x) noexcept { buf[pos] = x; pos = (pos + 1) % static_cast<std::uint32_t>(buf.size()); }
};

} // namespace hc::fx

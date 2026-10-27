#pragma once
// =============================================================================
//  hc::console — small DSP pieces shared by the console engines (header-only, no allocation).
//  2nd-order filters reuse hc::fx::BiquadCoefs (RBJ cookbook = bilinear transform of the analogue
//  prototypes the desk EQ was fitted with, pre-warped at the centre/corner frequency).
// =============================================================================
#include "hybrid/fx/Dsp.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace hc::console {

using fx::Biquad;
using fx::BiquadCoefs;
using BqType = fx::BiquadCoefs::Type;

[[nodiscard]] inline float dbToLin(float db) noexcept { return db <= -120.0f ? 0.0f : std::pow(10.0f, db * 0.05f); }
[[nodiscard]] inline float linToDb(float g) noexcept { return 20.0f * std::log10(std::max(g, 1e-12f)); }
[[nodiscard]] inline float coefMs(double fs, double ms) noexcept { return fx::onePoleCoef(fs, ms); }

// ---- 1st-order low/high pass (bilinear, pre-warped), double state --------------------------
struct FirstOrder {
    double b0 = 1, b1 = 0, a1 = 0, z = 0;
    enum class Type : std::uint8_t { LowPass, HighPass };
    void set(Type t, double fs, double hz) noexcept {
        hz = std::clamp(hz, 1.0, fs * 0.49);
        const double k = std::tan(std::numbers::pi * hz / fs);
        const double n = 1.0 / (1.0 + k);
        if (t == Type::LowPass) { b0 = k * n; b1 = k * n; }
        else                    { b0 = n;     b1 = -n;    }
        a1 = (k - 1.0) * n;
    }
    // Any 1st-order analogue section H(s) = (nb1·s/w0 + nb0) / (na1·s/w0 + na0), bilinear with pre-warp at f0.
    void setAnalog(double fs, double f0, double nb1, double nb0, double na1, double na0) noexcept {
        f0 = std::clamp(f0, 1.0, fs * 0.49);
        const double K = 1.0 / std::tan(std::numbers::pi * f0 / fs);
        const double B0 = nb1 * K + nb0, B1 = -nb1 * K + nb0, A0 = na1 * K + na0, A1 = -na1 * K + na0;
        b0 = B0 / A0; b1 = B1 / A0; a1 = A1 / A0;
    }
    // Shelves with a geometric mid-point at f0 and `gainDb` at the far end; 1st-order all-pass.
    void setLowShelf(double fs, double f0, double gainDb) noexcept {
        const double r = std::pow(10.0, gainDb / 40.0);
        setAnalog(fs, f0, 1.0, r, 1.0, 1.0 / r);
    }
    void setHighShelf(double fs, double f0, double gainDb) noexcept {
        const double r = std::pow(10.0, gainDb / 40.0);
        setAnalog(fs, f0, r, 1.0, 1.0 / r, 1.0);
    }
    void setAllPass(double fs, double f0) noexcept { setAnalog(fs, f0, -1.0, 1.0, 1.0, 1.0); }
    void clear() noexcept { z = 0; }
    [[nodiscard]] double tick(double x) noexcept {   // transposed direct form II
        const double y = b0 * x + z;
        z = b1 * x - a1 * y;
        return y;
    }
    [[nodiscard]] double magnitudeDb(double fs, double hz) const noexcept {
        const double w = 2.0 * std::numbers::pi * hz / fs;
        const double nr = b0 + b1 * std::cos(w), ni = -b1 * std::sin(w);
        const double dr = 1.0 + a1 * std::cos(w), di = -a1 * std::sin(w);
        return 10.0 * std::log10((nr * nr + ni * ni) / std::max(dr * dr + di * di, 1e-30));
    }
};

// ---- magnitude-matched bell ------------------------------------------------------------------
// The bilinear (cookbook) bell is forced to 0 dB at Nyquist, which bends very broad bells near the top
// of the band at 48 kHz. This design keeps impulse-invariant poles and solves the squared-magnitude
// numerator so the response equals the analogue prototype exactly at DC, at the centre and at Nyquist
// (measured max error vs the analogue curve: see tests). Cuts are the exact inverse of the matching
// boost, so a cut mirrors a boost.
[[nodiscard]] inline BiquadCoefs matchedBell(double fs, double f0, double q, double gainDb) noexcept {
    if (std::fabs(gainDb) < 1e-4) return {};
    const bool cut = gainDb < 0.0;
    const double g = std::fabs(gainDb);
    f0 = std::clamp(f0, 5.0, fs * 0.49);
    q = std::max(q, 0.025);
    const double A = std::pow(10.0, g / 40.0);
    auto analog2 = [&](double hz) {   // |H(j hz)|^2 of (s^2 + s A/Q + 1) / (s^2 + s/(A Q) + 1)
        const double w = hz / f0, re = 1.0 - w * w;
        const double ni = w * A / q, di = w / (A * q);
        return (re * re + ni * ni) / (re * re + di * di);
    };
    const double w0 = 2.0 * std::numbers::pi * f0 / fs;
    const double z = 1.0 / (2.0 * A * q);   // damping of the analogue poles
    double a1;
    if (z <= 1.0) a1 = -2.0 * std::exp(-z * w0) * std::cos(std::sqrt(1.0 - z * z) * w0);
    else          a1 = -2.0 * std::exp(-z * w0) * std::cosh(std::sqrt(z * z - 1.0) * w0);
    const double a2 = std::exp(-2.0 * z * w0);
    const double A0 = (1 + a1 + a2) * (1 + a1 + a2), A1 = (1 - a1 + a2) * (1 - a1 + a2), A2 = -4.0 * a2;
    const double p1 = std::sin(w0 / 2) * std::sin(w0 / 2), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
    const double B0 = A0 * analog2(0.0), B1 = A1 * analog2(fs / 2.0);
    const double B2 = ((A0 * p0 + A1 * p1 + A2 * p2) * analog2(f0) - B0 * p0 - B1 * p1) / p2;
    const double W = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
    const double b0 = 0.5 * (W + std::sqrt(std::max(W * W + B2, 0.0)));
    const double b1 = 0.5 * (std::sqrt(B0) - std::sqrt(B1));
    const double b2 = -B2 / (4.0 * b0);
    if (!cut) return {b0, b1, b2, a1, a2};
    return {1.0 / b0, a1 / b0, a2 / b0, b1 / b0, b2 / b0};   // exact inverse of the boost
}

// ---- static compressor curve ---------------------------------------------------------------
// Gain change in dB for an input level x (dB). Above the threshold the output rises 1/ratio dB per dB;
// a negative ratio makes the output FALL below the threshold (the bus processor's -2.5/-1.5/-0.5).
// knee: soft-knee width in dB (0 = hard).
[[nodiscard]] inline float compGainDb(float x, float thr, float ratio, float knee) noexcept {
    const float slope = 1.0f / ratio - 1.0f;   // dB of gain change per dB over
    const float o = x - thr;
    if (knee <= 0.0f) return o > 0.0f ? slope * o : 0.0f;
    if (2.0f * o <= -knee) return 0.0f;
    if (2.0f * o >= knee) return slope * o;
    const float k = o + knee * 0.5f;
    return slope * k * k / (2.0f * knee);
}

// ---- dual-rate gain-reduction smoother (dB domain, "smooth decoupled" peak detector) ----------
// 1. peak stage: jumps up to the target at once, falls back with the release time (holds the
//    per-cycle peaks of a waveform, so the static ratio is exact);
// 2. slow stage (only when dual): charges slowly while compression keeps going and releases over
//    `slowMs` — the deeper of the two wins, so a short transient releases fast and long compression
//    releases slowly ("auto release");
// 3. attack stage: rises toward that value with the attack time, follows it straight down.
struct DualRate {
    float peak = 0, slow = 0, out = 0;   // gain reduction in dB, >= 0
    float att = 0, rel = 0, chg = 0, slowRel = 0;
    bool dual = false;
    void set(double fs, double attackMs, double releaseMs, bool dualRate, double chargeMs, double slowMs) noexcept {
        att = coefMs(fs, attackMs); rel = coefMs(fs, releaseMs);
        chg = coefMs(fs, chargeMs); slowRel = coefMs(fs, slowMs); dual = dualRate;
    }
    void clear() noexcept { peak = slow = out = 0; }
    [[nodiscard]] float tick(float target) noexcept {
        peak = target > peak ? target : target + rel * (peak - target);
        float v = peak;
        if (dual) {
            slow = target + (target > slow ? chg : slowRel) * (slow - target);
            v = std::max(v, slow);
        } else {
            slow = 0;
        }
        out = v > out ? v + att * (out - v) : v;
        return out;
    }
};

// ---- peak meter: peak in dBFS with a falling release, published lock-free ---------------------
struct PeakMeter {
    float held = -120.0f, fallPerSample = 0.0f;
    std::atomic<float> out{-120.0f};
    void prepare(double fs, float releaseDbPerSec) noexcept { fallPerSample = releaseDbPerSec / static_cast<float>(fs); held = -120.0f; out.store(-120.0f); }
    void feedPeak(float peakLin, std::uint32_t frames) noexcept {
        held = std::max(held - fallPerSample * static_cast<float>(frames), linToDb(peakLin));
        held = std::max(held, -120.0f);
        out.store(held, std::memory_order_relaxed);
    }
    [[nodiscard]] float db() const noexcept { return out.load(std::memory_order_relaxed); }
};

[[nodiscard]] inline float peakOf(const float* x, std::uint32_t n) noexcept {
    float p = 0.0f;
    for (std::uint32_t i = 0; i < n; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// Number of LEDs lit for a level (dB) given LED marks sorted high -> low.
template <std::size_t N>
[[nodiscard]] int ledsLit(float levelDb, const std::array<float, N>& marksHighToLow) noexcept {
    int lit = 0;
    for (float m : marksHighToLow) if (levelDb >= m) ++lit;
    return lit;
}

} // namespace hc::console

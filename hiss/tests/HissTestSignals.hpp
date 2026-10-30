#pragma once
// =============================================================================
//  Known-answer test signals for the Hiss Tamer. Everything is generated from a
//  fixed seed, so every run (and the Python reference) sees exactly the same audio.
//  The level of every artifact we add is known exactly, so the right answer is known.
// =============================================================================
#include "hybrid/hiss/HissFft.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <vector>

namespace hc::hiss::test {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { for (int i = 0; i < 4; ++i) next(); }
    std::uint64_t next() noexcept {   // xorshift64*
        s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    double uniform() noexcept { return (static_cast<double>(next() >> 11) + 0.5) / 9007199254740992.0; }
    double gauss() noexcept {
        const double u1 = uniform(), u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * std::numbers::pi * u2);
    }
};

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double v) { return 20.0 * std::log10(std::max(v, 1e-30)); }

inline double rmsOf(const std::vector<float>& x) {
    double s = 0;
    for (float v : x) s += static_cast<double>(v) * v;
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, x.size())));
}

// Gaussian noise shaped by `mask(frequencyHz)` (0..1), scaled to an exact RMS in dBFS.
inline std::vector<float> shapedNoise(std::size_t n, double fs, const std::function<double(double)>& mask, double rmsDb, std::uint64_t seed) {
    std::size_t N2 = 1;
    while (N2 < n) N2 <<= 1;
    FftD fft;
    fft.prepare(static_cast<int>(N2));
    std::vector<std::complex<double>> v(N2);
    Rng rng(seed);
    for (auto& c : v) c = {rng.gauss(), 0.0};
    fft.forward(v.data());
    for (std::size_t k = 0; k <= N2 / 2; ++k) {
        const double f = static_cast<double>(k) * fs / static_cast<double>(N2);
        const double m = mask(f);
        v[k] *= m;
        if (k > 0 && k < N2 / 2) v[N2 - k] = std::conj(v[k]);
    }
    fft.inverse(v.data());
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(v[i].real());
    const double r = rmsOf(out);
    const double g = r > 0 ? dbToLin(rmsDb) / r : 0.0;
    for (auto& s : out) s = static_cast<float>(static_cast<double>(s) * g);
    return out;
}

// Raised-cosine band: 1 between lo and hi, soft edges of `edge` Hz outside.
inline std::function<double(double)> bandMask(double lo, double hi, double edge) {
    return [=](double f) {
        const double d = f < lo ? lo - f : (f > hi ? f - hi : 0.0);
        return d >= edge ? 0.0 : 0.5 * (1.0 + std::cos(std::numbers::pi * d / edge));
    };
}

inline std::vector<float> bandNoise(std::size_t n, double fs, double lo, double hi, double rmsDb, std::uint64_t seed, double edge = 250.0) {
    return shapedNoise(n, fs, bandMask(lo, hi, edge), rmsDb, seed);
}

inline std::vector<float> sine(std::size_t n, double fs, double hz, double rmsDb, double phase = 0.0) {
    std::vector<float> out(n);
    const double a = dbToLin(rmsDb) * std::numbers::sqrt2;
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<float>(a * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / fs + phase));
    return out;
}

// Noise whose loudness swings sinusoidally (a flutter): depth 0..1, rate in Hz. RMS stays at rmsDb.
inline void applyFlutter(std::vector<float>& x, double fs, double rateHz, double depth) {
    double e = 0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double m = 1.0 + depth * std::sin(2.0 * std::numbers::pi * rateHz * static_cast<double>(i) / fs);
        e += m * m;
    }
    const double norm = 1.0 / std::sqrt(e / static_cast<double>(std::max<std::size_t>(1, x.size())));
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double m = 1.0 + depth * std::sin(2.0 * std::numbers::pi * rateHz * static_cast<double>(i) / fs);
        x[i] = static_cast<float>(static_cast<double>(x[i]) * m * norm);
    }
}

enum class HitKind { Crash, Hat };

// A cymbal-like hit: metallic noise above 3 kHz plus a few inharmonic partials, fast attack, exponential decay.
inline std::vector<float> cymbalHit(double fs, HitKind kind, double peakDb, std::uint64_t seed) {
    const double len = kind == HitKind::Crash ? 1.6 : 0.30;
    const double tau = kind == HitKind::Crash ? 0.35 : 0.045;
    const auto n = static_cast<std::size_t>(len * fs);
    auto noise = shapedNoise(n, fs, [](double f) { return f < 3000.0 ? 0.0 : (f < 4000.0 ? (f - 3000.0) / 1000.0 : 1.0); }, -20.0, seed);
    static const double partials[] = {3310.0, 5120.0, 7740.0, 9210.0, 11330.0, 13890.0};
    std::vector<float> out(n);
    const double attack = 0.001 * fs;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        double tonal = 0;
        for (double p : partials) tonal += std::sin(2.0 * std::numbers::pi * p * t + p) * 0.05;
        const double env = (static_cast<double>(i) < attack ? static_cast<double>(i) / attack : 1.0) * std::exp(-t / tau);
        out[i] = static_cast<float>((static_cast<double>(noise[i]) + tonal) * env);
    }
    double pk = 0;
    for (float v : out) pk = std::max(pk, static_cast<double>(std::fabs(v)));
    const double g = pk > 0 ? dbToLin(peakDb) / pk : 0.0;
    for (auto& v : out) v = static_cast<float>(static_cast<double>(v) * g);
    return out;
}

inline void addAt(std::vector<float>& dst, const std::vector<float>& src, std::size_t at, double gain = 1.0) {
    for (std::size_t i = 0; i < src.size() && at + i < dst.size(); ++i) dst[at + i] += static_cast<float>(static_cast<double>(src[i]) * gain);
}

enum class Hiss { None, Steady, Tone, Flutter };

struct SceneSpec {
    double fs = 48000.0;
    double seconds = 20.0;
    Hiss   hiss = Hiss::Steady;
    double hissDb = -66.0;          // RMS of the artifact (per channel) in dBFS
    double hissLo = 11250.0, hissHi = 12250.0;   // steady / flutter band
    double toneHz = 11800.0;
    double flutterHz = 8.0, flutterDepth = 0.5;
    double floorDb = -100.0;        // a faint white floor under everything (like a real 24-bit file)
    bool   bed = true;              // low-frequency music-like content, far below the focus
    bool   hits = true;             // sparse cymbal hits
    bool   denseHats = false;       // a steady 8-per-second hat pattern (a hard case)
    double hitPeakDb = -18.0;
    std::uint64_t seed = 1;
};

struct Scene {
    std::vector<float> L, R;
    std::vector<std::size_t> hitStarts;   // sample index of every crash / hat hit (for the transient checks)
    std::vector<float> hissL, hissR;      // the artifact alone (exactly what was added)
    double fs = 48000.0;
};

inline Scene makeScene(const SceneSpec& sp) {
    Scene sc;
    sc.fs = sp.fs;
    const auto n = static_cast<std::size_t>(sp.seconds * sp.fs);
    sc.L.assign(n, 0.0f);
    sc.R.assign(n, 0.0f);
    sc.hissL.assign(n, 0.0f);
    sc.hissR.assign(n, 0.0f);
    // faint floor
    {
        auto a = shapedNoise(n, sp.fs, [](double) { return 1.0; }, sp.floorDb, sp.seed * 11 + 1);
        auto b = shapedNoise(n, sp.fs, [](double) { return 1.0; }, sp.floorDb, sp.seed * 11 + 2);
        addAt(sc.L, a, 0);
        addAt(sc.R, b, 0);
    }
    if (sp.bed) {
        auto a = shapedNoise(n, sp.fs, [](double f) { return f < 2500.0 ? 1.0 : (f < 3000.0 ? (3000.0 - f) / 500.0 : 0.0); }, -28.0, sp.seed * 11 + 3);
        auto b = shapedNoise(n, sp.fs, [](double f) { return f < 2500.0 ? 1.0 : (f < 3000.0 ? (3000.0 - f) / 500.0 : 0.0); }, -28.0, sp.seed * 11 + 4);
        auto t1 = sine(n, sp.fs, 110.0, -24.0), t2 = sine(n, sp.fs, 220.5, -30.0, 1.0);
        addAt(sc.L, a, 0); addAt(sc.R, b, 0);
        addAt(sc.L, t1, 0); addAt(sc.R, t1, 0);
        addAt(sc.L, t2, 0); addAt(sc.R, t2, 0);
    }
    if (sp.hits) {
        // a crash every ~3.7 s and a hat in between; levels and spacing fixed so every run matches
        std::size_t at = static_cast<std::size_t>(1.4 * sp.fs);
        int i = 0;
        while (at + static_cast<std::size_t>(0.4 * sp.fs) < n) {
            const bool crash = (i % 2) == 0;
            auto hit = cymbalHit(sp.fs, crash ? HitKind::Crash : HitKind::Hat, sp.hitPeakDb - (crash ? 0.0 : 4.0), sp.seed * 101 + static_cast<std::uint64_t>(i));
            addAt(sc.L, hit, at);
            addAt(sc.R, hit, at, 0.9);
            sc.hitStarts.push_back(at);
            at += static_cast<std::size_t>((crash ? 1.9 : 1.8) * sp.fs);
            ++i;
        }
    }
    if (sp.denseHats) {
        const std::size_t step = static_cast<std::size_t>(sp.fs / 8.0);
        for (std::size_t at = static_cast<std::size_t>(0.5 * sp.fs), j = 0; at + step < n; at += step, ++j) {
            auto hit = cymbalHit(sp.fs, HitKind::Hat, sp.hitPeakDb - 8.0, sp.seed * 977 + j);
            addAt(sc.L, hit, at);
            addAt(sc.R, hit, at, 0.95);
            sc.hitStarts.push_back(at);
        }
    }
    switch (sp.hiss) {
    case Hiss::None: break;
    case Hiss::Steady:
        sc.hissL = bandNoise(n, sp.fs, sp.hissLo, sp.hissHi, sp.hissDb, sp.seed * 11 + 5);
        sc.hissR = bandNoise(n, sp.fs, sp.hissLo, sp.hissHi, sp.hissDb, sp.seed * 11 + 6);
        break;
    case Hiss::Tone:
        sc.hissL = sine(n, sp.fs, sp.toneHz, sp.hissDb, 0.3);
        sc.hissR = sine(n, sp.fs, sp.toneHz, sp.hissDb, 1.1);
        break;
    case Hiss::Flutter:
        sc.hissL = bandNoise(n, sp.fs, sp.hissLo, sp.hissHi, sp.hissDb, sp.seed * 11 + 5);
        sc.hissR = bandNoise(n, sp.fs, sp.hissLo, sp.hissHi, sp.hissDb, sp.seed * 11 + 6);
        applyFlutter(sc.hissL, sp.fs, sp.flutterHz, sp.flutterDepth);
        applyFlutter(sc.hissR, sp.fs, sp.flutterHz, sp.flutterDepth);
        break;
    }
    addAt(sc.L, sc.hissL, 0);
    addAt(sc.R, sc.hissR, 0);
    return sc;
}

} // namespace hc::hiss::test

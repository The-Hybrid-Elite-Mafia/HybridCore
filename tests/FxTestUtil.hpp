#pragma once
// Helpers for the HEM effect tests (tests/fx_*_tests.cpp).
#include "hybrid/fx/Effects.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <string>
#include <vector>

namespace hc::test::fxt {

struct Stereo {
    std::vector<float> l, r;
    explicit Stereo(std::size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
    [[nodiscard]] std::size_t size() const noexcept { return l.size(); }
};

inline Stereo sine(double hz, double seconds, float amp, double fs = 48000.0) {
    Stereo s(static_cast<std::size_t>(seconds * fs));
    for (std::size_t i = 0; i < s.size(); ++i) s.l[i] = s.r[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / fs));
    return s;
}
inline Stereo noise(double seconds, float amp, unsigned seed = 7, double fs = 48000.0) {
    Stereo s(static_cast<std::size_t>(seconds * fs));
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> d(-amp, amp);
    for (std::size_t i = 0; i < s.size(); ++i) { s.l[i] = d(g); s.r[i] = d(g); }
    return s;
}
// Runs the whole buffer through the effect in blocks of `block`.
inline void run(fx::Effect& e, Stereo& s, std::uint32_t block = 512) {
    for (std::size_t i = 0; i < s.size(); i += block) {
        const auto n = static_cast<std::uint32_t>(std::min<std::size_t>(block, s.size() - i));
        e.process(s.l.data() + i, s.r.data() + i, n);
    }
}
inline double rmsDb(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
    to = std::min(to, x.size());
    double a = 0;
    for (std::size_t i = from; i < to; ++i) a += static_cast<double>(x[i]) * x[i];
    return 10.0 * std::log10(std::max(a / static_cast<double>(std::max<std::size_t>(1, to - from)), 1e-20));
}
inline float peak(const std::vector<float>& x, std::size_t from = 0) {
    float p = 0;
    for (std::size_t i = from; i < x.size(); ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}
inline bool finite(const Stereo& s) {
    for (std::size_t i = 0; i < s.size(); ++i) if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
    return true;
}
// Index of an effect parameter by id (-1 if missing).
inline int param(const fx::Effect& e, const char* id) {
    const auto ps = e.params();
    for (std::size_t i = 0; i < ps.size(); ++i) if (std::string(ps[i].id) == id) return static_cast<int>(i);
    return -1;
}
// Level (dB) of a sine at hz after the effect, measured over the last half (settled).
inline double gainAtDb(fx::Effect& e, double hz, float amp = 0.1f) {
    e.reset();
    Stereo s = sine(hz, 1.0, amp);
    run(e, s);
    const std::size_t h = s.size() / 2;
    return rmsDb(s.l, h) - (20.0 * std::log10(amp / std::sqrt(2.0)));
}

} // namespace hc::test::fxt

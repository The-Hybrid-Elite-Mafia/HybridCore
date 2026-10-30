#pragma once
// What the Comb and Click Finder window does when SCAN is pressed. No drawing and no Windows in here, so it is tested anywhere.
#include "HissWav.hpp"
#include "hybrid/hiss/HissLab.hpp"
#include "hybrid/hiss/HissMeters.hpp"
#include "hybrid/hiss/HissTools.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace hem::combclick {

struct ScanResult {
    bool ok = false;
    std::string error;
    double periodMs = 40.0;                       // the period the knob had when SCAN was pressed
    double fs = 0;
    std::size_t frames = 0;
    hc::hiss::lab::SeamResult seam;
    hc::hiss::meters::CombResult comb;
    double secondsTaken = 0;
};

[[nodiscard]] inline std::vector<float> toMono(const std::vector<float>& l, const std::vector<float>& r) {
    std::vector<float> m(l.size());
    for (std::size_t i = 0; i < l.size(); ++i) m[i] = 0.5f * (l[i] + (i < r.size() ? r[i] : l[i]));
    return m;
}

// SCAN: runs the two real engines on the file. periodMs is the knob.
[[nodiscard]] inline ScanResult scan(const std::vector<float>& l, const std::vector<float>& r, double fs, double periodMs) {
    ScanResult s;
    const auto t0 = std::chrono::steady_clock::now();
    s.periodMs = periodMs; s.fs = fs; s.frames = l.size();
    if (l.empty() || fs < 8000.0) { s.error = "no audio loaded"; return s; }
    const auto x = toMono(l, r);
    s.comb = hc::hiss::meters::combLines(x.data(), x.size(), fs);
    s.seam = hc::hiss::lab::seamClicks(x.data(), x.size(), fs, periodMs);
    s.ok = true;
    s.secondsTaken = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return s;
}

// SAVE REPORT: the same plain-words report the command-line tool writes, for the same period.
[[nodiscard]] inline std::string reportText(const std::vector<float>& l, const std::vector<float>& r, double fs, double periodMs, const std::string& name) {
    return hc::hiss::tools::combAndClickFinder(l.data(), r.data(), l.size(), fs, name, periodMs).text;
}

// A made-up clip for self-tests and the picture of the window made by the build machine. White noise at noiseRmsDb, plus (optionally)
// one single-sample click every clickPeriodMs at clickDb (clickPeriodMs <= 0: none). Always labelled MADE-UP wherever it is shown.
[[nodiscard]] inline std::vector<float> madeUpClip(double fs, double seconds, double clickPeriodMs, double clickDb, double noiseRmsDb, unsigned seed) {
    const std::size_t n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> x(n);
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, std::pow(10.0, noiseRmsDb / 20.0));
    for (auto& v : x) v = static_cast<float>(nd(rng));
    if (clickPeriodMs > 0.0) {
        const double step = clickPeriodMs * 0.001 * fs, amp = std::pow(10.0, clickDb / 20.0);
        for (double p = 0.37 * step; p < static_cast<double>(n); p += step) x[static_cast<std::size_t>(p)] += static_cast<float>(amp);
    }
    return x;
}

} // namespace hem::combclick

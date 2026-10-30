#pragma once
// =============================================================================
//  HEM Hiss Meters (hem_hiss_meters): the Phase 1C measuring tools, written in pure C++23.
//  No JUCE. Needs only HissFft.hpp. OFFLINE ONLY: these allocate and take their time.
//  Never call them on the audio thread. Each works on one channel of float samples.
//
//  What is TESTED, ASSUMED and UNKNOWN about each meter is written in
//  hiss/phase1c/PHASE1-REPORT.md and hiss/phase1c/FAILURES.md.
//
//  Python twins (same maths, separately written): hiss/python/phase1c/h1c.py and check*.py
// =============================================================================
#include "hybrid/hiss/HissConfig.hpp"

#include <cstddef>
#include <vector>

namespace hc::hiss::meters {

// ---- Frame power (STFT) -------------------------------------------------------------------
// power(frame, bin) = |X|^2 / (sum of window)^2. A sine of amplitude 1 adds 0.5 in total (its mean square).
struct Stft {
    double fs = 0.0;
    int nfft = 0, hop = 0, frames = 0, bins = 0;
    std::vector<double> p;   // frames x bins
    [[nodiscard]] double binHz() const noexcept { return fs / static_cast<double>(nfft); }
    [[nodiscard]] double band(int frame, double loHz, double hiHz) const noexcept;   // sum of bins with lo <= f < hi
};
[[nodiscard]] Stft stftPower(const float* x, std::size_t n, double fs, int nfft = cfg::kMeterFft, int hop = cfg::kMeterHop);

// "Loud part": frames whose power sits above P70 of the non-silent frames, until they fall below P60 (hysteresis).
[[nodiscard]] std::vector<char> loudMask(const std::vector<double>& framePower, double silenceDb = cfg::kSilenceDb, double enterPct = cfg::kLoudEnterPct, double leavePct = cfg::kLoudLeavePct);

struct RatioResult {
    bool valid = false;        // false: fewer than 20 loud frames, nothing is reported
    int frames = 0, loud = 0;
    double a5to10Db = 0, b16to20Db = 0, c8to20Db = 0;   // absolute band power over loud frames (dB re a 1.0-amplitude sine's mean square)
    double ratio16_20Over5_10Db = 0;                     // B - A: can rise because A fell. Always read it next to the absolute levels.
    double ratio16_19Over5_10Db = 0;                     // bandwidth-matched to 19 kHz
};
// mask: optional fixed loud-frame mask (same frame count). Use it for before/after tests (FAILURES.md F3).
[[nodiscard]] RatioResult ratioMetrics(const float* x, std::size_t n, double fs, const std::vector<char>* mask = nullptr);

// ---- Where the sound ends (bandwidth audit) --------------------------------------------------
enum class EndShape { None, Gentle, Wall };
struct CutoffResult {
    bool valid = false;
    EndShape shape = EndShape::None;
    double cutoffHz = 0;       // start of the fall (Wall), first 30 dB under the 5-10 kHz level (Gentle), Nyquist (None)
    double ref5to10Db = 0;
    double dropDbIn500Hz = 0;
    [[nodiscard]] bool reaches19k() const noexcept { return valid && cutoffHz >= cfg::kBandMatchedTopHz; }   // SPLIT.json rule
};
[[nodiscard]] CutoffResult cutoffAudit(const float* x, std::size_t n, double fs);

// ---- The 25 Hz comb ---------------------------------------------------------------------------
// Folds the signal over two comb periods. Level of each line = (on-line power) - (power halfway between lines).
// Music sitting on the grid shows up in both and cancels. Needs fs divisible by 25 (48000, 44100 work).
struct CombResult {
    bool valid = false;
    int lines = 0, periods = 0;
    double bandwidthHz = 0;          // measurement bandwidth = 1 / (listening time)
    double medianLineDb = -300;      // median over lines 3-20 kHz of the comb line level (dBFS rms per line)
    double medianOnDb = -300, medianOffDb = -300;
    double onGreaterThanOffPct = 0;  // about 45 for no comb; clearly higher with a comb
    std::vector<double> lineDb;      // per line, 3.000 kHz upward in 25 Hz steps
};
[[nodiscard]] CombResult combLines(const float* x, std::size_t n, double fs, double loHz = cfg::kCombLoHz, double hiHz = cfg::kCombHiHz);

// ---- Flutter (25 Hz loudness wobble of the 5-15 kHz band) -------------------------------------------
// DETECT-ONLY. Pre-registered test (check 4b) FAILED the +/-20 % accuracy rule: readings run about 27 % low.
// Use: reading above flutterPresentThreshold means "flutter is shown". Below it means "not shown", NOT "none".
struct FlutterResult {
    bool valid = false;      // false: clip shorter than 19.5 s, or the band is empty (< -90 dBFS)
    double depth = 0;        // wobble amplitude / mean envelope (a true 0.30 AM reads about 0.22)
};
inline constexpr double kFlutterPresentThreshold = cfg::kFlutterPresentThreshold;   // see HissConfig.hpp
[[nodiscard]] FlutterResult flutterDepth(const float* x, std::size_t n, double fs, double modHz = cfg::kCombFundamentalHz);

// ---- Levels and loudness ---------------------------------------------------------------------------
[[nodiscard]] double rmsDbfs(const float* x, std::size_t n);
[[nodiscard]] double truePeakDbfs(const float* x, std::size_t n);                 // 4x oversampled
[[nodiscard]] double integratedLufs(const float* x, std::size_t n, double fs);    // BS.1770-4 K-weighting, gates -70 abs / -10 LU rel (one channel)
[[nodiscard]] double bandRmsDbfs(const float* x, std::size_t n, double fs, double loHz, double hiHz);   // rms of the content between lo and hi

} // namespace hc::hiss::meters

#pragma once
// =============================================================================
//  HEM Hiss Lab (hem_hiss_lab): the "find out what is wrong" engines. Pure C++23, no JUCE.
//  OFFLINE ONLY: these allocate. Never call them on the audio thread.
//  Every function reads audio (const float*) and never changes it.
//
//  Engines in this file (numbers are the engine numbers in Steven's list):
//    8  seamClicks          finds clicks and checks whether they repeat every 40 ms
//    9  trackNotch          finds a narrow dip in the high band and follows it
//   11  hitsAndTails        finds hits (cymbals, hats) and measures how long the tail rings
//   12  maskingMargin       ASSUMED model: would a quiet artifact be covered by louder sound nearby?
//   13  averageHits         lines all hits up and averages them (shows the tail shape)
//   14  echoCheck           looks for a repeat of the hits some milliseconds later
//   15  highBandWobble      reads slow loudness wobble (0.5-60 Hz) of the 10-16 kHz band
//   16  phaseConsistency    left/right phase and agreement per band
//   17  maker::*            puts a problem into audio at a KNOWN level (the answer is known)
//   19  sha256, blindName   fingerprint of a file, and a name that carries no information
//
//  What is TESTED / ASSUMED / UNKNOWN is in hiss/tests/lab_tests.cpp output and hiss/phase1c/LAB-REPORT.md.
//  Every working number lives in HissConfig.hpp (section LAB), tagged ASSUMED.
// =============================================================================
#include "hybrid/hiss/HissConfig.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::hiss::lab {

// ---- statistics used by every decision (NO guessed thresholds: the only chosen number is cfg::kAlpha) -----------
namespace stats {
// z such that the chance of a normal value above z is p (one-sided). Computed, not looked up.
[[nodiscard]] double zUpper(double p) noexcept;
// 1.4826 x median absolute deviation (falls back to the standard deviation when the MAD is 0).
[[nodiscard]] double robustSigma(const std::vector<double>& v);
[[nodiscard]] double median(std::vector<double> v);
// chance of k or more successes out of n when each has chance p
[[nodiscard]] double binomTail(int k, int n, double p) noexcept;
// level a clean file beats only with chance alpha over neff looks, from the top 10 % tail of a sample that holds no signal
[[nodiscard]] double tailBar(const std::vector<double>& nullSample, double neff, double alpha);
} // namespace stats

// ---- 8: seam clicks ---------------------------------------------------------------------------------
struct Click { double timeMs = 0; double levelDb = 0; };           // levelDb: size of the click against the local typical size
struct SeamResult {
    bool valid = false;                   // false: clip too short (< 0.5 s)
    std::vector<Click> clicks;
    double medianIntervalMs = 0;          // 0 when fewer than 2 clicks
    double periodicShare = 0;             // share of gaps equal to 40 ms (or a whole multiple), within the tolerance
    bool pattern = false;                 // the 40 ms fold beats every other fold period at the chosen false-alarm rate (foldP <= kAlpha)
    // The fold test: add up the click-band energy at every position inside a 40 ms frame, over the whole clip.
    // A repeating click makes one position stand out. The same sum at other (not 40 ms) frame lengths is the null.
    double foldP = 1.0;                   // chance a clean file gives a fold this strong (rank among the null folds)
    int nullFolds = 0;                    // how many other frame lengths were used as the null
    double foldPhaseMs = 0;               // where inside the 40 ms frame the click sits
    double foldLevelDb = -200;            // click size read from the fold (single-sample peak, dBFS). Only meaningful when pattern is true.
    bool foldSigned = false;              // true: the matched (same-direction) fold gave the stronger answer
    double detectLimitDb = -200;          // smallest single-sample click, all pointing the same way, this file would show as a pattern (about half the time), from the file's own noise
    double detectLimitAnySignDb = -200;   // same, for clicks that point either way (energy fold)
    double clickThresholdRatio = 0;       // single-click rule used in this clip: how many times the local typical size (computed from the clip length)
};
[[nodiscard]] SeamResult seamClicks(const float* x, std::size_t n, double fs, double periodMs = cfg::kClickPeriodMs);

// ---- 9: moving notch ----------------------------------------------------------------------------------
struct NotchPoint { double timeSec = 0, freqHz = 0, depthDb = 0; bool significant = false; };
struct NotchResult {
    bool valid = false;                   // false: shorter than (kNotchAvgFrames + 1) frames
    std::vector<NotchPoint> track;        // one point per averaged group of frames
    double medianDepthDb = 0, medianFreqHz = 0;
    double driftHzPerSec = 0;             // straight-line fit through the track (0 if depth is too small to call)
    int groups = 0, significantGroups = 0;   // a group is significant when its deepest dip is a rare outlier among all dips in the band
    double depthBarDb = 0;                // dip depth (above the normal spread) a reading must beat in this clip
    double binomP = 1.0;                  // chance a clean file gives this many significant groups
    bool found = false;                   // binomP <= kAlpha
};
[[nodiscard]] NotchResult trackNotch(const float* x, std::size_t n, double fs);

// ---- 11 + 13: hits and tails -------------------------------------------------------------------------
struct Hit { double timeMs = 0; double peakDb = 0; double tailMs = -1; };   // tailMs < 0: not countable (cut off or too long)
struct HitResult {
    bool valid = false;
    std::vector<Hit> hits;
    int counted = 0;                      // hits with a tail time
    double medianTailMs = 0;              // median over counted hits
    double floorDb = 0;                   // quiet floor of the band (10th percentile)
    double riseBarDb = 0;                 // the rise (dB in 10 ms) a hit must beat in this clip: median + z x robust spread, z from kAlpha
};
[[nodiscard]] HitResult hitsAndTails(const float* x, std::size_t n, double fs, double bandLoHz = cfg::kHitBandLoHz, double bandHiHz = cfg::kHitBandHiHz);

struct AverageResult {
    bool valid = false;                   // false: fewer than 3 countable hits
    int hits = 0;
    double stepMs = 0, startMs = 0;       // profile[i] is at startMs + i*stepMs, in dB against the peak (0 at the hit)
    std::vector<double> profileDb;
    double tail20Ms = -1, tail30Ms = -1;  // time for the AVERAGE to fall 20 / 30 dB (-1: never inside the window)
};
[[nodiscard]] AverageResult averageHits(const float* x, std::size_t n, double fs, double bandLoHz = cfg::kHitBandLoHz, double bandHiHz = cfg::kHitBandHiHz);

// ---- 14: echo / repeat ----------------------------------------------------------------------------------
struct EchoResult {
    bool valid = false;
    double lagMs = 0;                     // where the strongest repeat sits
    double strength = 0;                  // about the loudness of the repeat against the original (0 to 1), see tests
    double contrast = 0;                  // peak against typical size at other lags (information only)
    int segments = 0;                     // how many separate parts of the clip were tested for the same lag
    int agreeing = 0;                     // how many of them put their strongest repeat at the same lag (within 1 step)
    double chanceAllAgree = 1.0;          // chance that many parts agree by luck
    double peakBar = 0;                   // height the strongest repeat must beat in this clip (from its own dips, 1 in 100)
    bool found = false;                   // all parts agree AND chanceAllAgree <= kAlpha AND the repeat beats peakBar
};
[[nodiscard]] EchoResult echoCheck(const float* x, std::size_t n, double fs, double bandLoHz = cfg::kHitBandLoHz, double bandHiHz = cfg::kHitBandHiHz);

// ---- 15: high band wobble -------------------------------------------------------------------------------
struct WobbleLine {
    double freqHz = 0;
    double depth = 0;                     // 0.2 = loudness swings +/-20 % at that rate; leakage-corrected and self-calibrated (see the .cpp)
    double depthLo = 0, depthHi = 0;      // range the depth sits in at the chosen false-alarm rate (from the clip's own leakage)
    double contrastDb = 0;                // line against the typical modulation level (information only)
    double p = 1.0;                       // chance a clean clip gives a line this strong (rank among ~180 nearby rates)
    bool significant = false;             // p <= kAlpha
    bool rhythmLike = false;              // the 2x or 3x rate is a significant line too: a beat or pattern, not a plain wobble
    bool depthTrusted = false;            // significant AND the range excludes 0 (derived, no guessed cut)
};
struct WobbleResult {
    bool valid = false;                   // false: clip shorter than kWobbleMinSeconds, band empty, or the calibration did not move
    std::vector<WobbleLine> lines;        // up to 6 lines, strongest first, each at least 1.5 Hz apart
    double freqHz = 0, depth = 0, contrastDb = 0;   // the best significant line that is NOT rhythm-like (else the strongest line)
    bool found = false;                   // some line is significant
    bool foundSteady = false;             // some significant line is not rhythm-like
    double detectLimitDepth = 0;          // smallest plain wobble depth THIS clip could show at a typical rate (its own wander/beat + the chance level for the number of rates tried)
    double detectLimitWorstDepth = 0, detectLimitWorstHz = 0;   // the same at the rate where this clip hides a wobble best
};
[[nodiscard]] WobbleResult highBandWobble(const float* x, std::size_t n, double fs, double bandLoHz = cfg::kWobbleBandLoHz, double bandHiHz = cfg::kWobbleBandHiHz);

// ---- 16: left/right phase (a band is "significant" when its agreement beats all time-shifted pairings of L with R) --------------------------------------------------------------------------------
struct PhaseBand { double loHz = 0, hiHz = 0, coherence = 0, phaseDeg = 0, nullMedian = 0; bool significant = false; };   // significant: agreement beats every one of the time-shifted (unrelated) pairings   // coherence 1 = same signal; phaseDeg = L ahead of R (+)
struct PhaseResult { bool valid = false; std::vector<PhaseBand> bands; double meanCoherence = 0; };
[[nodiscard]] PhaseResult phaseConsistency(const float* left, const float* right, std::size_t n, double fs);

// ---- 12: masking (ASSUMED model) ---------------------------------------------------------------------------
// artifactDb / maskerDb are band levels in dB. distanceBark is how far apart the two bands sit.
// Returns marginDb = artifact level minus the covered level. Negative = covered (probably not heard). ASSUMED, not a hearing test.
[[nodiscard]] double maskingMarginDb(double artifactDb, double maskerDb, double distanceBark) noexcept;
[[nodiscard]] double hzToBark(double hz) noexcept;
// Share of loud frames in which an artifact band is covered by the rest of the spectrum (ASSUMED model).
struct MaskResult { bool valid = false; double coveredShare = 0; double medianMarginDb = 0; int frames = 0; };
[[nodiscard]] MaskResult maskingShare(const float* x, std::size_t n, double fs, double artifactLoHz, double artifactHiHz, double artifactDb);

// ---- 19: fingerprint and blind name ----------------------------------------------------------------------------
[[nodiscard]] std::string sha256Hex(const void* data, std::size_t bytes);
[[nodiscard]] std::string blindName(const std::string& contentHashHex);     // "HEM-" + first 8 hex digits: carries nothing about the source

// ---- 17: signal maker (puts a problem in at a known level) --------------------------------------------------------
namespace maker {
struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { for (int i = 0; i < 4; ++i) next(); }
    std::uint64_t next() noexcept { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    double uniform() noexcept { return (static_cast<double>(next() >> 11) + 0.5) / 9007199254740992.0; }
    double gauss() noexcept;
};
// White noise at an exact RMS (dBFS).
[[nodiscard]] std::vector<float> whiteNoise(std::size_t n, double rmsDb, std::uint64_t seed);
// Band-limited copy of x (8th-order, zero phase). Used by the injections and by the tests.
[[nodiscard]] std::vector<float> bandpass(const float* x, std::size_t n, double fs, double loHz, double hiHz);
// out = x + depth * sin(2 pi f t) * band(x): the loudness of [loHz, hiHz] swings by +/-depth at modHz. Truth: depth.
void injectBandWobble(std::vector<float>& x, double fs, double modHz, double depth, double loHz, double hiHz);
// Adds a 25 Hz comb: lines every fs/P Hz (P = fs/25) from loHz to hiHz, each line at lineDb (dBFS rms per line, random phase).
void injectComb(std::vector<float>& x, double fs, double lineDb, double loHz, double hiHz, std::uint64_t seed);
// Adds one single-sample click every periodMs, peak = peakDb (dBFS). Returns the sample index of every click (the answer).
std::vector<std::size_t> injectSeamClicks(std::vector<float>& x, double fs, double periodMs, double peakDb, double startMs);
// Adds a copy of x delayed by delayMs at gainDb. Truth: delayMs, gainDb.
void injectEcho(std::vector<float>& x, double fs, double delayMs, double gainDb);
// A dip (gaussian in dB-free amplitude) that slides from f0 to f1 over the clip. depthDb at the centre, sigmaHz wide. Truth: f0->f1, depthDb.
void injectNotchSweep(std::vector<float>& x, double fs, double f0, double f1, double depthDb, double sigmaHz);
// A hit train: bursts of band noise with exponential decay. tauMs is the amplitude time constant (power falls 20 dB in 2.3026 x tauMs).
struct HitTrain { std::vector<float> audio; std::vector<double> hitMs; };
[[nodiscard]] HitTrain hitTrain(std::size_t n, double fs, double meanSpacingMs, double tauMs, double peakDb, double loHz, double hiHz, std::uint64_t seed);
} // namespace maker

} // namespace hc::hiss::lab

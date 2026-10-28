#pragma once
// =============================================================================
//  HEM HISS AUTO (hem_hiss_auto) — the self-running brain.
//
//  The app calls ONE function: autoFix(). Give it a stereo file (or call autoFixStems()
//  with all 13 stems). It will:
//    1. SCAN     read the whole file
//    2. DIAGNOSE find where the shimmer/hiss is, and whether it is steady noise, a tone
//                or a flutter, and whether it lives in mid, side or both
//    3. DECIDE   choose the focus, the cut, and the method (it tries the candidates and
//                keeps the one with the best measured result)
//    4. FIX      run the engine (transient guard on, smooth gains)
//    5. CHECK    compare before and after: artifact dropped? hits intact? nothing outside
//                the focus changed? no watery sound?
//    6. DO NO HARM  a failed check backs the cut off; if it still fails, or nothing clear
//                was found, or it is not sure: the file comes back untouched, bit for bit.
//
//  Nothing here claims perfect separation. Every number in the report was measured on the
//  audio that was actually processed. Offline only: allocates and takes seconds per minute of audio.
// =============================================================================
#include "hybrid/hiss/HissConfig.hpp"
#include "hybrid/hiss/HissTamerEngine.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace hc::hiss {

enum class ArtifactKind { None, Steady, Tonal, Flutter };
enum class Confidence   { Untouched, Low, Medium, High };

[[nodiscard]] const char* kindName(ArtifactKind k) noexcept;         // "steady noise", "a tone", "a flutter", "nothing clear"
[[nodiscard]] const char* confidenceName(Confidence c) noexcept;     // "Untouched", "Low", "Medium", "High"

struct Diagnosis {
    bool         scanned = false;       // false = file too short or silent to scan
    ArtifactKind kind = ArtifactKind::None;
    double centreHz = 0.0, widthHz = 0.0;   // where it sits (width = -3 dB extent)
    double bumpDb = 0.0;                    // how far above "normal" the always-there floor is (mid + side together)
    double bumpMidDb = 0.0, bumpSideDb = 0.0;
    double steadiness = 0.0;                // quiet-moment level / typical level of the band; 1 = perfectly steady
    double pulseHz = 0.0, pulseDepthDb = 0.0, pulseProminenceDb = 0.0, pulseSkew = 0.0;
    Mode   mode = Mode::Stereo;
    std::string notes;
};

// Everything the self-check measures. Taken on the audio that was really processed.
struct CheckResult {
    bool   measured = false;
    double dropDb = 0.0;            // how far the floor of the focus band fell (the always-there level, 10th percentile)
    int    hitsChecked = 0;
    double peakLossDb = 0.0;        // worst peak change of a hit (<= 0 = lost level)
    double envelopeErrDb = 0.0;
    double attackShiftMs = 0.0;
    double outOfBandDb = 0.0;       // biggest change anywhere outside the focus
    double musicalNoise = 1.0;      // 1.0 = same texture as the input
    double levelDiffDb = 0.0;       // full-band RMS, after - before
    bool   passed = false;
    std::string failReason;
};

struct CandidateResult {
    Params      params;
    CheckResult check;
    double      score = 0.0;
    bool        feasible = false;
};

struct AutoResult {
    std::string name;                       // stem name, if given
    bool        touched = false;
    Diagnosis   diagnosis;
    Params      chosen;                     // what was used (valid when touched)
    double      targetReductionDb = 0.0;
    Confidence  confidence = Confidence::Untouched;
    std::vector<CandidateResult> candidates;
    int         backOffSteps = 0;
    CheckResult finalCheck;                 // measured on the whole file
    std::vector<float> left, right;         // fixed audio: same length, same alignment; identical to the input when untouched
    std::vector<float> removedLeft, removedRight;   // input - output
    double      levelMatchGainDb = 0.0;     // gain that would make the fixed file as loud as the original (for A/B)
    std::string report;                     // plain words
};

struct AutoOptions {
    bool   tryFloor = true, tryPercentile = true, tryTonal = true;   // which methods may be tried (tests turn some off)
    double chunkSeconds = 10.0;              // length of each trial excerpt
    int    chunkCount = 3;                   // how many excerpts a long file is judged on (files up to 3 x chunk are used whole)
    double primeSeconds = 3.0;               // how much of the start is used to teach the detector before the first sample
    std::function<void(const std::string&)> progress;   // optional, called with short status lines
};

[[nodiscard]] Diagnosis diagnose(const float* left, const float* right, std::size_t frames, double sampleRate);

[[nodiscard]] AutoResult autoFix(const float* left, const float* right, std::size_t frames, double sampleRate,
                                 const AutoOptions& options = {}, const std::string& name = {});

struct StemRef { std::string name; const std::vector<float>* left; const std::vector<float>* right; };
// Every stem on its own. Only stems that really have the problem are touched; the rest come back bit-exact.
[[nodiscard]] std::vector<AutoResult> autoFixStems(const std::vector<StemRef>& stems, double sampleRate, const AutoOptions& options = {});

// One page, plain words, for a person who is not a programmer.
[[nodiscard]] std::string stemsReport(const std::vector<AutoResult>& results);

// Run the engine over a whole stereo file and return it aligned (latency removed, same length).
// `primeFrames` of the start teach the detector first (0 = none). Offline helper used by the brain and the tests.
void renderWithEngine(HissTamerEngine& engine, const float* left, const float* right, std::size_t frames,
                      std::size_t primeFrames, std::vector<float>& outLeft, std::vector<float>& outRight);

} // namespace hc::hiss

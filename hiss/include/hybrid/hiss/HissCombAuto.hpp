#pragma once
// =============================================================================
//  HEM HISS COMB AUTO (hem_hiss_comb_auto) — the self-running brain for the 25 Hz comb.
//
//  What it does, in order:
//    1. SCAN      look at the quiet floor of the file (mid and side) for many narrow lines
//                 that sit on a 25 Hz grid between 3 and 20 kHz.
//    2. DECIDE    enough lines, and most of them on the grid -> it is the comb. Otherwise the file
//                 is returned untouched, bit for bit.
//    3. FIX       learn the repeating pattern from the quietest moments of the file and subtract it
//                 (HissCombEngine, offline mode). Digital silence stays exactly silent.
//    4. CHECK     did the comb get shallower? was the low end left alone? is the removed part small,
//                 and made only of grid lines? A failed check halves the cut; still failing -> untouched.
//
//  Offline only: allocates and takes seconds per minute of audio.
//  Nothing here claims the comb is audible. It measures how much of the comb is gone.
// =============================================================================
#include "hybrid/hiss/HissAuto.hpp"
#include "hybrid/hiss/HissCombEngine.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace hc::hiss {

struct CombDiagnosis {
    bool   scanned = false;       // false = too short or silent
    bool   found = false;         // enough grid lines to call it the comb
    int    lines = 0;             // narrow lines found (louder of mid / side)
    int    onGrid = 0;            // of those, on the 25 Hz grid
    double chanceOnGrid = 0.0;    // how many would land on the grid by luck
    double gridShare = 0.0;       // onGrid / lines
    double depthDb = 0.0;         // comb strength: the height of all on-grid lines above their surroundings, added up (power), in dB
    bool   sideLouder = false;
    std::string notes;
};

[[nodiscard]] CombDiagnosis diagnoseComb(const float* left, const float* right, std::size_t frames, double sampleRate);

struct CombCheck {
    bool   measured = false;
    bool   passed = false;
    int    linesBefore = 0, linesAfter = 0;
    int    onGridBefore = 0, onGridAfter = 0;
    double depthBeforeDb = 0.0, depthAfterDb = 0.0;
    double removedRmsDb = -200.0;      // level of what was taken out
    double lowBandShare = 0.0;         // share of the removed energy below 2.5 kHz (should be ~0)
    double peakChangeDb = -200.0;      // biggest sample change, dBFS
    std::string failReason;
};

struct CombResult {
    std::string name;
    bool        touched = false;
    CombDiagnosis before;
    double      reductionDb = 0.0;
    int         backOffSteps = 0;
    Confidence  confidence = Confidence::Untouched;
    CombCheck   check;
    std::vector<float> left, right;                 // fixed audio; identical to the input when untouched
    std::vector<float> removedLeft, removedRight;   // input - output
    std::string report;
};

struct CombOptions {
    double reductionDb = 15.0;                       // first try
    bool   force = false;                            // TEST ONLY: skip the "is it really the comb?" decision and the improvement check, run the engine anyway
    std::function<void(const std::string&)> progress;
};

[[nodiscard]] CombResult autoFixComb(const float* left, const float* right, std::size_t frames, double sampleRate,
                                     const CombOptions& options = {}, const std::string& name = {});

} // namespace hc::hiss

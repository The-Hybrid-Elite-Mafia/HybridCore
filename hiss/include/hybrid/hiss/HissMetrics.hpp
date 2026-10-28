#pragma once
// =============================================================================
//  Measurements for the Hiss Tamer. Used by the automatic brain (to judge its own
//  work) and by the tests (to prove the engine does what it says).
//  Offline only: these allocate and take their time. Never call them on the audio thread.
//  Every function works on plain float arrays, one channel at a time.
// =============================================================================
#include <cstddef>
#include <vector>

namespace hc::hiss::metrics {

// ---- Long-FFT scan of a frequency band -------------------------------------------------
// power(frame, bin) = |X|^2 / sum(window^2), so white noise with variance s^2 reads s^2 in every bin.
struct BandScan {
    double fs = 0.0;
    int    nfft = 0, bin0 = 0, bins = 0, frames = 0;
    std::vector<float> power;        // frames x bins
    std::vector<float> frameRms;     // full-band RMS of each frame (linear), for silence gating
    std::vector<char>  used;         // 1 = frame is not digital silence
    [[nodiscard]] double binHz(int i) const noexcept { return static_cast<double>(bin0 + i) * fs / static_cast<double>(nfft); }
    [[nodiscard]] int    usedFrames() const noexcept;
};

[[nodiscard]] BandScan scanBand(const float* x, std::size_t n, double fs, int nfft, double loHz, double hiHz);

// q-quantile (nearest rank) of the power in every bin, over the frames that are not silent. Linear power.
[[nodiscard]] std::vector<double> quantileSpectrum(const BandScan& s, double q);

// q-quantile of an arbitrary list (nearest rank). The list is copied.
[[nodiscard]] double quantileOf(std::vector<double> v, double q);

// ---- Floor of a band ------------------------------------------------------------------------
struct FloorStat {
    double floorDb = -200.0;   // quantile q of the frame-by-frame band power: "what is always there"
    double meanDb  = -200.0;   // average band power
    int    frames  = 0;
};
[[nodiscard]] FloorStat bandFloor(const float* x, std::size_t n, double fs, double loHz, double hiHz, double q = 0.10, int nfft = 8192);

// ---- Cymbal / hit checks ---------------------------------------------------------------------
// Sample positions where the sound above 6 kHz suddenly jumps up (at most maxCount, strongest first, in time order).
[[nodiscard]] std::vector<std::size_t> findOnsets(const float* x, std::size_t n, double fs, int maxCount = 48);

struct HitStats {
    int    hits = 0;
    double worstPeakLossDb = 0.0;    // most any hit lost at its peak (<= 0 means lost; 0 = nothing lost or gained)
    double envRmsErrDb = 0.0;        // RMS gap of the loudness curve, only where the hit is 10 dB above what came before it
    double maxAttackShiftMs = 0.0;   // biggest shift of when 10 / 50 / 90 % of a hit's first 48 ms of energy has arrived
};
[[nodiscard]] HitStats compareHits(const float* before, const float* after, std::size_t n, double fs, const std::vector<std::size_t>& onsets);

// ---- Spectrum outside the focus ---------------------------------------------------------------
struct OutOfBand {
    double maxAbsDb = 0.0;   // biggest change of any bin outside the focus
    double rmsDb = 0.0;
    int    bins = 0;
};
// Compares long-average spectra. Bins quieter than minLevelDb (dB re full scale per bin) are ignored.
[[nodiscard]] OutOfBand outOfBand(const float* before, const float* after, std::size_t n, double fs,
                                  double centreHz, double halfSpanHz, double minLevelDb = -110.0);

// ---- "Watery" sound ---------------------------------------------------------------------------
// Kurtosis of the time-frequency magnitudes in the band (Uemura-style). Ratio after / before:
// 1.0 = the same texture as the input; clearly above 1 = isolated blips ("musical noise").
// quietShare = share of the quietest frames (by input band energy) that are judged; 0.5 = the quieter half.
[[nodiscard]] double musicalNoiseRatio(const float* before, const float* after, std::size_t n, double fs, double loHz, double hiHz, double quietShare = 0.5);

// ---- Levels -----------------------------------------------------------------------------------
[[nodiscard]] double rmsDb(const float* x, std::size_t n);
[[nodiscard]] double levelDifferenceDb(const float* before, const float* after, std::size_t n);   // after - before, full band RMS

// ---- Small statistics helpers -----------------------------------------------------------------
[[nodiscard]] double skewness(const std::vector<double>& v);
[[nodiscard]] double kurtosis(const std::vector<double>& v);   // not "excess": a Rayleigh or Gaussian-like set reads about 3

} // namespace hc::hiss::metrics

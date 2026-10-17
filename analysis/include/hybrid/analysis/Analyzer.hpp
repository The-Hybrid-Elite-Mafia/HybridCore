#pragma once
// =============================================================================
//  Analyzer — what the ANALYZER page shows, measured from the Main output (UI thread).
//
//   * Waterfall: kRows spectra of the newest audio (row 0 = newest), kPoints log-spaced
//     frequencies 20 Hz .. 20 kHz, in dB where a full-scale sine reads 0 dB.
//     4096-point Hann FFT every 1024 samples; a row is the louder of two FFTs (43 ms).
//   * Loudness (ITU-R BS.1770-4): momentary (400 ms), short-term (3 s), integrated since
//     reset (gated -70 LUFS absolute / -10 LU relative).
//   * Sample peak (now, held, highest since reset), samples at or over full scale.
//   * Stereo correlation, tone balance (bass / mids / highs), and plain-words checks.
//  Feed it with add() (any block size); allocation-free after construction except the
//  integrated-loudness history (one double per 100 ms).
// =============================================================================
#include "hybrid/analysis/Fft.hpp"
#include "hybrid/analysis/ScopeTap.hpp"

#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::analysis {

class Analyzer {
public:
    static constexpr int    kRows    = 64;
    static constexpr int    kPoints  = 112;
    static constexpr int    kFftSize = 4096;
    static constexpr int    kHop     = 1024;
    static constexpr float  kFloorDb = -96.0f;
    static constexpr double kSilentLufs = -144.0;

    explicit Analyzer(double sampleRate = 48000.0);

    // `songSeconds` = transport position when these samples were heard (for check timestamps).
    void add(const StereoFrame* frames, std::size_t n, double songSeconds = 0.0);
    // Forget everything (new file, play from the start).
    void reset();

    // ---- waterfall ----
    [[nodiscard]] const float* row(int age) const noexcept {   // age 0 = newest; kPoints values in dB
        return rows_.data() + static_cast<std::size_t>((newest_ - age + kRows) % kRows) * kPoints;
    }
    [[nodiscard]] static float pointHz(int i) noexcept;          // frequency of point i
    [[nodiscard]] std::uint64_t rowsMade() const noexcept { return rowsMade_; }

    // ---- loudness (LUFS; kSilentLufs when nothing to measure) ----
    [[nodiscard]] double momentaryLufs() const noexcept { return momentary_; }
    [[nodiscard]] double shortTermLufs() const noexcept { return shortTerm_; }
    [[nodiscard]] double integratedLufs() const;   // cached; recomputed when new audio arrived

    // ---- peaks (dBFS) ----
    [[nodiscard]] float peakDb() const noexcept { return toDb(peakNow_); }    // falling 20 dB/s
    [[nodiscard]] float peakHoldDb() const noexcept { return toDb(peakHold_); } // held 2 s
    [[nodiscard]] float maxPeakDb() const noexcept { return toDb(peakMax_); }   // since reset
    [[nodiscard]] std::uint64_t clippedSamples() const noexcept { return clipped_; }
    [[nodiscard]] double lastClipSeconds() const noexcept { return lastClipAt_; }

    // ---- stereo, balance ----
    [[nodiscard]] float correlation() const noexcept { return corrNow_; }     // -1 .. +1, last ~0.3 s
    [[nodiscard]] float correlationAvg() const noexcept { return corrAvg_; }  // ~3 s average
    // Level of each band in dB (bass 20-150 Hz, mids 150 Hz-5 kHz, highs 5-20 kHz): the last ~3 s
    // (live bars) and the average since reset (used by the checks).
    [[nodiscard]] float bassDb() const noexcept { return toDbPow(bandPow_[0]); }
    [[nodiscard]] float midsDb() const noexcept { return toDbPow(bandPow_[1]); }
    [[nodiscard]] float highsDb() const noexcept { return toDbPow(bandPow_[2]); }
    [[nodiscard]] float bandAvgDb(int band) const noexcept {
        return bandHops_ > 0 ? toDbPow(bandSum_[static_cast<std::size_t>(band)] / static_cast<double>(bandHops_)) : -140.0f;
    }

    // Seconds of audio with signal (above -70 dBFS) since reset.
    [[nodiscard]] double heardSeconds() const noexcept { return heard_ / sr_; }
    [[nodiscard]] bool   hearingNow() const noexcept { return quietFor_ < 0.5 * sr_; }

    // ---- plain-words checks ----
    enum class Mark : std::uint8_t { Good, Note, Warn, Bad };
    struct Check {
        Mark        mark = Mark::Note;
        std::string text;
    };
    [[nodiscard]] std::vector<Check> checks() const;

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double run(double x) noexcept {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    static float toDb(float lin) noexcept;
    static float toDbPow(double p) noexcept;
    void analyseHop();
    void addQuarter(double energy);

    double sr_;
    Fft    fft_;
    std::vector<float>               window_;
    float                            windowNorm_ = 1.0f;   // amplitude -> full-scale sine = 1
    std::vector<StereoFrame>         hist_;                // last kFftSize frames (ring)
    std::size_t                      histPos_ = 0, sinceHop_ = 0, filled_ = 0;
    std::vector<std::complex<float>> buf_;
    std::vector<float>               rows_;                // kRows x kPoints
    std::array<float, kPoints>       pending_{};           // max of the FFTs of the row being built
    int                              pendingCount_ = 0;
    int                              newest_ = 0;
    std::uint64_t                    rowsMade_ = 0;
    std::array<int, kPoints>         binLo_{}, binHi_{};   // FFT bins [lo, hi] of each point
    std::array<double, 3>            bandPow_{}, bandSum_{};
    std::uint64_t                    bandHops_ = 0;

    // loudness
    std::array<std::array<Biquad, 2>, 2> kw_{};            // [channel][stage]
    double quarterSum_ = 0.0;
    std::size_t quarterLen_ = 0, quarterFill_ = 0;
    std::array<double, 30> quarters_{};
    std::size_t quarterCount_ = 0;
    double momentary_ = kSilentLufs, shortTerm_ = kSilentLufs;
    std::vector<double> blocks_;                           // 400 ms block energies (every 100 ms)
    mutable double integrated_ = kSilentLufs;
    mutable std::size_t integratedAt_ = 0;

    // peaks, stereo
    float peakNow_ = 0.0f, peakHold_ = 0.0f, peakMax_ = 0.0f;
    double holdAge_ = 0.0;
    std::uint64_t clipped_ = 0;
    double lastClipAt_ = -1.0;
    double sLR_ = 0.0, sLL_ = 0.0, sRR_ = 0.0, corrK_ = 0.0;
    float corrNow_ = 0.0f, corrAvg_ = 0.0f;
    double heard_ = 0.0, quietFor_ = 1.0e12;
};

} // namespace hc::analysis

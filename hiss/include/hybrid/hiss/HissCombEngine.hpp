#pragma once
// =============================================================================
//  HEM HISS COMB ENGINE (hem_hiss_comb_engine)
//
//  What it removes: a fixed repeating pattern. In the real Suno files measured so far (see real/CHARACTERISATION.md)
//  the faint shimmer is a set of very thin steady lines exactly 25 Hz apart (a signal that repeats every 40 ms),
//  and the pattern is locked to the sample clock, so it can be learned and subtracted.
//
//  How: every sample is filed into a "fold" that is 80 ms long (two repeats). Averaging many repeats makes the music
//  cancel out and the pattern stay. Every second the fold is read line by line (25 Hz apart); each line is kept only if
//  it clearly stands out of the noise measured BETWEEN the lines (a per-line Wiener gain), then the lines are
//  re-built into one 40 ms pattern. That pattern, scaled by Reduction, is subtracted from the audio.
//  Quiet moments teach the pattern; loud moments barely change it (inverse-variance weighting), so the music
//  does not leak in. Subtracting a known pattern cannot soften a cymbal: there is no gain riding, no transient guard needed.
//
//  Latency: 0 samples. Audio thread: no allocation, no locks. All work is spread over the blocks (a refresh every ~1 s).
//  Needs the sample rate to be a multiple of 25 Hz (every common rate is); otherwise it passes the audio through.
// =============================================================================
#include "hybrid/hiss/HissConfig.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hc::hiss {

// Every field is wired to real DSP. Nothing here is a dummy control.
struct CombParams {
    float reductionDb  = 12.0f;     // 0..kMaxReductionDb  how far each line is turned down (12 dB = 75 % of the line removed)
    float loHz         = cfg::kCombLoDefaultHz;   // lowest line that may be touched
    float hiHz         = cfg::kCombHiDefaultHz;   // highest line that may be touched
    float mix          = 1.0f;      // 0 = original, 1 = fully treated
    float adaptSeconds = cfg::kCombAdaptDefaultSeconds;   // memory: longer = steadier pattern, slower to follow a change
    float sensitivity  = 0.5f;      // 0..1 how weak a line may be and still be removed (higher = also weak lines)
    bool  bypass       = false;     // click-free
    bool  listenRemoved = false;    // play only what is being subtracted
};

class HissCombEngine {
public:
    HissCombEngine();

    // message thread -------------------------------------------------------------------------------
    void prepare(double sampleRate);   // allocates everything the audio thread needs
    void reset() noexcept;             // forget the pattern and all state
    // Learning on (default): quiet moments keep teaching the pattern. Off: the learned pattern is frozen and only subtracted
    // (offline use after prime(), so loud music can never change it).
    void setLearning(bool on) noexcept { learning_.store(on, std::memory_order_relaxed); }
    // Offline: learn the pattern from `frames` of audio (the audio is not changed), finish the first refresh, and rewind the
    // clock to sample 0. A pattern learned from the whole file means even the first second is treated.
    void prime(const float* left, const float* right, std::uint32_t frames);

    // any thread -------------------------------------------------------------------------------------
    void setParams(const CombParams& p) noexcept;
    [[nodiscard]] CombParams params() const noexcept;

    // audio thread -----------------------------------------------------------------------------------
    void process(float* left, float* right, std::uint32_t frames) noexcept;

    // info -------------------------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t latency() const noexcept { return 0; }
    [[nodiscard]] bool   supported() const noexcept { return supported_; }
    [[nodiscard]] int    periodSamples() const noexcept { return period_; }
    [[nodiscard]] double sampleRate() const noexcept { return fs_; }
    [[nodiscard]] float  patternLevelDb() const noexcept { return levelDb_.load(std::memory_order_relaxed); }   // RMS of the learned pattern, dBFS (L+R)
    [[nodiscard]] int    linesKept() const noexcept { return linesKept_.load(std::memory_order_relaxed); }    // lines the last refresh found worth removing
    [[nodiscard]] bool   suspect() const noexcept { return suspect_.load(std::memory_order_relaxed); }         // the learned pattern was too loud to be believed and was not used

private:
    struct Chan {
        std::vector<double> S, Z;                 // fold sums (2 periods)
        std::vector<double> SK, ZK, TK;           // offline only: separate fold sums per time chunk, for the agreement test
        std::array<bool, 6> validK{};
        std::vector<double> T2;                   // snapshot S / (Z + Z0)
        std::vector<float>  w[2];                 // learned pattern, one period, double buffered
        std::vector<double> cRe, cIm;             // kept line amplitudes (already Wiener-weighted), per line
        int    active = 0;                        // which w[] the audio thread reads
        double hpS1[2] = {0, 0}, hpS2[2] = {0, 0};   // two biquad states
        double p = 0.0;                           // short-term power of the high-passed signal
        double blockMin = 1e300;
        std::array<double, 256> minRing{};        // lowest short-term power of each 0.25 s block, last 64 s
        int    ringFill = 0, ringPos = 0, blockCount = 0;
        double permMin = 1e300;                   // lowest ever (offline learning pass: the quiet floor of the whole file)
        int    warm = 0;
        double pref = 1e300;
        std::uint64_t gated = 0, gatedTotal = 0;   // offline: quiet samples seen so far / in total
    };
    struct Ramp {
        float cur = 0.0f, target = 0.0f, step = 0.0f;
        float next() noexcept {
            if (cur < target) { cur += step; if (cur > target) cur = target; }
            else if (cur > target) { cur -= step; if (cur < target) cur = target; }
            return cur;
        }
    };
    struct Snap { float red, lo, hi, mix, adapt, sens; bool bypass, listen; };

    [[nodiscard]] Snap snapshot() const noexcept;
    void clearState() noexcept;
    void refreshWork(int items) noexcept;          // does a few slices of the refresh
    void finishRefreshNow() noexcept;              // does the whole refresh (offline only)
    void beginRefresh() noexcept;
    bool step(int ch) noexcept;                    // one work item; true when the refresh of this channel is finished
    void processOne(float& l, float& r) noexcept;

    // ---- setup ---------------------------------------------------------------------------------------
    double fs_ = 48000.0;
    int    period_ = 1920, period2_ = 3840;
    bool   supported_ = false, prepared_ = false;
    int    jMin_ = 1, jMax_ = 1;                    // line numbers (frequency = j * spacing) that can ever be touched
    std::vector<double> cosT_, sinT_;               // e^{-2 pi i n / period2}
    std::vector<double> hRe_, hIm_;                 // response of the high-pass at every line, per line
    double hp_[2][5] = {};                          // biquad coefficients b0 b1 b2 a1 a2
    double pAlpha_ = 0.01;
    std::array<Chan, 2> ch_;

    // ---- running state ---------------------------------------------------------------------------------
    int    m2_ = 0, m1_ = 0;                        // position in the 2-period fold and in the 1-period pattern
    Snap   snap_{};
    Ramp   mixR_, byR_, listenR_;
    float  gainSub_ = 0.0f;
    double forget_ = 0.0;
    // refresh state machine (both channels share the schedule)
    int    stage_ = 0;                              // 0 idle, 1 snapshot, 2 lines, 3 synth
    int    lineIdx_ = 0, synthIdx_ = 0, chIdx_ = 0;
    double workCredit_ = 0.0, itemsPerSample_ = 0.0;
    int    refreshSamples_ = 0, sinceRefresh_ = 0;
    double kappa_ = 2.0;
    float  lo_ = 0.0f, hi_ = 0.0f;                  // band used by the refresh under way

    std::atomic<float> red_{12.0f}, lo_p_{0.0f}, hi_p_{0.0f}, mix_p_{1.0f}, adapt_p_{3.0f}, sens_p_{0.5f};
    std::atomic<bool>  bypass_{false}, listen_{false};
    std::atomic<float> levelDb_{-200.0f};
    std::atomic<int>   linesKept_{0};
    std::atomic<bool>  learning_{true}, suspect_{false};
    bool   infiniteMemory_ = false, suspectNow_ = false, everRefreshed_ = false, freezeFloor_ = false, hardGate_ = false, agree_ = false;
    int    warmN_ = 2400;
    std::vector<double> floorLog_[2];          // offline only
    static constexpr int kChunks = 6;
};

} // namespace hc::hiss

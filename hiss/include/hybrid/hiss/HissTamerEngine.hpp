#pragma once
// =============================================================================
//  HEM HISS TAMER ENGINE (hem_hiss_tamer_engine)
//  Pure C++, no JUCE. Reduces steady high-frequency hiss, tones or flutter inside a
//  chosen frequency focus, and lets cymbals and other sudden sounds through.
//
//  How it works, in plain words:
//    * The sound is cut into overlapping 2048-point slices (4096 above 64 kHz).
//    * For every frequency bin in the watched range it learns what is ALWAYS there
//      (the steady part) and how steady that is. Steady and noise-like = hiss.
//      A sudden loud hit is not steady, so it is not touched.
//    * It builds the "removed" signal (only the steady artifact) and subtracts it
//      from the original, delayed by the engine's latency. When nothing is removed
//      the output is the input, bit for bit, just delayed.
//    * A transient guard lets every attack through untouched, and the cut fades in
//      slowly and out quickly so nothing pumps or sounds watery.
//
//  Thread rules (same as every Hybrid Core effect):
//    message thread : prepare(), prime() - may allocate
//    any thread     : setParams() / set...() - lock-free, one relaxed atomic per control
//    audio thread   : process() - no allocation, no locks, no waiting
//
//  Stereo, in place. Latency = fftSize() samples (reported by latency()).
//  Nothing here claims perfect separation: it removes what looks steady and noise-like.
// =============================================================================
#include "hybrid/hiss/HissConfig.hpp"
#include "hybrid/hiss/HissFft.hpp"

#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

namespace hc::hiss {

enum class Method : int { Floor = 0, Percentile = 1, Tonal = 2 };
enum class Mode   : int { Stereo = 0, MidSide = 1 };

[[nodiscard]] const char* methodName(Method m) noexcept;
[[nodiscard]] const char* modeName(Mode m) noexcept;

// Every field is wired to real DSP. Nothing here is a dummy control.
struct Params {
    float  sensitivity   = cfg::kDefaultSensitivity;                 // 0..1  how easily steady hiss is detected
    float  reductionDb   = cfg::kDefaultReductionDb;                 // 0..kMaxReductionDb  most the cut may ever be
    float  focusHz       = static_cast<float>(cfg::kFocusDefaultHz); // centre of the watched band
    float  widthHz       = static_cast<float>(cfg::kWidthDefaultHz); // flat part of the band (soft edges are added)
    float  mix           = 1.0f;                                     // 0 = original, 1 = fully treated
    float  steadySeconds = cfg::kDefaultSteadySeconds;               // memory of the Percentile / Tonal detectors
    float  smoothMs      = cfg::kPowerSmoothMs;                      // how slowly the level is averaged (long = flutter is seen as one steady level)
    bool   bypass        = false;                                    // click-free, delay-matched
    bool   listenRemoved = false;                                    // play only what is being cut
    Mode   mode          = Mode::Stereo;                             // linked, or separate Mid and Side
    Method method        = Method::Floor;                            // which detector (AUTO FIX picks this)
};

class HissTamerEngine {
public:
    HissTamerEngine();

    // message thread ----------------------------------------------------------------
    void prepare(double sampleRate);   // allocates everything the audio thread will need
    void reset() noexcept;             // forget everything (detector memory too)
    // Offline helper: run the detector over `frames` of audio so the first seconds of a file
    // are cut as well as the rest. Output is thrown away; the audio path is cleared afterwards.
    void prime(const float* left, const float* right, std::uint32_t frames);

    // any thread ---------------------------------------------------------------------
    void setParams(const Params& p) noexcept;
    [[nodiscard]] Params params() const noexcept;

    // audio thread -------------------------------------------------------------------
    void process(float* left, float* right, std::uint32_t frames) noexcept;

    // info ---------------------------------------------------------------------------
    [[nodiscard]] std::uint32_t latency() const noexcept { return static_cast<std::uint32_t>(n_); }
    [[nodiscard]] int           fftSize() const noexcept { return n_; }
    [[nodiscard]] double        sampleRate() const noexcept { return fs_; }
    [[nodiscard]] float         reductionDb() const noexcept { return reductionDb_.load(std::memory_order_relaxed); }   // <= 0, average cut inside the focus
    [[nodiscard]] float         artifactDb() const noexcept { return artifactDb_.load(std::memory_order_relaxed); }     // level of what is being treated, dB per bin
    [[nodiscard]] float         guardAmount() const noexcept { return guardMeter_.load(std::memory_order_relaxed); }    // 0..1, 1 = a hit is being protected

    // The analysis window (square root of a periodic Hann). Public so the tests can prove perfect reconstruction.
    [[nodiscard]] static std::vector<float> makeWindow(int n);

private:
    using Cx = std::complex<float>;

    struct Ramp {                // linear ramp that lands exactly on its target
        float cur = 0.0f, target = 0.0f, step = 0.0f;
        void set(float t) noexcept { target = t; }
        float next() noexcept {
            if (cur < target) { cur += step; if (cur > target) cur = target; }
            else if (cur > target) { cur -= step; if (cur < target) cur = target; }
            return cur;
        }
    };

    struct Detector {
        std::vector<float> S, Fm, curMin, slotMin, ringS, ringF, gain, medS, base, minS, aTmp, cTmp, gTmp;
        float guard = 0.0f, energyAvg = 0.0f;
        void clear() noexcept;
    };

    struct Snap {
        float sens, red, focus, width, mix, steady, smoothA;
        bool  bypass, listen;
        Mode  mode;
        Method method;
    };

    void frame() noexcept;
    void updateFocus() noexcept;
    void clearDetectors() noexcept;
    void runDetector(Detector& d, const float* P, float gmin) noexcept;
    [[nodiscard]] Snap snapshot() const noexcept;

    // ---- setup -----------------------------------------------------------------
    double fs_ = 48000.0;
    int    n_ = 2048, hop_ = 512, bins_ = 1025;
    int    k0_ = 0, k1_ = 0, nT_ = 0;     // tracked bins k0_..k1_
    int    tMax_ = 8, framesPerSlot_ = 47, warmFrames_ = 94;
    float  olaScale_ = 0.5f, gainFallK_ = 0.1f, guardRiseK_ = 0.1f, guardFallK_ = 0.1f, guardDecay_ = 0.9f;
    FftT<float> fft_;
    std::vector<float> win_;
    std::array<std::vector<float>, 2> in_, out_;
    std::vector<Cx> z_, xl_, xr_;
    std::array<Detector, 2> det_;
    std::vector<float> w_, scratch_;
    std::array<std::vector<float>, 2> pw_;
    std::vector<float> primeL_, primeR_;
    bool   prepared_ = false;

    // ---- running state -----------------------------------------------------------
    int  pos_ = 0, hopCount_ = 0;
    int  ringPos_ = 0, ringFill_ = 0, slotFrame_ = 0, slot_ = 0, slotsFilled_ = 0, warm_ = 0;
    Snap snap_{};
    float focusCacheHz_ = -1.0f, widthCacheHz_ = -1.0f;
    int  aLo_ = 0, aHi_ = -1;               // active (focus weight > 0) range, tracked-bin indices
    Mode lastMode_ = Mode::Stereo;
    Ramp mixRamp_, bypassRamp_, listenRamp_;
    float reductionAvg_ = 0.0f;

    // ---- parameters (lock-free) ----------------------------------------------------
    std::atomic<float> sens_{cfg::kDefaultSensitivity}, red_{cfg::kDefaultReductionDb},
        focus_{static_cast<float>(cfg::kFocusDefaultHz)}, width_{static_cast<float>(cfg::kWidthDefaultHz)},
        mix_{1.0f}, steady_{cfg::kDefaultSteadySeconds}, smooth_{cfg::kPowerSmoothMs};
    std::atomic<int> bypass_{0}, listen_{0}, mode_{0}, method_{0};

    // ---- meters ------------------------------------------------------------------
    std::atomic<float> reductionDb_{0.0f}, artifactDb_{-120.0f}, guardMeter_{0.0f};
};

} // namespace hc::hiss

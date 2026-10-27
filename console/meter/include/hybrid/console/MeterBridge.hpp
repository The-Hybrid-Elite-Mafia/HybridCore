#pragma once
// =============================================================================
//  HEM METER BRIDGE — stereo loudness / peak / stereo-image / spectrum meter engine (hc::console::meter).
//
//  What it measures (the reference desktop meter's toolbox, built on the published standards it follows):
//    * Loudness, ITU-R BS.1770-4 / EBU R128: momentary (400 ms), short-term (3 s), integrated ("program",
//      gated -70 LUFS / -10 LU), loudness range (EBU Tech 3342: -70 / -20 LU gates, 10th..95th percentile),
//      max momentary, max short-term, short-term history for the loudness radar.
//    * True peak (4x oversampled, BS.1770-4 Annex 2 method) per channel, max since reset; sample peak.
//    * Peak-to-loudness ratio (PLR = true-peak max - integrated).
//    * Stereo: correlation, left/right balance, mid/side balance, mono (downmix) deviation.
//    * RTA: 31 third-octave bands (20 Hz .. 20 kHz, base-10 centres), level + peak hold.
//    * Vectorscope: the most recent frames for drawing.
//  RESET clears everything measured "since reset" (integrated, LRA, maxima, history).
//
//  Threads: prepare() allocates (message thread). process() is real-time safe (no allocation, no locks).
//  Every reading is an atomic, safe to read from the UI thread at any time.
//  Display options of the reference unit (presets, radar speed, scales) are not in the documents we have:
//  see ConsoleSpecs.hpp, namespace meter.
// =============================================================================
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

namespace hc::console {

class MeterBridge {
public:
    static constexpr int kBands = 31;
    static constexpr double kSilent = -144.0;   // LUFS / dB reading when there is nothing to measure

    MeterBridge();
    ~MeterBridge();
    MeterBridge(const MeterBridge&) = delete;
    MeterBridge& operator=(const MeterBridge&) = delete;

    void prepare(double sampleRate, std::uint32_t maxBlock, double historySeconds = 3600.0);
    void reset() noexcept;                                      // the RESET button
    void process(const float* left, const float* right, std::uint32_t frames) noexcept;

    // ---- loudness ----
    [[nodiscard]] double momentaryLufs() const noexcept;
    [[nodiscard]] double shortTermLufs() const noexcept;
    [[nodiscard]] double integratedLufs() const noexcept;
    [[nodiscard]] double loudnessRangeLu() const noexcept;
    [[nodiscard]] double maxMomentaryLufs() const noexcept;
    [[nodiscard]] double maxShortTermLufs() const noexcept;
    // ---- peaks ----
    [[nodiscard]] double truePeakDbtp(int side) const noexcept;        // max since reset
    [[nodiscard]] double truePeakNowDbtp(int side) const noexcept;     // last block
    [[nodiscard]] double samplePeakDbfs(int side) const noexcept;      // last block
    [[nodiscard]] double plrDb() const noexcept;                       // true-peak max - integrated
    // ---- stereo ----
    [[nodiscard]] double correlation() const noexcept;                 // -1 .. +1
    [[nodiscard]] double balanceLrLu() const noexcept;                 // left minus right loudness (3 s), LU
    [[nodiscard]] double balanceMsDb() const noexcept;                 // side minus mid energy (3 s), dB
    [[nodiscard]] double monoDeviationLu() const noexcept;             // mono downmix minus stereo loudness (3 s), LU
    // ---- RTA ----
    [[nodiscard]] static double bandHz(int band) noexcept;
    [[nodiscard]] double bandDb(int band) const noexcept;
    [[nodiscard]] double bandPeakDb(int band) const noexcept;
    // ---- loudness radar history (short-term loudness every 100 ms) ----
    [[nodiscard]] std::uint64_t historyCount() const noexcept;
    [[nodiscard]] float historyLufs(std::uint64_t index) const noexcept;   // index < historyCount(); old entries wrap
    // ---- vectorscope ----
    static constexpr std::uint32_t kScopeFrames = 4096;
    [[nodiscard]] std::uint64_t scopeWritten() const noexcept;
    void scopeFrame(std::uint64_t index, float& left, float& right) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace hc::console

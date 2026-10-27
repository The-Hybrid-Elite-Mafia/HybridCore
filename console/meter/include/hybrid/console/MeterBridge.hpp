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
//  Settings: the reference unit's EDIT menu (radar, RTA, vector scope, general) and its front buttons, one-for-one,
//  as plain parameters (params(), set(), get(); saved as "meter.<id>"). The engine uses the ones that change what is
//  measured (pause, RTA channel / resolution / speed / hold, mono downmix, deviation span, alerts, peak-meter
//  ballistics); the screen uses the display ones. Source key MTR-UM; numbers the manual does not give are in
//  ConsoleSpecs.hpp, namespace meter, marked ASSUMED.
//
//  Threads: prepare() allocates (message thread). process() is real-time safe (no allocation, no locks).
//  Every reading and every setting is an atomic, safe from any thread.
// =============================================================================
#include "hybrid/fx/Fx.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace hc::console {

namespace meter_ids {
enum : int {
    // front buttons
    Run = 0,        // RUN / PAUSE: loudness measurement (true peak, correlation, RTA and scope always run)
    View,           // main view: RADAR / RTA / SCOPE
    // EDIT - RADAR
    RadarSpeed, RadarRes, Target, LoudAlert, MomRange,
    // EDIT - RTA
    RtaChannels, RtaBands, RtaTime, RtaHold, RtaRange, RtaTop, RtaAuto, RtaCursor,
    // EDIT - VECTOR SCOPE
    ScopePersist, ScopeScale, ScopeStyle,
    // EDIT - GENERAL
    Layout, LoudMaxMetric, Unit, MonoDownmix, DevSpan, PeakAlert, PeakHold, PeakScale, PpmAlign, TpFull,
    Slot1, Slot2, Slot3, Slot4, Slot5, Slot6,   // CUSTOM layout: what each descriptor slot shows
    // the plug-in's transport options (here: the console's own transport)
    FollowTransport, ResetOnPlay,
    kCount
};
// RadarSpeed choices (seconds per turn) and the descriptor list.
enum View : int { ViewRadar = 0, ViewRta, ViewScope };
enum Desc : int { DescNone = 0, DescProgram, DescTruePeakMax, DescLoudMax, DescRange, DescPlr, DescSliding10, DescShortTerm, DescMomentary,
                  DescCorrelation, DescMonoDev, DescBalance, kDescCount };
enum PeakScaleType : int { ScaleTp60 = 0, ScaleTp30, ScaleTp20, ScaleDigitalPpm, ScaleDin, ScaleNordic, ScaleBbc, ScaleEbu, kScaleCount };
} // namespace meter_ids

class MeterBridge {
public:
    static constexpr int kBands = 31;
    static constexpr int kOctaves = 10;   // 31.5 Hz .. 16 kHz octave bands (the RTA's 10-band resolution)
    static constexpr double kSilent = -144.0;   // LUFS / dB reading when there is nothing to measure

    MeterBridge();
    ~MeterBridge();
    MeterBridge(const MeterBridge&) = delete;
    MeterBridge& operator=(const MeterBridge&) = delete;

    void prepare(double sampleRate, std::uint32_t maxBlock, double historySeconds = 25.0 * 3600.0);

    // ---- settings (the EDIT menu and front buttons) ----
    [[nodiscard]] static std::span<const fx::ParamInfo> params() noexcept;
    void set(int id, float plain) noexcept;
    [[nodiscard]] float get(int id) const noexcept;
    void setDefaults() noexcept;
    [[nodiscard]] static double radarSpeedSeconds(int choice) noexcept;
    [[nodiscard]] double targetLufs() const noexcept;          // TARGET LOUDNESS setting
    [[nodiscard]] double radarResolutionDb() const noexcept;   // RADAR RESOLUTION setting
    // LIBRARY: factory presets (our picks from the published delivery standards: ConsoleSpecs.hpp, meter).
    [[nodiscard]] static int factoryPresetCount() noexcept;
    [[nodiscard]] static const char* factoryPresetName(int preset) noexcept;
    void applyFactoryPreset(int preset) noexcept;
    // The six descriptors the current LAYOUT shows (meter_ids::Desc; Balance takes two slots).
    [[nodiscard]] std::array<int, 6> descriptors() const noexcept;
    // Transport (FOLLOW TRANSPORT): the console tells the meter whether it is playing.
    void setTransportPlaying(bool playing) noexcept;
    [[nodiscard]] bool measuring() const noexcept;                     // RUN and (not following, or playing)
    [[nodiscard]] double measuredSeconds() const noexcept;             // time measured since RESET
    // AUTO (RTA): the scale TOP (and RANGE if AUTO SCALE = RANGE & TOP) from the bands right now.
    void rtaAutoScale() noexcept;
    // Presets (LIBRARY): every setting as text ("id=value" lines) and back.
    [[nodiscard]] std::string settingsText() const;
    int applySettingsText(std::string_view text);
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
    [[nodiscard]] double sliding10Lufs() const noexcept;               // max momentary over the last 10 s
    [[nodiscard]] double ppmDb(int side) const noexcept;               // peak meter with the PEAK METER SCALE's ballistics, dBFS
    [[nodiscard]] double peakHoldDb(int side) const noexcept;          // the meter's peak-hold line (PEAK HOLD), dBFS
    [[nodiscard]] bool peakAlert() const noexcept;                     // PEAK lamp (PEAK ALERT, held for PEAK HOLD)
    [[nodiscard]] bool loudAlert() const noexcept;                     // LOUD lamp (momentary >= TARGET + LOUD ALERT)
    // ---- stereo ----
    [[nodiscard]] double correlation() const noexcept;                 // -1 .. +1
    [[nodiscard]] double balanceLrLu() const noexcept;                 // left minus right loudness (3 s), LU
    [[nodiscard]] double balanceMsDb() const noexcept;                 // side minus mid energy (3 s), dB
    [[nodiscard]] double monoDeviationLu() const noexcept;             // mono downmix minus stereo loudness (DEVIATION SPAN, STEREO-MONO DOWNMIX), LU
    // ---- RTA ----
    [[nodiscard]] static double bandHz(int band) noexcept;
    [[nodiscard]] double bandDb(int band) const noexcept;
    [[nodiscard]] double bandPeakDb(int band) const noexcept;
    [[nodiscard]] static double octaveHz(int octave) noexcept;          // 31.5 .. 16000 Hz
    [[nodiscard]] double octaveDb(int octave) const noexcept;           // RESOLUTION 10 bands
    [[nodiscard]] double octavePeakDb(int octave) const noexcept;
    // ---- loudness radar history (short-term loudness every 100 ms) ----
    [[nodiscard]] std::uint64_t historyCount() const noexcept;
    [[nodiscard]] float historyLufs(std::uint64_t index) const noexcept;   // index < historyCount(); old entries wrap
    // ---- vectorscope ----
    static constexpr std::uint32_t kScopeFrames = 65536;   // 1.36 s at 48 kHz (the longest PERSISTENCE)
    [[nodiscard]] std::uint64_t scopeWritten() const noexcept;
    void scopeFrame(std::uint64_t index, float& left, float& right) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace hc::console

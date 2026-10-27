#pragma once
// =============================================================================
//  HEM TWIN BUS — 2-channel bus compressor + 2-band dynamic EQ engine (hc::console::twinbus).
//
//  Panel (one-for-one with the reference unit; left side = side 1, right side = side 2):
//    per side : LOW THD · F/B · GRIT (colour switch, + 9-level SET DIST) · THRESHOLD (31) · ATTACK (11)
//               · MAKE-UP (31, FINE) · POST D-EQ · S/C HPF (OUT, 10..300 Hz) · RATIO (11, incl. negative)
//               · RELEASE (9 + AUTO + AUTO 2) · MIX (31) · LF GAIN (31, + RESO) · D-EQ LF / HF
//               COMP..EXP knobs (31) with band ON, FREQ (16) and RANGE (23) · HF BELL · HF FAST (+AUTO)
//               · LF FAST (+AUTO) · GR meter (also shows D-EQ freq/range) · activity LEDs
//    centre   : CH 1 IN · MODE (CLASSIC STEREO, SUM S/C STEREO, DUAL MONO, MID-SIDE) · CH 2 IN
//               · MID SOLO / SIDE SOLO (Mid-Side only)
//    settings : MIX mode (classic / parallel) · input trim (0 / +10 / -5 dB) · 26 kHz input LPF
//               · side-solo mode · right meter in stereo modes · transient expander (replaces HF band)
//    ext S/C  : key input for the compressor, the D-EQ or both (any stem can be the key)
//  Stereo modes: side 1 sets both channels; side 2's LF/HF GAIN knob becomes the HF static gain.
//
//  Implements hc::fx::Effect. A key (side-chain) signal can be passed with setKey() before process().
// =============================================================================
#include "hybrid/fx/Fx.hpp"

#include <memory>

namespace hc::console {

[[nodiscard]] std::unique_ptr<fx::Effect> makeTwinBus();

namespace twinbus_ids {
enum : int {
    Mode = 0, ExtSc, ExtScBands, MixMode, InputTrim, Lpf, SideSoloMode, MidSolo, SideSolo, RightMeter,
    kGlobalCount,
    // per side block (add kSideStride * side)
    In = kGlobalCount, Threshold, Attack, Ratio, Release, Makeup, Fine, PostDeq, ScHpf, Mix,
    LowThd, Feedback, Grit, GritLevel,
    LfOn, LfCompExp, LfFreq, LfRange, LfGain, Reso, LfTime,
    HfOn, HfCompExp, HfFreq, HfRange, HfBell, HfTime, TransientExp,
    kSideEnd,
    kSideStride = kSideEnd - kGlobalCount,
    kCount = kGlobalCount + 2 * kSideStride
};
[[nodiscard]] constexpr int id(int side, int p) noexcept { return p < kGlobalCount ? p : p + side * kSideStride; }
// Meters (Effect::meter(i)).
enum : int { MeterGr1 = 0, MeterGr2, MeterLf1, MeterHf1, MeterLf2, MeterHf2, MeterOver1, MeterOver2 };
} // namespace twinbus_ids

// Pass a key (external side-chain) signal for the next process() call; nullptr = none. Audio thread.
void twinBusSetKey(fx::Effect& twinBus, const float* keyL, const float* keyR) noexcept;

// Control laws (exposed for tests and faceplates).
[[nodiscard]] float twinBusThresholdDb(int index) noexcept;     // 0..30 -> -20..+20 dB
[[nodiscard]] float twinBusMakeupDb(int index, bool fine) noexcept;
[[nodiscard]] float twinBusStaticGainDb(int index) noexcept;    // LF/HF GAIN 0..30 -> -10..+10 dB
[[nodiscard]] float twinBusRangeDb(int index) noexcept;         // 0..22 -> 0.5..15 dB
[[nodiscard]] float twinBusBellQ(float gainDb) noexcept;

} // namespace hc::console

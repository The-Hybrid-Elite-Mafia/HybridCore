#pragma once
// =============================================================================
//  HEM STEP EQ — two-channel, 4-band stepped mastering EQ engine (hc::console::stepeq).
//
//  Panel (one-for-one with the reference unit's front panel):
//    BYPASS (global hard bypass)
//    per channel: IN · LO (7 freqs, 11 gains, SHELF) · LO-MID (7, 11) · HI-MID (7, 11)
//                 · HI (7 freqs, 11 gains, SHELF) · RANGE (x1 / x.5 / x.25)
//  Channel 1 processes the left input, channel 2 the right (two independent mono EQs, like the
//  reference; for stereo the app sets both channels the same).
//  Proportional Q follows the EFFECTIVE gain (knob × RANGE), so 6 dB at x1 = 12 dB at x.5.
//  Every band is reciprocal: a cut exactly undoes the same boost.
//
//  Implements hc::fx::Effect (stereo, in place, lock-free parameters).
// =============================================================================
#include "hybrid/fx/Fx.hpp"

#include <memory>

namespace hc::console {

[[nodiscard]] std::unique_ptr<fx::Effect> makeStepEq();

namespace stepeq_ids {
enum : int {
    Bypass = 0,
    // per channel block (add kChannelStride * channel)
    In = 1, LoFreq, LoGain, LoShelf, LoMidFreq, LoMidGain, HiMidFreq, HiMidGain, HiFreq, HiGain, HiShelf, Range,
    kChannelStride = 12,
    kCount = 1 + 2 * kChannelStride
};
[[nodiscard]] constexpr int id(int channel, int p) noexcept { return p + channel * kChannelStride; }
} // namespace stepeq_ids

// Effective gain (dB) and proportional Q of a band (exposed for tests and the faceplate).
[[nodiscard]] float stepEqEffectiveGainDb(int gainIndex, int rangeIndex) noexcept;
[[nodiscard]] float stepEqQ(float effectiveGainDb) noexcept;

} // namespace hc::console

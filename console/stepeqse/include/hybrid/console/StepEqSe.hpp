#pragma once
// =============================================================================
//  HEM STEP EQ SE — the special-edition model of the 2-channel 4-band stepped mastering EQ (hc::console::stepeqse).
//
//  Panel (one-for-one with the reference special edition's front panel, EQAE-OM §2):
//    BYPASS (hard bypass) · M/S (channel 1 = mid, channel 2 = side, recombined at the output)
//    per channel: IN · HP-FILT + HPF knob (40..600 Hz continuous, 12 dB/oct, ahead of the EQ)
//                 · LO (8 freqs, 21 gains, SHELF) · LO-MID (8, 21) · HI-MID (8, 21) · HI (8 freqs, 21 gains, SHELF)
//  Same proportional-Q and fully reciprocal bands as HEM STEP EQ; every band has its own 21 steps
//  (±0.5 dB .. ±12 dB), no RANGE switch.
//  Bands near or above half the sample rate (HI 40 kHz at 48 kHz) are fitted to the analogue curve over
//  the audible band, so their "air" skirt below 20 kHz is still there.
//
//  Implements hc::fx::Effect (stereo, in place, lock-free parameters, no allocation in process()).
// =============================================================================
#include "hybrid/fx/Dsp.hpp"
#include "hybrid/fx/Fx.hpp"

#include <memory>

namespace hc::console {

[[nodiscard]] std::unique_ptr<fx::Effect> makeStepEqSe();

namespace stepeqse_ids {
enum : int {
    Bypass = 0, MidSide = 1,
    // per channel block (add kChannelStride * channel)
    In = 2, HpfOn, HpfHz, LoFreq, LoGain, LoShelf, LoMidFreq, LoMidGain, HiMidFreq, HiMidGain, HiFreq, HiGain, HiShelf,
    kChannelStride = 13,
    kCount = 2 + 2 * kChannelStride
};
[[nodiscard]] constexpr int id(int channel, int p) noexcept { return p + channel * kChannelStride; }
} // namespace stepeqse_ids

// One band's digital design (exposed for tests and the faceplate): band 0..3 = LO, LO-MID, HI-MID, HI.
struct StepEqSeDesign {
    fx::BiquadCoefs c;
    double maxErrDb = 0;   // largest difference from the analogue curve, 20 Hz .. min(20 kHz, 0.48 fs)
    bool fitted = false;   // re-fitted to the analogue curve (centre too close to / above half the sample rate)
};
[[nodiscard]] StepEqSeDesign stepEqSeDesign(double fs, int band, int freqIndex, int gainIndex, bool shelf);
// The analogue curve every band is designed from (dB at hz).
[[nodiscard]] double stepEqSeAnalogDb(int band, int freqIndex, int gainIndex, bool shelf, double hz);

} // namespace hc::console

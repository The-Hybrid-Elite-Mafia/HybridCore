#pragma once
// =============================================================================
//  HEM COLOUR STAGE — stereo analogue-style colour processor engine (hc::console::colour).
//
//  Panel (one-for-one with the reference unit's front panel):
//    O/L LEDs · INPUT TRIM ±12 dB · HPF OFF/30/40/50 Hz (18 dB/oct)
//    DRIVE: DRIVE 1..11 · DENSITY MIN..MAX · IN · tri-colour LED
//    TONE EQ: LOW 30/50/70/90 Hz · HIGH 8/12/16/20 kHz · LOW / HIGH gain ±9 dB · IN
//    HF COMPRESSOR: THRESHOLD +10..-10 dB · X-OVER 3..20 kHz · IN (+ CRUSH mode: X-OVER = WET/DRY) · LED
//    STEREO IMAGE: SPACE ±12 dB · WIDTH ±6 dB · IN
//    TRANSFORMER · INSERT (+ M/S mode) · PRE EQ · OUTPUT TRIM ±12 dB · BYPASS (+ POST I/P TRIM mode)
//    master meter L/R (dBu, 3 s peak hold, shows the input in BYPASS)
//  Signal order follows the reference's signal-flow page. Latency is fixed (the drive stage is
//  oversampled; when it is out, a matching delay keeps the timing the same).
//
//  Implements hc::fx::Effect. The insert is a hc::console::InsertSlot (Mid on left / Side on right in M/S mode).
// =============================================================================
#include "hybrid/console/InsertSlot.hpp"
#include "hybrid/fx/Fx.hpp"

#include <memory>

namespace hc::console {

[[nodiscard]] std::unique_ptr<fx::Effect> makeColourStage();
void colourStageSetInsert(fx::Effect& colourStage, InsertSlot* slot) noexcept;   // message thread; nullptr = empty

namespace colour_ids {
enum : int {
    InputTrim = 0, Hpf,
    DriveIn, Drive, Density,
    EqIn, LowFreq, LowGain, HighFreq, HighGain,
    HfcIn, HfcCrush, HfcThreshold, HfcXover,
    ImageIn, Space, Width,
    Transformer, Insert, InsertMs, PreEq,
    OutputTrim, Bypass, BypassPostTrim,
    kCount
};
// Meters (Effect::meter(i)).
enum : int { MeterOverL = 0, MeterOverR, MeterDriveThd, MeterHfcGr, MeterL, MeterR, MeterHoldL, MeterHoldR };
} // namespace colour_ids

// Control laws / pieces exposed for tests and faceplates.
[[nodiscard]] float colourDriveLedColour(float thdPercent) noexcept;   // 0 off, 1 green, 2 orange, 3 red
[[nodiscard]] float colourCrushWet(float xoverHz) noexcept;              // X-OVER knob position -> 0..1 wet in CRUSH mode

} // namespace hc::console

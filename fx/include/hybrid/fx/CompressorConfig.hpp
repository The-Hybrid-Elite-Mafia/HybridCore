#pragma once
// =============================================================================
//  HEM COMPRESSOR: central config. Every unverified working guess lives in THIS file.
//    // ASSUMED = a guess used so the control works. Not checked against any hardware.
//    // UNKNOWN = not known at all. A plain default is used and it is listed in kNotes.
//  Nothing here copies, or claims to match, any other maker's product.
// =============================================================================
#include <array>
#include <cstddef>

namespace hc::fx::compressor_cfg {

inline constexpr float kThrKnobToDbFS = -18.0f;  // ASSUMED: threshold dBFS = knob + this. From the published note that the unit's meters are calibrated 18 dBu = 0 dBFS; that this also applies to the THRESHOLD knob is a guess.
// DOCUMENTED (public manuals): THRESHOLD spans -15 to +15; lowering it gives more compression.
inline constexpr float kKneeDb       = 6.0f;     // ASSUMED: soft knee, 6 dB wide.
inline constexpr float kAutoFastRelS = 0.1f;     // ASSUMED: AUTO release, fast stage: 0.1 s (what a short peak releases with).
inline constexpr float kAutoSlowAttS = 0.25f;    // ASSUMED: AUTO release, slow stage fills up over 0.25 s, so only longer peaks reach it.
inline constexpr float kAutoSlowRelS = 1.2f;     // ASSUMED: AUTO release, slow stage: 1.2 s (what a long peak releases with).
inline constexpr float kAnalogDrive  = 0.8f;     // ASSUMED: ANALOG = gentle tanh saturation, drive 0.8.
inline constexpr float kAnalogBias   = 0.05f;    // ASSUMED: small bias adds a little even-harmonic colour.
inline constexpr double kScQ         = 0.7071;   // ASSUMED: Butterworth Q for the SC FILTER. DOCUMENTED: it is 12 dB/oct, detector only, sweeps 20 to 500 Hz, lowest setting = off.
inline constexpr float kScOffBelowHz = 20.0f;    // DOCUMENTED: lowest setting defeats the filter.
inline constexpr float kSmoothMs     = 20.0f;    // ASSUMED: make-up, mix and trim glide over 20 ms so knob moves do not click.
inline constexpr float kSwitchMs     = 10.0f;    // ASSUMED: IN and ANALOG switches cross-fade over 10 ms so they do not click.

// Attack steps in ms and release steps in seconds (the last release step is AUTO).
inline constexpr std::array<float, 6> kAttackMs   = {0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f};   // ASSUMED: values are time constants (about 63% of the move).
inline constexpr std::array<float, 5> kReleaseS   = {0.1f, 0.3f, 0.6f, 1.2f, 0.0f};          // ASSUMED: time constants; index 4 = AUTO.
inline constexpr std::array<float, 3> kRatios     = {2.0f, 4.0f, 10.0f};

struct Note { const char* tag; const char* text; };
inline constexpr std::array<Note, 15> kNotes = {{
    {"ASSUMED", "THRESHOLD: -15 to +15, lower = more compression (published). Knob 0 = -18 dBFS is a guess from the published 18 dBu = 0 dBFS meter calibration."},
    {"ASSUMED", "Soft knee, 6 dB wide."},
    {"ASSUMED", "ATTACK value is the time constant (about 63% of the move) in the gain-reduction smoother."},
    {"ASSUMED", "RELEASE steps are time constants. AUTO is program dependent (published). Two stages, fast 0.1 s and slow 1.2 s; the slow one only fills on longer peaks. Stage times are guesses."},
    {"ASSUMED", "Detector is peak, stereo linked (loudest of left and right drives both)."},
    {"ASSUMED", "SC FILTER is a 12 dB/oct high-pass on the detector only, up to 500 Hz, off below 20 Hz (all published). Q 0.7071 and the linear knob taper are guesses."},
    {"ASSUMED", "MIX is parallel: dry plus compressed. MAKE-UP is applied to the compressed part only."},
    {"ASSUMED", "TRIM and FADE sit after the mix."},
    {"ASSUMED", "RATE-S = fade time in seconds (1 to 60). Linear fade. FADE on = fade out, off = fade back in."},
    {"ASSUMED", "ANALOG is ON by default (published). What it does is not published: ours is soft tanh saturation on the compressed part, no noise."},
    {"ASSUMED", "IN off = compressor, make-up and mix are bypassed. TRIM and FADE still work."},
    {"ASSUMED", "Meter ballistics (needle and LEDs) are the faceplate's own choice."},
    {"ASSUMED", "Make-up, mix, trim and the IN / ANALOG switches glide over 10 to 20 ms so moving them does not click."},
    {"UNKNOWN", "Knee shape and width, detector type (peak or RMS), exact AUTO release times, whether ratio shifts the threshold, and the third FADE press are not published."},
    {"UNKNOWN", "The exact curves and timings of any reference hardware. None are copied or claimed."},
}};

} // namespace hc::fx::compressor_cfg

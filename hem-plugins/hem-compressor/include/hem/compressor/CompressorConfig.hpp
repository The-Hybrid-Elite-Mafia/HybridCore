#pragma once
// =============================================================================
//  HEM COMPRESSOR: central config (the ONE place for this plugin's guesses). Every unverified working guess lives in THIS file.
//    // ASSUMED = a guess used so the control works. Not checked against any hardware.
//    // UNKNOWN = not known at all. A plain default is used and it is listed in kNotes.
//  Nothing here copies, or claims to match, any other maker's product.
// =============================================================================
#include <array>
#include <cstddef>
#include <cstdint>

namespace hem::compressor::cfg {

inline constexpr float kThrKnobToDbFS = -18.0f;  // ASSUMED: threshold dBFS = knob + this. From the published note that the unit's meters are calibrated 18 dBu = 0 dBFS; that this also applies to the THRESHOLD knob is a guess.
// DOCUMENTED (public manuals): THRESHOLD spans -15 to +15; lowering it gives more compression.
inline constexpr float kKneeDb       = 6.0f;     // ASSUMED: soft knee, 6 dB wide.
inline constexpr float kAutoFastRelS = 0.1f;     // ASSUMED: AUTO release, fast stage: 0.1 s (what a short peak releases with).
inline constexpr float kAutoSlowAttS = 0.25f;    // ASSUMED: AUTO release, slow stage fills up over 0.25 s, so only longer peaks reach it.
inline constexpr float kAutoSlowRelS = 1.2f;     // ASSUMED: AUTO release, slow stage: 1.2 s (what a long peak releases with).
// ANALOG (published: ON by default, emulates console noise + harmonic distortion; the amount is NOT published).
// Ours is a subtle colour after MIX: y += a2*x^2 (its DC removed) - a3*x^3 + noise. Targets (all ASSUMED):
// level change <= 0.1 dB at 0, -6 and -30 dBFS; 2nd and 3rd harmonic each about -56 / -62 dB at -6 dBFS,
// about -50 dB at 0 dBFS, falling with level; noise around -96 dBFS RMS.
inline constexpr float kAnalogEven   = 0.0063f;  // ASSUMED: 2nd-harmonic amount (2nd = a2*A/2 relative, about -56 dB at -6 dBFS).
inline constexpr float kAnalogOdd    = 0.0127f;  // ASSUMED: 3rd-harmonic amount (3rd = a3*A^2/4 relative, about -62 dB at -6 dBFS; costs <= 0.09 dB at 0 dBFS).
inline constexpr float kAnalogDcHz   = 5.0f;     // ASSUMED: high-pass that removes the DC the even part makes.
inline constexpr float kAnalogInputLimit = 2.0f; // ASSUMED: the colour stops growing above +6 dBFS so overs cannot blow up.
inline constexpr float kAnalogNoiseDbFS = -96.0f;// ASSUMED: console noise floor, white, RMS, each side.
inline constexpr std::uint32_t kNoiseSeed = 0x48454D31u;   // ASSUMED: fixed noise seed ("HEM1"); each instance offsets it, reset() restarts it.
inline constexpr float kFadeSkipMs   = 10.0f;    // ASSUMED: FADE OFF ("skip") jumps to unity with a 10 ms anti-click glide.
inline constexpr double kScQ         = 0.7071;   // ASSUMED: Butterworth Q for the SC FILTER. DOCUMENTED: it is 12 dB/oct, detector only, sweeps 20 to 500 Hz, lowest setting = off.
inline constexpr float kScOffBelowHz = 20.0f;    // DOCUMENTED: lowest setting defeats the filter.
inline constexpr float kSmoothMs     = 20.0f;    // ASSUMED: make-up, mix and trim glide over 20 ms so knob moves do not click.
inline constexpr float kSwitchMs     = 10.0f;    // ASSUMED: IN and ANALOG switches cross-fade over 10 ms so they do not click.

// Attack steps in ms and release steps in seconds (the last release step is AUTO).
inline constexpr std::array<float, 6> kAttackMs   = {0.1f, 0.3f, 1.0f, 3.0f, 10.0f, 30.0f};   // ASSUMED: values are time constants (about 63% of the move).
inline constexpr std::array<float, 5> kReleaseS   = {0.1f, 0.3f, 0.6f, 1.2f, 0.0f};          // ASSUMED: time constants; index 4 = AUTO.
inline constexpr std::array<float, 3> kRatios     = {2.0f, 4.0f, 10.0f};

struct Note { const char* tag; const char* text; };
inline constexpr std::array<Note, 17> kNotes = {{
    {"ASSUMED", "THRESHOLD: -15 to +15, lower = more compression (published). Knob 0 = -18 dBFS is a guess from the published 18 dBu = 0 dBFS meter calibration."},
    {"ASSUMED", "Soft knee, 6 dB wide."},
    {"ASSUMED", "ATTACK value is the time constant (about 63% of the move) in the gain-reduction smoother."},
    {"ASSUMED", "RELEASE steps are time constants. AUTO is program dependent (published). Two stages, fast 0.1 s and slow 1.2 s; the slow one only fills on longer peaks. Stage times are guesses."},
    {"ASSUMED", "Detector is peak, stereo linked (loudest of left and right drives both)."},
    {"ASSUMED", "SC FILTER is a 12 dB/oct high-pass on the detector only, up to 500 Hz, off below 20 Hz (all published). Q 0.7071 and the linear knob taper are guesses."},
    {"ASSUMED", "MIX is parallel: dry plus compressed. MAKE-UP is applied to the compressed part only."},
    {"ASSUMED", "TRIM and FADE sit after the mix."},
    {"ASSUMED", "RATE-S = fade time in seconds (1 to 60, published). Linear fade. Default 10 s is a guess."},
    {"ASSUMED", "FADE has three steps (published: fade-out, fade-in, skip). Button cycles OFF -> FADE OUT -> FADE IN -> OFF. FADE OUT ramps to silence over RATE-S; FADE IN ramps from the current level up to unity over RATE-S; OFF = 'skip' jumps to unity (10 ms anti-click). What 'skip' exactly does on the original is a guess. Old sessions: saved 1 = FADE OUT."},
    {"ASSUMED", "ANALOG is ON by default (published). Amount not published: ours adds about -56 dB 2nd and -62 dB 3rd harmonic at -6 dBFS (rising gently with level), changes level by under 0.1 dB, and adds seeded white noise at about -96 dBFS. Acts after MIX, so it colours the whole output, not only the compressed part."},
    {"ASSUMED", "Defaults: THRESHOLD 0, MAKE-UP 0, TRIM 0, SC FILTER off, FADE OFF (neutral). MIX 100% and ANALOG ON are published defaults. RATIO 4:1, ATTACK 3 ms, RELEASE 0.6 s and RATE-S 10 s have no zero and no published default: our picks."},
    {"ASSUMED", "IN off = compressor, make-up and mix are bypassed. TRIM and FADE still work."},
    {"ASSUMED", "Meter ballistics (needle and LEDs) are the faceplate's own choice."},
    {"ASSUMED", "Make-up, mix, trim and the IN / ANALOG switches glide over 10 to 20 ms so moving them does not click."},
    {"UNKNOWN", "Knee shape and width, detector type (peak or RMS), exact AUTO release times, whether ratio shifts the threshold, and exactly what the FADE skip step does are not published."},
    {"UNKNOWN", "The exact curves and timings of any reference hardware. None are copied or claimed."},
}};

} // namespace hem::compressor::cfg

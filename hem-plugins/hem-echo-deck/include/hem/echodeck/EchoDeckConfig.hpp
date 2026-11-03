#pragma once
// =============================================================================
//  HEM ECHO DECK: central config (the ONE place for this plugin's guesses).
//    // ASSUMED = a guess used so the control works. Not checked against any hardware or other plug-in.
//    // UNKNOWN = not known at all. A plain default is used and it is listed in kNotes.
//  The control set is "in the spirit of" a classic tape-echo style plug-in. Nothing here copies its
//  code, curves, preset or style names, or art. The style names below are our own.
// =============================================================================
#include <array>
#include <cstdint>

namespace hem::echodeck::cfg {

// ---- time ---------------------------------------------------------------------------------
inline constexpr float  kTimeMinMs   = 1.0f;     // ASSUMED: shortest echo (flanging range starts near 0).
inline constexpr float  kTimeMaxMs   = 2500.0f;  // ASSUMED: longest echo in MS mode.
inline constexpr double kMaxTimeS    = 4.0;      // ASSUMED: any echo time (also NOTE modes at slow tempos) is capped at 4 s.
inline constexpr float  kBpmMin      = 30.0f;    // DOCUMENTED (reference manual): manual tempo 30 to 300 BPM.
inline constexpr float  kBpmMax      = 300.0f;
inline constexpr float  kGlideMs     = 40.0f;    // ASSUMED: MS mode glides to a new time tape-style (pitch bends while it moves; manual: Time mode pitch-shifts).
inline constexpr float  kNoteFadeMs  = 50.0f;    // ASSUMED: NOTE / DOT / TRIP change by cross-fading the old and new times (manual: crossfades smoothly).
// Note values in beats (a beat = a quarter note).
inline constexpr std::array<double, 7> kNoteBeats = {1.0 / 16, 1.0 / 8, 1.0 / 4, 1.0 / 2, 1.0, 2.0, 4.0};   // 1/64 .. 1/1
inline constexpr double kDotMul  = 1.5;
inline constexpr double kTripMul = 2.0 / 3.0;

// ---- tap tempo ----------------------------------------------------------------------------
inline constexpr double kTapMaxGapS  = 2.0;      // ASSUMED: a gap longer than 2 s (30 BPM) starts a new tap series.
inline constexpr double kTapMinGapS  = 0.2;      // = 300 BPM; quicker taps are ignored.
inline constexpr int    kTapAverage  = 4;        // ASSUMED: average of the last 4 gaps.

// ---- groove / feel -------------------------------------------------------------------------
inline constexpr double kGrooveMax   = 1.0 / 3.0;// ASSUMED: full GROOVE moves every other repeat by 1/3 of the echo time (= triplet shuffle / swing).
inline constexpr double kFeelMaxMs   = 30.0;     // ASSUMED: full FEEL moves every repeat 30 ms late (DRAG) or early (RUSH).

// ---- feedback / levels --------------------------------------------------------------------
inline constexpr float kFeedbackMax  = 1.0f;     // ASSUMED: FEEDBACK 100% = loop gain 1 (endless repeats, held in by the loop limiter, never louder).
inline constexpr float kInOutMinDb   = -24.0f;   // ASSUMED: INPUT / OUTPUT range (echo only, not the dry sound - manual).
inline constexpr float kInOutMaxDb   = 12.0f;
inline constexpr float kSatMaxDriveDb= 18.0f;    // ASSUMED: SATURATION 100% drives the echo 18 dB harder into the style's curve (level is then taken back down).
inline constexpr double kFilterQ     = 0.7071;   // ASSUMED: LOW CUT / HIGH CUT and the style band limits are 12 dB/oct Butterworth.
inline constexpr float kLowCutOffHz  = 20.0f;    // ASSUMED: LOW CUT at its lowest = off.
inline constexpr float kHighCutOffHz = 20000.0f; // ASSUMED: HIGH CUT at its highest = off.
inline constexpr float kSmoothMs     = 20.0f;    // ASSUMED: input, output, mix and feedback glide over 20 ms (no clicks).
inline constexpr float kMeterFloorDb = -120.0f;

// ---- diffusion (inside the loop: each repeat gets smeared more) ---------------------------
inline constexpr std::array<double, 4> kDiffuseMs = {4.7, 6.3, 8.9, 11.3};   // ASSUMED: 4 all-pass stages.
inline constexpr float kDiffuseGainMax = 0.7f;   // ASSUMED: all-pass gain at full diffusion.

// ---- styles (our own names and voicings - every number ASSUMED) ---------------------------
enum class Shape : std::uint8_t { Clean, Soft, Tape, Hard };
struct Style {
    const char* name;
    float hpHz, lpHz;        // band limits on every repeat (0 = none)
    float driveDb;           // how hard the echo hits the curve at SATURATION 0
    Shape shape;
    float wowMs, wowHz;      // slow pitch drift
    float flutterMs, flutterHz;
    float diffusion;         // 0..1
};
inline constexpr std::array<Style, 8> kStyles = {{
    {"CLEAN",     0.0f,     0.0f,    0.0f, Shape::Clean, 0.0f,  0.0f, 0.0f,  0.0f, 0.0f},
    {"ANALOG",   40.0f,  6000.0f,    3.0f, Shape::Soft,  0.05f, 0.5f, 0.0f,  0.0f, 0.0f},
    {"TAPE",     50.0f,  9000.0f,    2.0f, Shape::Tape,  0.15f, 0.6f, 0.03f, 6.0f, 0.0f},
    {"WORN TAPE",120.0f, 4500.0f,    6.0f, Shape::Tape,  0.6f,  0.8f, 0.08f, 7.0f, 0.0f},
    {"DARK DRUM", 80.0f, 2200.0f,    4.0f, Shape::Soft,  0.3f,  3.0f, 0.0f,  0.0f, 0.25f},
    {"RADIO",    400.0f, 4000.0f,    8.0f, Shape::Hard,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f},
    {"PHONE",    700.0f, 2800.0f,   10.0f, Shape::Hard,  0.0f,  0.0f, 0.0f,  0.0f, 0.0f},
    {"SMEAR",     60.0f, 8000.0f,    1.5f, Shape::Soft,  0.0f,  0.0f, 0.0f,  0.0f, 0.7f},
}};
inline constexpr float kTapeEven = 0.15f;        // ASSUMED: TAPE curve's even-harmonic lean (tanh(u + 0.15 u^2)).
inline constexpr double kModMaxMs = 1.0;         // largest wow + flutter any style uses, plus margin (sizes the lines).

struct Note { const char* tag; const char* text; };
inline constexpr std::array<Note, 15> kNotes = {{
    {"ASSUMED", "Time: 1 ms to 2.5 s in MS mode. Every echo time is capped at 4 s (also at slow tempos in NOTE modes). The reference manual does not give its ranges."},
    {"ASSUMED", "MS mode glides to a new time over about 40 ms (pitch bends while it moves). NOTE / DOT / TRIP cross-fade to the new time over 50 ms. Both behaviours are described in the reference manual; our times are guesses."},
    {"ASSUMED", "Note values 1/64 to 1/1; DOT = x1.5, TRIP = x2/3. Tempo: the host's (HOST) or our own (TAP: the BPM knob, which TAP sets)."},
    {"ASSUMED", "TAP: each press flips the TAP switch; the engine times the gaps (to the audio block, about 11 ms at 512 samples) and averages the last 4. A gap over 2 s starts again. Gaps under 0.2 s (300 BPM) are ignored."},
    {"ASSUMED", "MIX: at 50% dry and echo are both full; below, the echo comes down; above, the dry comes down (reference manual: 12 o'clock = 50/50, past 12 dry falls away). Default 50%."},
    {"ASSUMED", "FEEDBACK 100% = loop gain 1: endless repeats, never louder. A soft limiter sits in the loop so nothing can blow up."},
    {"ASSUMED", "INPUT / OUTPUT -24 to +12 dB, echo only (reference manual: they do not touch the dry sound). INPUT also sets how hard the echo hits the style curve."},
    {"ASSUMED", "LOW CUT 20 Hz to 2 kHz (20 = off), HIGH CUT 1 kHz to 20 kHz (20 k = off), 12 dB/oct, inside the loop: every repeat is filtered again. The reference manual does not document a LOW CUT; we added it because it was asked for."},
    {"ASSUMED", "GROOVE: -100% = shuffle (every other repeat later), +100% = swing (every other repeat earlier). Full = 1/3 of the echo time."},
    {"ASSUMED", "FEEL: -100% = DRAG (every repeat 30 ms late), +100% = RUSH (30 ms early)."},
    {"ASSUMED", "SATURATION 0 to 100% adds 0 to 18 dB of drive into the style's curve, then takes the level back down. Default 0 (the reference manual's default is 12 o'clock; our rule is zero)."},
    {"ASSUMED", "STYLES are our own 8 voicings (band limits, curve, wow / flutter, diffusion). None is modelled on, or claims to sound like, any unit or plug-in."},
    {"ASSUMED", "PING-PONG sums the input to mono; repeats alternate left, then right."},
    {"ASSUMED", "Defaults: INPUT, OUTPUT, FEEDBACK, GROOVE, FEEL, SATURATION 0; LOW / HIGH CUT off; STYLE CLEAN; MODE SINGLE; TIME 1/4 note at the host tempo; MIX 50%; own BPM 120."},
    {"UNKNOWN", "The reference unit's exact knob ranges, curves, saturation amounts and style voicings are not published. None are copied or claimed."},
}};

} // namespace hem::echodeck::cfg

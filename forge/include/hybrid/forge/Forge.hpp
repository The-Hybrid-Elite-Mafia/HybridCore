#pragma once
// =============================================================================
//  hc::forge — BEAT FORGE inside The Hybrid Core: 8 synthesized tracks (no samples,
//  nothing to license), a rule-based composer by genre, and a song arranger.
//  Ported from Hybrid Studio 2.0.9 (hybrid-studio-android/pc/Source/Engine/BeatForge),
//  rewritten without JUCE. This header: track list, sound knobs, choice lists, parameter ids.
//
//  In the console each Beat Forge track plays through its own stem strip (KICK = strip 1 ...
//  PAD = strip 8), so it gets the strip's EQ, compressor, fader, pan, mutes and meters.
// =============================================================================
#include <array>
#include <cstdint>
#include <string>

namespace hc::params { class ParameterTree; }

namespace hc::forge {

inline constexpr int kTracks = 8;
inline constexpr int kKnobs  = 5;
enum Track : int { kick = 0, snare, hats, perc, bass, keys, lead, pad };
[[nodiscard]] inline constexpr bool isDrum(int t) noexcept { return t <= perc; }
[[nodiscard]] inline constexpr bool hasType(int t) noexcept { return t >= bass; }

inline constexpr const char* kTrackKeys[kTracks]  = {"kick", "snare", "hats", "perc", "bass", "keys", "lead", "pad"};
inline constexpr const char* kTrackNames[kTracks] = {"KICK", "SNARE", "HATS", "PERC", "BASS", "KEYS", "LEAD", "PAD"};

struct KnobSpec {
    const char* key;
    const char* caption;   // short, engraved under the knob
    float lo, hi, def;
    float centre;          // value at the middle of travel (0 = linear)
    const char* unit;      // "st", "ms", "%"
    const char* shortCap;  // 4 letters at most, for the small knobs
};
// The five sound knobs of each track, left to right (big, big, small, small, small on the strip).
[[nodiscard]] const std::array<KnobSpec, kKnobs>& knobSpecs(int track) noexcept;

// Sound types of the melodic tracks (4 each).
inline constexpr int kTypes = 4;
inline constexpr const char* kTypeNames[kTracks][kTypes] = {
    {}, {}, {}, {},
    {"808", "REESE", "PLUCK", "UPRIGHT"},
    {"E-PIANO", "BELL", "ORGAN", "GUITAR"},
    {"SAW", "SQUARE", "SUPERSAW", "FLUTE"},
    {"WARM", "STRINGS", "CHOIR", "GLASS"}};

inline constexpr int kGenres = 8;
inline constexpr const char* kGenreNames[kGenres] = {"TRAP", "POP TRAP", "DRILL", "BOOM BAP", "OUTLAW COUNTRY", "FOLK", "GOSPEL", "CINEMATIC"};
inline constexpr const char* kKeyNames[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
inline constexpr int kScales = 9;
inline constexpr const char* kScaleNames[kScales] = {"MAJOR", "MINOR", "HARM MINOR", "DORIAN", "MIXOLYDIAN", "PHRYGIAN",
                                                     "MINOR PENTA", "MAJOR PENTA", "BLUES"};
inline constexpr int kBarChoices[4] = {1, 2, 4, 8};
inline constexpr int kDelayChoices = 6;
inline constexpr const char* kDelayNames[kDelayChoices] = {"1/16", "1/8", "1/8 D", "1/4", "1/4 D", "1/2"};
inline constexpr double kDelaySteps[kDelayChoices] = {1, 2, 3, 4, 6, 8};   // in 16ths

// Fixed per-track voice gain (dB) so the console's faders at 0 dB give a balanced beat
// (Hybrid Studio's default track levels).
inline constexpr float kVoiceGainDb[kTracks] = {-1.0f, -1.0f, -5.0f, -7.0f, -3.0f, -9.0f, -10.0f, -13.0f};
// Headroom for the whole beat. Measured before this (TRAP, 6 songs, faders at 0 dB): peaks
// +4.7 .. +5.6 dBFS before the console, so Main's limiter was always working. -6 dB leaves the
// beat peaking around -1 dBFS and the limiter as a safety net only.
inline constexpr float kHeadroomDb = -6.0f;
// Stereo spread per track (0 = centred). The side signal is a delayed, high-passed copy of the
// track's centre, added to L and taken from R: wider on speakers and headphones, and the mono sum
// (phone speaker, club mono) is exactly what it was. Kick and 808 stay centred.
inline constexpr float kSpread[kTracks]    = {0.0f, 0.15f, 0.55f, 0.6f, 0.0f, 0.6f, 0.4f, 0.65f};
inline constexpr float kSpreadMs[kTracks]  = {0.0f, 9.7f, 13.1f, 11.3f, 0.0f, 15.7f, 17.9f, 19.3f};
inline constexpr float kSpreadLowHz        = 250.0f;   // below this the side signal is removed (bass stays centred)
inline constexpr float kRevSendDefault[kTracks] = {0, 12, 0, 15, 0, 25, 20, 35};   // %
inline constexpr float kDlySendDefault[kTracks] = {0, 0, 0, 0, 0, 10, 15, 0};      // %

// ---- parameter ids ("forge.*", added by addForgeParameters) ----
namespace id {
inline constexpr const char* on       = "forge.on";
inline constexpr const char* bpm      = "forge.bpm";
inline constexpr const char* swing    = "forge.swing";
inline constexpr const char* genre    = "forge.genre";
inline constexpr const char* key      = "forge.key";
inline constexpr const char* scale    = "forge.scale";
inline constexpr const char* bars     = "forge.bars";
inline constexpr const char* density  = "forge.density";
inline constexpr const char* variation = "forge.variation";
inline constexpr const char* humanize = "forge.humanize";
inline constexpr const char* setTempo = "forge.settempo";
inline constexpr const char* arrange  = "forge.arrange";
inline constexpr const char* revSize  = "forge.revsize";
inline constexpr const char* revTone  = "forge.revtone";
inline constexpr const char* dlyTime  = "forge.dlytime";
inline constexpr const char* dlyFb    = "forge.dlyfb";
inline constexpr const char* fxLevel  = "forge.fx";
[[nodiscard]] std::string track(int t, const char* what);   // "forge.kick.decay"
[[nodiscard]] std::string knob(int t, int k);
[[nodiscard]] inline std::string lock(int t) { return track(t, "lock"); }
[[nodiscard]] inline std::string type(int t) { return track(t, "type"); }
[[nodiscard]] inline std::string rev(int t) { return track(t, "rev"); }
[[nodiscard]] inline std::string dly(int t) { return track(t, "dly"); }
} // namespace id

// Adds every forge.* parameter (message thread). Separate from the mixer set, so the VST3's
// parameter numbers never change.
void addForgeParameters(params::ParameterTree& tree);

// RESET (put every knob and fader back) leaves the beat itself alone: Beat Forge on / off, tempo,
// swing, genre, key, scale, bars, density / variation / humanize, song form, the sound picked for
// each melodic track, and the track locks. Everything else (mixer, Beat Forge sound knobs and sends) resets.
[[nodiscard]] bool keepOnReset(const std::string& parameterId) noexcept;

} // namespace hc::forge

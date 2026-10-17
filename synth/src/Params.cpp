#include "WaveBank.hpp"
#include "hybrid/synth/Wave.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace hc::synth {

namespace {
const char* const kUnisonNames[] = {"1", "2", "3", "4", "5", "6", "7"};
const char* const kOctaveNames[] = {"-2", "-1", "0", "+1", "+2"};
const char* const kFilterNames[] = {"LOW PASS", "BAND PASS", "HIGH PASS"};
const char* const kLfoNames[]    = {"SINE", "TRIANGLE", "SAW", "SQUARE", "S&H"};
const char* const kModeNames[]   = {"POLY", "MONO", "LEGATO"};

const ParamInfo kParams[kParamCount] = {
    {"table",     "Wavetable",        "",   0, 7, 0, Taper::Choice, detail::kTableNames},
    {"position",  "Position",         "%",  0, 100, 0},
    {"warp",      "Warp",             "%",  -100, 100, 0},
    {"unison",    "Unison",           "",   0, 6, 0, Taper::Choice, kUnisonNames},
    {"detune",    "Detune",           "ct", 0, 100, 15},
    {"spread",    "Stereo Spread",    "%",  0, 100, 60},
    {"sub",       "Sub",              "%",  0, 100, 0},
    {"octave",    "Octave",           "",   0, 4, 2, Taper::Choice, kOctaveNames},
    {"fine",      "Fine",             "ct", -100, 100, 0},
    {"ftype",     "Filter Type",      "",   0, 2, 0, Taper::Choice, kFilterNames},
    {"cutoff",    "Cutoff",           "Hz", 20, 20000, 20000, Taper::Log},
    {"reso",      "Resonance",        "%",  0, 100, 10},
    {"keytrack",  "Key Track",        "%",  0, 100, 50},
    {"drive",     "Filter Drive",     "dB", 0, 24, 0},
    {"envcut",    "Env > Cutoff",     "st", -48, 48, 0},
    {"velcut",    "Velocity > Cutoff","st", 0, 48, 0},
    {"aatt",      "Amp Attack",       "ms", 0.5f, 10000, 2, Taper::Log},
    {"adec",      "Amp Decay",        "ms", 1, 10000, 300, Taper::Log},
    {"asus",      "Amp Sustain",      "%",  0, 100, 100},
    {"arel",      "Amp Release",      "ms", 1, 10000, 200, Taper::Log},
    {"matt",      "Mod Attack",       "ms", 0.5f, 10000, 2, Taper::Log},
    {"mdec",      "Mod Decay",        "ms", 1, 10000, 400, Taper::Log},
    {"msus",      "Mod Sustain",      "%",  0, 100, 0},
    {"mrel",      "Mod Release",      "ms", 1, 10000, 300, Taper::Log},
    {"envpos",    "Env > Position",   "%",  -100, 100, 0},
    {"lfoshape",  "LFO Shape",        "",   0, 4, 0, Taper::Choice, kLfoNames},
    {"lforate",   "LFO Rate",         "Hz", 0.05f, 20, 2, Taper::Log},
    {"lfopos",    "LFO > Position",   "%",  0, 100, 0},
    {"lfocut",    "LFO > Cutoff",     "st", 0, 48, 0},
    {"lfopitch",  "LFO > Pitch",      "ct", 0, 100, 0},
    {"velamp",    "Velocity > Level", "%",  0, 100, 50},
    {"mode",      "Voice Mode",       "",   0, 2, 0, Taper::Choice, kModeNames},
    {"glide",     "Glide",            "ms", 0, 1000, 0},
    {"bend",      "Bend Range",       "st", 0, 24, 2},
    {"volume",    "Volume",           "dB", -48, 6, -6},
};

struct PresetValue { int param; float value; };
struct Preset { const char* name; std::array<PresetValue, 24> values; int count; };

// Each preset starts from the defaults and changes only what is listed.
#define PV(p, v) PresetValue{p, static_cast<float>(v)}
const Preset kPresets[] = {
    {"Init", {}, 0},
    {"Glass Keys", {PV(Table, 5), PV(Position, 22), PV(EnvPosition, 35), PV(ModDecay, 900), PV(ModSustain, 0),
                    PV(AmpAttack, 1), PV(AmpDecay, 1800), PV(AmpSustain, 35), PV(AmpRelease, 600), PV(Cutoff, 9000),
                    PV(VelCutoff, 18), PV(Unison, 1), PV(Detune, 6), PV(Volume, -8)}, 14},
    {"Vocal Pad", {PV(Table, 2), PV(Position, 30), PV(LfoPosition, 35), PV(LfoRate, 0.25), PV(LfoShape, 1),
                   PV(Unison, 4), PV(Detune, 18), PV(Spread, 90), PV(AmpAttack, 900), PV(AmpRelease, 2500),
                   PV(Cutoff, 6000), PV(Resonance, 15), PV(Volume, -6)}, 13},
    {"808 Wave Bass", {PV(Table, 0), PV(Position, 8), PV(Octave, 1), PV(Sub, 70), PV(Mode, 2), PV(Glide, 70),
                       PV(Cutoff, 900), PV(KeyTrack, 30), PV(Drive, 9), PV(EnvCutoff, 14), PV(ModDecay, 250),
                       PV(AmpDecay, 2500), PV(AmpSustain, 60), PV(AmpRelease, 300), PV(Volume, 2)}, 15},
    {"Neon Lead", {PV(Table, 0), PV(Position, 70), PV(Unison, 2), PV(Detune, 12), PV(Spread, 70), PV(Mode, 2),
                   PV(Glide, 45), PV(Cutoff, 7000), PV(Resonance, 25), PV(EnvCutoff, 10), PV(ModDecay, 500),
                   PV(LfoPitch, 18), PV(LfoRate, 5.5), PV(Warp, 30), PV(Volume, 0)}, 15},
    {"Pluck", {PV(Table, 3), PV(Position, 15), PV(EnvPosition, 45), PV(ModDecay, 180), PV(Cutoff, 1200),
               PV(EnvCutoff, 36), PV(Resonance, 30), PV(AmpAttack, 0.5), PV(AmpDecay, 450), PV(AmpSustain, 0),
               PV(AmpRelease, 250), PV(VelCutoff, 12), PV(Unison, 2), PV(Detune, 8), PV(Volume, -1)}, 15},
    {"Dark Choir", {PV(Table, 2), PV(Position, 75), PV(Warp, -55), PV(Unison, 6), PV(Detune, 25), PV(Spread, 100),
                    PV(Octave, 1), PV(AmpAttack, 1500), PV(AmpRelease, 3500), PV(Cutoff, 2500), PV(LfoPosition, 20),
                    PV(LfoRate, 0.15), PV(LfoShape, 1), PV(Volume, -6)}, 14},
    {"Bell Tower", {PV(Table, 5), PV(Position, 60), PV(EnvPosition, -50), PV(ModDecay, 3000), PV(AmpAttack, 0.5),
                    PV(AmpDecay, 6000), PV(AmpSustain, 0), PV(AmpRelease, 4000), PV(Octave, 3), PV(Unison, 1),
                    PV(Detune, 4), PV(Spread, 80), PV(Volume, -8)}, 13},
    {"Hollow Keys", {PV(Table, 7), PV(Position, 40), PV(EnvPosition, 25), PV(ModDecay, 700), PV(AmpDecay, 1200),
                     PV(AmpSustain, 50), PV(AmpRelease, 400), PV(Cutoff, 5000), PV(Volume, -5)}, 9},
    {"Air Texture", {PV(Table, 6), PV(Position, 50), PV(LfoPosition, 60), PV(LfoRate, 0.1), PV(Unison, 6),
                     PV(Detune, 40), PV(Spread, 100), PV(FilterType, 1), PV(Cutoff, 3000), PV(Resonance, 35),
                     PV(LfoCutoff, 18), PV(AmpAttack, 2500), PV(AmpRelease, 4000), PV(Volume, 4)}, 14},
};
#undef PV
} // namespace

float clampPlain(const ParamInfo& p, float v) noexcept {
    if (!(v == v)) v = p.def;
    v = std::clamp(v, p.min, p.max);
    if (p.taper == Taper::Choice || p.taper == Taper::Toggle) v = std::round(v);
    return v;
}

float toPlain(const ParamInfo& p, float n) noexcept {
    n = std::clamp(n, 0.0f, 1.0f);
    switch (p.taper) {
    case Taper::Log:    return clampPlain(p, p.min * std::pow(p.max / p.min, n));
    case Taper::Choice:
    case Taper::Toggle: return clampPlain(p, p.min + std::round(n * (p.max - p.min)));
    default:            return clampPlain(p, p.min + n * (p.max - p.min));
    }
}

float toNormalized(const ParamInfo& p, float v) noexcept {
    v = clampPlain(p, v);
    if (p.max <= p.min) return 0.0f;
    if (p.taper == Taper::Log) return std::log(v / p.min) / std::log(p.max / p.min);
    return (v - p.min) / (p.max - p.min);
}

std::string formatValue(const ParamInfo& p, float v) {
    v = clampPlain(p, v);
    if (p.taper == Taper::Toggle) return v >= 0.5f ? "ON" : "OFF";
    if (p.taper == Taper::Choice) return p.choices ? p.choices[static_cast<int>(v)] : std::to_string(static_cast<int>(v));
    const std::string u = p.unit ? p.unit : "";
    char b[48];
    if (u == "Hz") {
        if (v >= 1000.0f) std::snprintf(b, sizeof b, "%.2f kHz", v / 1000.0f);
        else std::snprintf(b, sizeof b, v < 10.0f ? "%.2f Hz" : "%.0f Hz", v);
    } else if (u == "dB") {
        std::snprintf(b, sizeof b, std::fabs(v) < 0.05f ? "0.0 dB" : "%+.1f dB", v);
    } else if (u == "ms") {
        if (v >= 1000.0f) std::snprintf(b, sizeof b, "%.2f s", v / 1000.0f);
        else std::snprintf(b, sizeof b, v < 10.0f ? "%.2f ms" : v < 100.0f ? "%.1f ms" : "%.0f ms", v);
    } else if (u == "%") {
        std::snprintf(b, sizeof b, "%.0f %%", v);
    } else if (u == "st" || u == "ct") {
        std::snprintf(b, sizeof b, "%+.1f %s", v, u.c_str());
    } else {
        std::snprintf(b, sizeof b, "%.2f", v);
    }
    return b;
}

bool parseValue(const ParamInfo& p, const std::string& s, float& out) {
    if (p.taper == Taper::Toggle) { out = (s == "ON" || s == "on" || s == "1") ? 1.0f : 0.0f; return true; }
    if (p.taper == Taper::Choice && p.choices)
        for (int i = 0; i <= static_cast<int>(p.max); ++i)
            if (s == p.choices[i]) { out = static_cast<float>(i); return true; }
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (*end == ' ') ++end;
    const std::string u = p.unit ? p.unit : "";
    if ((*end == 'k' || *end == 'K') && u == "Hz") v *= 1000.0f;
    if (*end == 's' && u == "ms") v *= 1000.0f;   // "1.20 s"
    out = clampPlain(p, v);
    return true;
}

std::span<const ParamInfo> WaveSynth::params() noexcept { return kParams; }

void WaveSynth::set(int i, float plain) noexcept {
    if (i >= 0 && i < kParamCount) vals_[static_cast<std::size_t>(i)].store(clampPlain(kParams[i], plain), std::memory_order_relaxed);
}
float WaveSynth::get(int i) const noexcept {
    return i >= 0 && i < kParamCount ? vals_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed) : 0.0f;
}
void WaveSynth::setDefaults() noexcept {
    for (int i = 0; i < kParamCount; ++i) vals_[static_cast<std::size_t>(i)].store(kParams[i].def, std::memory_order_relaxed);
}
int WaveSynth::paramIndex(const std::string& id) const noexcept {
    for (int i = 0; i < kParamCount; ++i) if (id == kParams[i].id) return i;
    return -1;
}

int WaveSynth::presetCount() noexcept { return static_cast<int>(std::size(kPresets)); }
const char* WaveSynth::presetName(int i) noexcept { return i >= 0 && i < presetCount() ? kPresets[i].name : ""; }
void WaveSynth::applyPreset(int i) noexcept {
    if (i < 0 || i >= presetCount()) return;
    setDefaults();
    const Preset& p = kPresets[i];
    for (int k = 0; k < p.count; ++k) set(p.values[static_cast<std::size_t>(k)].param, p.values[static_cast<std::size_t>(k)].value);
}

} // namespace hc::synth

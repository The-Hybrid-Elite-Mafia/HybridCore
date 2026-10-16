#include "hybrid/forge/Forge.hpp"
#include "hybrid/forge/Pattern.hpp"
#include "hybrid/params/ParameterTree.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <vector>

namespace hc::forge {

const std::array<KnobSpec, kKnobs>& knobSpecs(int t) noexcept {
    static const std::array<std::array<KnobSpec, kKnobs>, kTracks> specs{{
        {{{"tune", "TUNE", -12, 12, 0, 0, "st", "TUNE"}, {"decay", "DECAY", 50, 1500, 450, 300, "ms", "DCY"}, {"punch", "PUNCH", 0, 100, 55, 0, "%", "PNCH"},
          {"click", "CLICK", 0, 100, 30, 0, "%", "CLIK"}, {"drive", "DRIVE", 0, 100, 20, 0, "%", "DRV"}}},
        {{{"tune", "TUNE", -12, 12, 0, 0, "st", "TUNE"}, {"decay", "DECAY", 40, 800, 240, 160, "ms", "DCY"}, {"tone", "TONE", 0, 100, 55, 0, "%", "TONE"},
          {"snap", "SNAP", 0, 100, 60, 0, "%", "SNAP"}, {"drive", "DRIVE", 0, 100, 10, 0, "%", "DRV"}}},
        {{{"tune", "TUNE", -12, 12, 0, 0, "st", "TUNE"}, {"decay", "CLOSED", 10, 300, 60, 50, "ms", "CLSD"}, {"open", "OPEN", 80, 1500, 380, 350, "ms", "OPEN"},
          {"tone", "TONE", 0, 100, 60, 0, "%", "TONE"}, {"width", "WIDTH", 0, 100, 30, 0, "%", "WIDE"}}},
        {{{"tune", "TUNE", -12, 12, 0, 0, "st", "TUNE"}, {"length", "LENGTH", 25, 400, 100, 100, "%", "LEN"}, {"tone", "TONE", 0, 100, 50, 0, "%", "TONE"},
          {"drive", "DRIVE", 0, 100, 0, 0, "%", "DRV"}, {"width", "WIDTH", 0, 100, 40, 0, "%", "WIDE"}}},
        {{{"decay", "DECAY", 50, 4000, 900, 700, "ms", "DCY"}, {"glide", "GLIDE", 0, 400, 60, 80, "ms", "GLID"}, {"drive", "DRIVE", 0, 100, 35, 0, "%", "DRV"},
          {"tone", "TONE", 0, 100, 55, 0, "%", "TONE"}, {"punch", "PUNCH", 0, 100, 40, 0, "%", "PNCH"}}},
        {{{"tone", "TONE", 0, 100, 50, 0, "%", "TONE"}, {"decay", "DECAY", 100, 6000, 1800, 1200, "ms", "DCY"}, {"release", "REL", 10, 3000, 350, 300, "ms", "REL"},
          {"width", "WIDTH", 0, 100, 40, 0, "%", "WIDE"}, {"drive", "DRIVE", 0, 100, 0, 0, "%", "DRV"}}},
        {{{"cutoff", "CUTOFF", 0, 100, 60, 0, "%", "CUT"}, {"reso", "RESO", 0, 100, 25, 0, "%", "RES"}, {"env", "ENV", 0, 100, 40, 0, "%", "ENV"},
          {"glide", "GLIDE", 0, 400, 30, 80, "ms", "GLID"}, {"release", "REL", 10, 2000, 200, 250, "ms", "REL"}}},
        {{{"cutoff", "CUTOFF", 0, 100, 45, 0, "%", "CUT"}, {"attack", "ATTACK", 5, 4000, 600, 500, "ms", "ATK"}, {"release", "REL", 50, 8000, 1800, 1500, "ms", "REL"},
          {"width", "WIDTH", 0, 100, 70, 0, "%", "WIDE"}, {"motion", "MOTION", 0, 100, 30, 0, "%", "MOVE"}}},
    }};
    return specs[static_cast<std::size_t>(std::clamp(t, 0, kTracks - 1))];
}

namespace id {
std::string track(int t, const char* what) { return std::string("forge.") + kTrackKeys[std::clamp(t, 0, kTracks - 1)] + "." + what; }
std::string knob(int t, int k) { return track(t, knobSpecs(t)[static_cast<std::size_t>(std::clamp(k, 0, kKnobs - 1))].key); }
} // namespace id

namespace {
using params::Kind;
using params::ParamSpec;

ParamSpec make(std::string id, std::string name, Kind kind, float lo, float hi, float def, const char* unit = "") {
    ParamSpec s;
    s.id = std::move(id); s.name = std::move(name); s.kind = kind; s.min = lo; s.max = hi; s.def = def; s.unit = unit;
    return s;
}
ParamSpec choice(std::string id, std::string name, std::vector<std::string> names, int def) {
    ParamSpec s;
    s.id = std::move(id); s.name = std::move(name); s.kind = Kind::Choice; s.choices = std::move(names); s.def = static_cast<float>(def);
    return s;
}
ParamSpec toggle(std::string id, std::string name, bool def) { return make(std::move(id), std::move(name), Kind::Toggle, 0, 1, def ? 1.0f : 0.0f); }
template <std::size_t N>
std::vector<std::string> names(const char* const (&a)[N]) { return std::vector<std::string>(std::begin(a), std::end(a)); }
} // namespace

void addForgeParameters(params::ParameterTree& tree) {
    tree.add(toggle(id::on, "Beat Forge On", false));
    {
        ParamSpec bpm = make(id::bpm, "Beat Forge Tempo", Kind::Linear, 60, 200, 140, "bpm");
        tree.add(bpm);
    }
    tree.add(make(id::swing, "Beat Forge Swing", Kind::Linear, 50, 75, 50, "%"));
    tree.add(choice(id::genre, "Beat Forge Genre", names(kGenreNames), 0));
    tree.add(choice(id::key, "Beat Forge Key", names(kKeyNames), 9));      // A
    tree.add(choice(id::scale, "Beat Forge Scale", names(kScaleNames), 1)); // minor
    tree.add(choice(id::bars, "Beat Forge Bars", {"1 BAR", "2 BARS", "4 BARS", "8 BARS"}, 2));
    tree.add(make(id::density, "Beat Forge Density", Kind::Linear, 0, 100, 55, "%"));
    tree.add(make(id::variation, "Beat Forge Variation", Kind::Linear, 0, 100, 40, "%"));
    tree.add(make(id::humanize, "Beat Forge Humanize", Kind::Linear, 0, 100, 30, "%"));
    tree.add(toggle(id::setTempo, "Beat Forge Genre Tempo", true));
    tree.add(toggle(id::arrange, "Beat Forge Full Song", true));
    tree.add(make(id::revSize, "Beat Forge Reverb Size", Kind::Linear, 0, 100, 55, "%"));
    tree.add(make(id::revTone, "Beat Forge Reverb Tone", Kind::Linear, 0, 100, 50, "%"));
    tree.add(choice(id::dlyTime, "Beat Forge Delay Time", names(kDelayNames), 2));
    tree.add(make(id::dlyFb, "Beat Forge Delay Feedback", Kind::Linear, 0, 95, 35, "%"));
    {
        ParamSpec fx = make(id::fxLevel, "Beat Forge FX Return", Kind::Decibel, -60, 6, 0, "dB");
        fx.minIsOff = true; fx.anchorPlain = 0.0f; fx.anchorNorm = 0.75f;
        tree.add(fx);
    }
    for (int t = 0; t < kTracks; ++t) {
        const std::string n = std::string("Beat Forge ") + kTrackNames[t] + " ";
        tree.add(toggle(id::lock(t), n + "Lock", false));
        if (hasType(t)) {
            std::vector<std::string> types(std::begin(kTypeNames[t]), std::end(kTypeNames[t]));
            tree.add(choice(id::type(t), n + "Sound", std::move(types), 0));
        }
        for (int k = 0; k < kKnobs; ++k) {
            const KnobSpec& s = knobSpecs(t)[static_cast<std::size_t>(k)];
            ParamSpec p = make(id::knob(t, k), n + s.caption, Kind::Linear, s.lo, s.hi, s.def, s.unit);
            if (s.centre > 0.0f) { p.anchorPlain = s.centre; p.anchorNorm = 0.5f; }
            tree.add(p);
        }
        tree.add(make(id::rev(t), n + "Reverb Send", Kind::Linear, 0, 100, kRevSendDefault[t], "%"));
        tree.add(make(id::dly(t), n + "Delay Send", Kind::Linear, 0, 100, kDlySendDefault[t], "%"));
    }
}

// ---- song text ----
namespace {
bool parseFloat(std::string_view s, float& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && std::isfinite(v);
}
bool parseInt(std::string_view s, int& v) {
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc();
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

std::string songToText(const Song& song) {
    std::string out = "bars=" + std::to_string(song.pattern.bars) + "\n";
    char b[64];
    for (int t = 0; t < kTracks; ++t) {
        out += kTrackKeys[t];
        out += '=';
        const TrackNotes& tn = song.pattern.tracks[static_cast<std::size_t>(t)];
        for (int i = 0; i < tn.count; ++i) {
            const Note& n = tn.notes[static_cast<std::size_t>(i)];
            std::snprintf(b, sizeof b, "%.4f,%.4f,%d,%d;", static_cast<double>(n.start), static_cast<double>(n.length), n.pitch, n.velocity);
            out += b;
        }
        out += '\n';
    }
    const Arrangement& a = song.arrangement;
    out += "arr.bars=" + std::to_string(a.bars) + "\n";
    if (a.bars > 0) {
        out += "arr.mask=";
        for (int i = 0; i < a.bars; ++i) { std::snprintf(b, sizeof b, "%02x", a.mask[static_cast<std::size_t>(i)]); out += b; }
        out += "\narr.sect=";
        for (int i = 0; i < a.bars; ++i) out += static_cast<char>('0' + a.section[static_cast<std::size_t>(i)]);
        out += '\n';
    }
    return out;
}

bool songFromText(std::string_view text, Song& out) {
    out.pattern.clear();
    out.pattern.bars = 4;
    out.arrangement = Arrangement{};
    bool any = false;
    std::string_view mask, sect;
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        int iv = 0;
        if (k == "bars" && parseInt(v, iv)) { out.pattern.bars = std::clamp(iv, 1, kMaxBars); any = true; continue; }
        if (k == "arr.bars" && parseInt(v, iv)) { out.arrangement.bars = std::clamp(iv, 0, kMaxSongBars); continue; }
        if (k == "arr.mask") { mask = v; continue; }
        if (k == "arr.sect") { sect = v; continue; }
        for (int t = 0; t < kTracks; ++t) {
            if (k != kTrackKeys[t]) continue;
            any = true;
            TrackNotes& tn = out.pattern.tracks[static_cast<std::size_t>(t)];
            std::string_view rest = v;
            while (!rest.empty()) {
                const auto sc = rest.find(';');
                const std::string_view item = rest.substr(0, sc);
                rest.remove_prefix(sc == std::string_view::npos ? rest.size() : sc + 1);
                float f[2] = {};
                int p = 0, vel = 0, field = 0;
                bool ok = true;
                std::string_view r = item;
                while (ok && field < 4) {
                    const auto c = r.find(',');
                    const std::string_view part = trim(r.substr(0, c));
                    ok = field < 2 ? parseFloat(part, f[field]) : parseInt(part, field == 2 ? p : vel);
                    ++field;
                    if (c == std::string_view::npos) break;
                    r.remove_prefix(c + 1);
                }
                if (!ok || field != 4) continue;
                Note n;
                n.start    = std::clamp(f[0], 0.0f, static_cast<float>(kMaxSteps));
                n.length   = std::clamp(f[1], 0.05f, static_cast<float>(kMaxSteps));
                n.pitch    = static_cast<std::uint8_t>(std::clamp(p, 0, 127));
                n.velocity = static_cast<std::uint8_t>(std::clamp(vel, 1, 127));
                tn.add(n);
            }
            tn.sort();
        }
    }
    Arrangement& a = out.arrangement;
    if (a.bars > 0 && mask.size() >= static_cast<std::size_t>(2 * a.bars)) {
        for (int i = 0; i < a.bars; ++i) {
            const int hi = hexDigit(mask[static_cast<std::size_t>(2 * i)]), lo = hexDigit(mask[static_cast<std::size_t>(2 * i + 1)]);
            a.mask[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(hi < 0 || lo < 0 ? 0xFF : hi * 16 + lo);
            const int sc = i < static_cast<int>(sect.size()) ? sect[static_cast<std::size_t>(i)] - '0' : kVerse;
            a.section[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::clamp(sc, 0, kSections - 1));
        }
    } else {
        a.bars = 0;
    }
    return any;
}

} // namespace hc::forge

#include "hybrid/params/MixerParams.hpp"

#include <cstdio>
#include <cstring>

namespace hc::params {

namespace {
constexpr std::uint32_t kStems = dsp::StemMatrix::kStems;
constexpr std::uint32_t kBuses = dsp::StemMatrix::kBuses;
constexpr const char*   kBusIds[]   = {"main", "aux1", "aux2", "aux3"};
constexpr const char*   kBusNames[] = {"Main", "Aux 1", "Aux 2", "Aux 3"};
static_assert(sizeof(kBusIds) / sizeof(kBusIds[0]) == kBuses, "bus names must match kBusCount");

ParamSpec fader(std::string id, std::string name, float def) {
    ParamSpec s;
    s.id = std::move(id); s.name = std::move(name);
    s.kind = Kind::Decibel; s.min = -60.0f; s.max = 6.0f; s.def = def;
    s.minIsOff = true; s.anchorPlain = 0.0f; s.anchorNorm = 0.75f;   // unity at 3/4 travel
    return s;
}
ParamSpec toggle(std::string id, std::string name) {
    ParamSpec s;
    s.id = std::move(id); s.name = std::move(name);
    s.kind = Kind::Toggle; s.min = 0.0f; s.max = 1.0f; s.def = 0.0f;
    return s;
}

// Parse "stemNN.field" / "<bus>.field". Returns false if not a mixer id.
bool parseStem(const std::string& id, std::uint32_t& stem, std::string& field) {
    if (id.size() < 8 || id.compare(0, 4, "stem") != 0 || id[6] != '.') return false;
    stem  = static_cast<std::uint32_t>((id[4] - '0') * 10 + (id[5] - '0')) - 1;
    field = id.substr(7);
    return stem < kStems;
}
bool parseBus(const std::string& id, std::uint32_t& bus, std::string& field) {
    for (std::uint32_t b = 0; b < kBuses; ++b) {
        const std::size_t n = std::strlen(kBusIds[b]);
        if (id.size() > n && id.compare(0, n, kBusIds[b]) == 0 && id[n] == '.') {
            bus = b; field = id.substr(n + 1); return true;
        }
    }
    return false;
}
} // namespace

std::string stemParamId(std::uint32_t stem, const char* field) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "stem%02u.%s", stem + 1, field);
    return buf;
}
std::string busParamId(std::uint32_t bus, const char* field) { return std::string(kBusIds[bus]) + "." + field; }
const char* busName(std::uint32_t bus) { return bus < kBuses ? kBusNames[bus] : "?"; }

void addMixerParameters(ParameterTree& tree) {
    for (std::uint32_t s = 0; s < kStems; ++s) {
        const std::string n = "Stem " + std::to_string(s + 1) + " ";
        ParamSpec trim;
        trim.id = stemParamId(s, "trim"); trim.name = n + "Trim";
        trim.kind = Kind::Decibel; trim.min = -24.0f; trim.max = 24.0f; trim.def = 0.0f;
        tree.add(trim);

        ParamSpec pan;
        pan.id = stemParamId(s, "pan"); pan.name = n + "Pan";
        pan.kind = Kind::Pan; pan.min = -1.0f; pan.max = 1.0f; pan.def = 0.0f;
        tree.add(pan);

        tree.add(fader(stemParamId(s, "main"), n + "Main", 0.0f));
        for (std::uint32_t b = 1; b < kBuses; ++b)
            tree.add(fader(stemParamId(s, kBusIds[b]), n + kBusNames[b] + " Send", -60.0f));
        tree.add(toggle(stemParamId(s, "mute"), n + "Mute"));
        tree.add(toggle(stemParamId(s, "solo"), n + "Solo"));
        tree.add(toggle(stemParamId(s, "invert"), n + "Phase Invert"));
    }
    for (std::uint32_t b = 0; b < kBuses; ++b) {
        tree.add(fader(busParamId(b, "gain"), std::string(kBusNames[b]) + " Level", 0.0f));
        tree.add(toggle(busParamId(b, "mute"), std::string(kBusNames[b]) + " Mute"));
    }

    // ---- 0.8.0: channel strips + main limiter (appended: earlier indices stay put) ----
    auto lin = [](std::string id, std::string name, float lo, float hi, float def, const char* unit) {
        ParamSpec s;
        s.id = std::move(id); s.name = std::move(name);
        s.kind = Kind::Linear; s.min = lo; s.max = hi; s.def = def; s.unit = unit;
        return s;
    };
    auto lg = [](std::string id, std::string name, float lo, float hi, float def, const char* unit, bool off = false) {
        ParamSpec s;
        s.id = std::move(id); s.name = std::move(name);
        s.kind = Kind::Log; s.min = lo; s.max = hi; s.def = def; s.unit = unit; s.offLabel = off;
        return s;
    };
    for (std::uint32_t s = 0; s < kStems; ++s) {
        const std::string n = "Stem " + std::to_string(s + 1) + " ";
        tree.add(lg(stemParamId(s, "hpf"), n + "EQ Low Cut", dsp::ChannelStrip::kHpfOffHz, 1000.0f, dsp::ChannelStrip::kHpfOffHz, "Hz", true));
        tree.add(lin(stemParamId(s, "lo"), n + "EQ Low", -15.0f, 15.0f, 0.0f, "dB"));
        tree.add(lin(stemParamId(s, "mid"), n + "EQ Mid", -15.0f, 15.0f, 0.0f, "dB"));
        tree.add(lg(stemParamId(s, "midf"), n + "EQ Mid Freq", 150.0f, 8000.0f, 1000.0f, "Hz"));
        tree.add(lin(stemParamId(s, "hi"), n + "EQ High", -15.0f, 15.0f, 0.0f, "dB"));
        tree.add(lin(stemParamId(s, "thr"), n + "Comp Threshold", -48.0f, 0.0f, -18.0f, "dB"));
        tree.add(lg(stemParamId(s, "ratio"), n + "Comp Ratio", 1.0f, 20.0f, 1.0f, ":1", true));
        tree.add(lg(stemParamId(s, "att"), n + "Comp Attack", 0.1f, 100.0f, 10.0f, "ms"));
        tree.add(lg(stemParamId(s, "rel"), n + "Comp Release", 10.0f, 2000.0f, 150.0f, "ms"));
        tree.add(lin(stemParamId(s, "mkup"), n + "Comp Makeup", 0.0f, 24.0f, 0.0f, "dB"));
    }
    ParamSpec limit = toggle(busParamId(0, "limit"), "Main Limiter");
    limit.def = 1.0f;
    tree.add(limit);
    tree.add(lin(busParamId(0, "ceil"), "Main Limiter Ceiling", -12.0f, 0.0f, -0.3f, "dB"));
}

MixerBindings::MixerBindings(const ParameterTree& tree) : table_(tree.size()) {
    for (std::size_t i = 0; i < tree.size(); ++i) {
        const std::string& id = tree.at(i).spec().id;
        std::uint32_t idx = 0;
        std::string   field;
        Entry e;
        if (parseStem(id, idx, field)) {
            e.index = static_cast<std::uint8_t>(idx);
            if (field == "trim")        e.target = Target::Trim;
            else if (field == "pan")    e.target = Target::Pan;
            else if (field == "main")   { e.target = Target::Send; e.bus = 0; }
            else if (field == "mute")   e.target = Target::Mute;
            else if (field == "solo")   e.target = Target::Solo;
            else if (field == "invert") e.target = Target::Invert;
            else if (field == "hpf")    e.target = Target::EqHpf;
            else if (field == "lo")     e.target = Target::EqLow;
            else if (field == "mid")    e.target = Target::EqMid;
            else if (field == "midf")   e.target = Target::EqMidHz;
            else if (field == "hi")     e.target = Target::EqHigh;
            else if (field == "thr")    e.target = Target::CompThreshold;
            else if (field == "ratio")  e.target = Target::CompRatio;
            else if (field == "att")    e.target = Target::CompAttack;
            else if (field == "rel")    e.target = Target::CompRelease;
            else if (field == "mkup")   e.target = Target::CompMakeup;
            else
                for (std::uint32_t b = 1; b < kBuses; ++b)
                    if (field == kBusIds[b]) { e.target = Target::Send; e.bus = static_cast<std::uint8_t>(b); }
        } else if (parseBus(id, idx, field)) {
            e.index = static_cast<std::uint8_t>(idx);
            if (field == "gain")      e.target = Target::BusGain;
            else if (field == "mute") e.target = Target::BusMute;
            else if (field == "limit" && idx == 0) e.target = Target::LimitOn;
            else if (field == "ceil" && idx == 0)  e.target = Target::LimitCeiling;
        }
        table_[i] = e;
    }
}

void MixerBindings::apply(const Parameter& p, dsp::StemMatrix& m, dsp::BusLimiter* lim) const noexcept {
    if (p.index() >= table_.size()) return;
    const Entry& e = table_[p.index()];
    switch (e.target) {
    case Target::Trim:    m.setStemTrimDb(e.index, p.plain()); break;
    case Target::Pan:     m.setStemBalance(e.index, p.plain()); break;
    case Target::Send:    m.setSendDb(e.index, e.bus, p.effectiveDb()); break;
    case Target::Mute:    m.setStemMute(e.index, p.isOn()); break;
    case Target::Solo:    m.setStemSolo(e.index, p.isOn()); break;
    case Target::Invert:  m.setStemInvert(e.index, p.isOn()); break;
    case Target::BusGain: m.setBusGainDb(e.index, p.effectiveDb()); break;
    case Target::BusMute: m.setBusMute(e.index, p.isOn()); break;
    case Target::EqHpf:         m.strip(e.index).setHpfHz(p.plain()); break;
    case Target::EqLow:         m.strip(e.index).setLowDb(p.plain()); break;
    case Target::EqMid:         m.strip(e.index).setMidDb(p.plain()); break;
    case Target::EqMidHz:       m.strip(e.index).setMidHz(p.plain()); break;
    case Target::EqHigh:        m.strip(e.index).setHighDb(p.plain()); break;
    case Target::CompThreshold: m.strip(e.index).setThresholdDb(p.plain()); break;
    case Target::CompRatio:     m.strip(e.index).setRatio(p.plain()); break;
    case Target::CompAttack:    m.strip(e.index).setAttackMs(p.plain()); break;
    case Target::CompRelease:   m.strip(e.index).setReleaseMs(p.plain()); break;
    case Target::CompMakeup:    m.strip(e.index).setMakeupDb(p.plain()); break;
    case Target::LimitOn:       if (lim) lim->setEnabled(p.isOn()); break;
    case Target::LimitCeiling:  if (lim) lim->setCeilingDb(p.plain()); break;
    case Target::None:    break;
    }
}

void MixerBindings::applyAll(const ParameterTree& tree, dsp::StemMatrix& m, dsp::BusLimiter* lim) const noexcept {
    for (std::size_t i = 0; i < tree.size(); ++i) apply(tree.at(i), m, lim);
}

void bindMixerParameters(ParameterTree& tree, const MixerBindings& bindings, dsp::StemMatrix& matrix, dsp::BusLimiter* limiter) {
    for (std::size_t i = 0; i < tree.size(); ++i)
        tree.bind(tree.at(i), [&bindings, &matrix, limiter](const Parameter& p) { bindings.apply(p, matrix, limiter); });
}

} // namespace hc::params

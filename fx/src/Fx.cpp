#include "hybrid/fx/Fx.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hc::fx {

float clampPlain(const ParamInfo& p, float v) noexcept {
    if (!(v == v)) v = p.def;   // NaN
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
    char b[48];
    const std::string u = p.unit ? p.unit : "";
    if (p.taper == Taper::Toggle) return v >= 0.5f ? "ON" : "OFF";
    if (p.taper == Taper::Choice) {
        const int i = static_cast<int>(v);
        return p.choices ? p.choices[i] : std::to_string(i);
    }
    if (u == "Hz") {
        if (v >= 1000.0f) std::snprintf(b, sizeof b, "%.2f kHz", v / 1000.0f);
        else std::snprintf(b, sizeof b, "%.0f Hz", v);
    } else if (u == "dB") {
        std::snprintf(b, sizeof b, "%+.1f dB", v);
        if (std::fabs(v) < 0.05f) std::snprintf(b, sizeof b, "0.0 dB");
    } else if (u == "dBr") {   // an amount of reduction: shown as how far it turns down
        std::snprintf(b, sizeof b, "-%.1f dB", std::fabs(v));
        if (std::fabs(v) < 0.05f) std::snprintf(b, sizeof b, "0.0 dB");
    } else if (u == "/10") {   // a 0..10 dial (HEM-Elite-Rack faceplates)
        std::snprintf(b, sizeof b, "%.1f", v);
    } else if (u == "bit") {
        std::snprintf(b, sizeof b, "%.1f bit", v);
    } else if (u == "ms") {
        std::snprintf(b, sizeof b, v < 10.0f ? "%.2f ms" : v < 100.0f ? "%.1f ms" : "%.0f ms", v);
    } else if (u == "s") {
        std::snprintf(b, sizeof b, "%.2f s", v);
    } else if (u == "%") {
        std::snprintf(b, sizeof b, "%.0f %%", v);
    } else if (u == ":1") {
        std::snprintf(b, sizeof b, "%.1f:1", v);
    } else if (u.empty()) {
        std::snprintf(b, sizeof b, "%.2f", v);
    } else {
        std::snprintf(b, sizeof b, "%.2f %s", v, u.c_str());
    }
    return b;
}

bool parseValue(const ParamInfo& p, const std::string& s, float& out) {
    if (p.taper == Taper::Toggle) {
        out = (s == "ON" || s == "on" || s == "1") ? 1.0f : 0.0f;
        return true;
    }
    if (p.taper == Taper::Choice && p.choices) {
        for (int i = 0; i <= static_cast<int>(p.max); ++i)
            if (s == p.choices[i]) { out = static_cast<float>(i); return true; }
    }
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str()) return false;
    while (*end == ' ') ++end;
    if ((*end == 'k' || *end == 'K') && p.unit && std::string(p.unit) == "Hz") v *= 1000.0f;
    if (p.unit && std::string(p.unit) == "dBr") v = std::fabs(v);   // "-60 dB" and "60" mean the same reduction
    out = clampPlain(p, v);
    return true;
}

} // namespace hc::fx

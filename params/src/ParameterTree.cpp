#include "hybrid/params/ParameterTree.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace hc::params {

namespace {
constexpr float kOffDb = -144.0f;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
} // namespace

Parameter::Parameter(ParamSpec spec, std::uint32_t index) : spec_(std::move(spec)), index_(index) {
    if (!(spec_.max > spec_.min)) throw std::invalid_argument("parameter range must be max > min: " + spec_.id);
    if (spec_.kind == Kind::Toggle) { spec_.min = 0.0f; spec_.max = 1.0f; }
    if (spec_.anchorNorm > 0.0f && spec_.anchorNorm < 1.0f && spec_.anchorPlain > spec_.min && spec_.anchorPlain < spec_.max) {
        const float t = (spec_.anchorPlain - spec_.min) / (spec_.max - spec_.min);
        taperExp_ = std::log(t) / std::log(spec_.anchorNorm);
    }
    value_.store(clampPlain(spec_.def), std::memory_order_relaxed);
}

float Parameter::clampPlain(float v) const noexcept {
    if (!std::isfinite(v)) v = (v < 0.0f) ? spec_.min : spec_.def;
    v = std::clamp(v, spec_.min, spec_.max);
    if (spec_.kind == Kind::Toggle) v = v >= 0.5f ? 1.0f : 0.0f;
    return v;
}

bool Parameter::isOff() const noexcept {
    return spec_.kind == Kind::Decibel && spec_.minIsOff && plain() <= spec_.min;
}

float Parameter::effectiveDb() const noexcept { return isOff() ? kOffDb : plain(); }

void Parameter::setPlain(float v) noexcept {
    const float c   = clampPlain(v);
    const float old = value_.exchange(c, std::memory_order_relaxed);
    if (old != c) {
        dirty_.store(true, std::memory_order_release);
        if (treeVersion_) treeVersion_->fetch_add(1, std::memory_order_acq_rel);
    }
}

float Parameter::toNormalized(float p) const noexcept {
    const float t = (clampPlain(p) - spec_.min) / (spec_.max - spec_.min);
    return taperExp_ == 1.0f ? t : std::pow(t, 1.0f / taperExp_);
}

float Parameter::toPlain(float n) const noexcept {
    if (!std::isfinite(n)) n = 0.0f;
    n = std::clamp(n, 0.0f, 1.0f);
    const float t = taperExp_ == 1.0f ? n : std::pow(n, taperExp_);
    return clampPlain(spec_.min + t * (spec_.max - spec_.min));
}

std::string Parameter::formatValue(float v) const {
    char buf[32];
    switch (spec_.kind) {
    case Kind::Toggle:
        return v >= 0.5f ? "ON" : "OFF";
    case Kind::Pan: {
        const int pct = static_cast<int>(std::lround(std::fabs(v) * 100.0f));
        if (pct == 0) return "C";
        std::snprintf(buf, sizeof(buf), "%c%d", v < 0.0f ? 'L' : 'R', pct);
        return buf;
    }
    case Kind::Decibel:
        if (spec_.minIsOff && v <= spec_.min) return "-inf";
        std::snprintf(buf, sizeof(buf), "%+.1f dB", static_cast<double>(v));
        return buf;
    case Kind::Linear:
    default:
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
        return buf;
    }
}

Parameter& ParameterTree::add(ParamSpec spec) {
    if (find(spec.id)) throw std::invalid_argument("duplicate parameter id: " + spec.id);
    params_.push_back(std::make_unique<Parameter>(std::move(spec), static_cast<std::uint32_t>(params_.size())));
    listeners_.emplace_back();
    params_.back()->treeVersion_ = &version_;
    return *params_.back();
}

void ParameterTree::bind(Parameter& p, Listener listener) {
    listeners_.at(p.index()).push_back(std::move(listener));
}

Parameter* ParameterTree::find(std::string_view id) noexcept {
    for (auto& p : params_) if (p->spec().id == id) return p.get();
    return nullptr;
}

const Parameter* ParameterTree::find(std::string_view id) const noexcept {
    for (const auto& p : params_) if (p->spec().id == id) return p.get();
    return nullptr;
}

std::size_t ParameterTree::dispatchChanges() {
    std::size_t n = 0;
    for (std::size_t i = 0; i < params_.size(); ++i) {
        if (!params_[i]->takeDirty()) continue;
        ++n;
        for (const auto& l : listeners_[i]) l(*params_[i]);
    }
    return n;
}

std::string ParameterTree::serialize() const {
    std::string out;
    char buf[64];
    for (const auto& p : params_) {
        std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(p->plain()));
        out += p->spec().id;
        out += '=';
        out += buf;
        out += '\n';
    }
    return out;
}

std::size_t ParameterTree::deserialize(std::string_view text) {
    std::size_t applied = 0;
    while (!text.empty()) {
        const auto nl   = text.find('\n');
        std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const auto eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        Parameter* p = find(trim(line.substr(0, eq)));
        if (!p) continue;
        const std::string valueText(trim(line.substr(eq + 1)));
        char* end = nullptr;
        const float v = std::strtof(valueText.c_str(), &end);
        if (end == valueText.c_str()) continue;
        p->setPlain(v);
        ++applied;
    }
    return applied;
}

} // namespace hc::params

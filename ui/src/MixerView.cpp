#include "hybrid/ui/MixerView.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::ui {

namespace {
constexpr float kKnobMin = -0.75f * std::numbers::pi_v<float>;   // -135°
constexpr float kKnobMax = 0.75f * std::numbers::pi_v<float>;    // +135°

Rgba rgba(Rgb c, float a = 1.0f) { return {c.r, c.g, c.b, a}; }
Rgba mixc(Rgb a, Rgb b, float t, float alpha = 1.0f) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, alpha};
}
float lum(Rgb c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

// Shortens text with "..." until it fits maxWidth design units.
std::string fit(const Renderer& r, TextSize size, std::string s, float maxWidth) {
    if (r.textWidth(size, s) <= maxWidth) return s;
    while (s.size() > 4 && r.textWidth(size, s + "...") > maxWidth) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();   // keep UTF-8 whole
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
    }
    return s + "...";
}

void decay(float& level, float& hold, float& age, float in, float dt) {
    const float fall = std::pow(10.0f, -24.0f * dt / 20.0f);   // 24 dB/s
    level = std::max(in, level * fall);
    if (in >= hold) { hold = in; age = 0.0f; }
    else if ((age += dt) > 1.5f) hold = std::max(level, hold * fall);
}
} // namespace

void MeterState::update(const float* sp, const float* bl, const float* br, float dt) noexcept {
    for (std::size_t i = 0; i < stem.size(); ++i) decay(stem[i], stemHold[i], stemHoldAge_[i], sp[i], dt);
    for (std::size_t i = 0; i < busL.size(); ++i) {
        decay(busL[i], busHoldL[i], busHoldAgeL_[i], bl[i], dt);
        decay(busR[i], busHoldR[i], busHoldAgeR_[i], br[i], dt);
    }
}

float MixerView::levelToNorm(float linear) noexcept {
    if (!(linear > 0.0f)) return 0.0f;
    const float db = 20.0f * std::log10(linear);
    if (db <= -60.0f) return 0.0f;
    const float t = std::min((db + 60.0f) / 66.0f, 1.0f);
    static const float k = std::log(60.0f / 66.0f) / std::log(0.75f);   // same taper as faders
    return std::pow(t, 1.0f / k);
}

void MixerView::drawKnob(Renderer& r, const Palette& p, const Widget& w, bool hot) const {
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const float cx = w.rect.cx(), cy = w.rect.cy();
    const float R  = w.rect.w * 0.5f;
    const float n  = prm.normalized();
    const float ang = kKnobMin + (kKnobMax - kKnobMin) * n;
    const auto& sp = prm.spec();
    const bool  bipolar = sp.kind == params::Kind::Pan ||
                         (sp.kind == params::Kind::Decibel && !sp.minIsOff && sp.min < 0.0f && sp.max > 0.0f);
    const float centreN = bipolar ? prm.toNormalized(0.0f) : 0.0f;
    const float a0 = kKnobMin + (kKnobMax - kKnobMin) * centreN;

    // Track ring + value ring (neon) + glow.
    r.arc(cx, cy, R + 4.0f, 2.5f, kKnobMin, kKnobMax, rgba(p.groove, 0.55f));
    if (std::fabs(ang - a0) > 0.01f) {
        r.arc(cx, cy, R + 4.0f, 2.5f, std::min(a0, ang), std::max(a0, ang), rgba(p.neon));
        r.glow({cx - 1, cy - 1, 2, 2}, R * 0.9f, rgba(p.neon, 0.10f + 0.10f * n));
    }
    // Body with rim and light/shade.
    r.circle(cx, cy + 1.5f, R, {0, 0, 0, 0.30f});
    r.circle(cx, cy, R, rgba(p.knobRim), 0.0f);
    r.circle(cx, cy, R - 2.0f, rgba(p.knobBody), 0.22f);
    r.circle(cx, cy, R * 0.55f, mixc(p.knobBody, {1, 1, 1}, 0.15f), 0.10f);
    // Indicator.
    const float ix = cx + std::sin(ang) * R * 0.55f, iy = cy - std::cos(ang) * R * 0.55f;
    r.roundRectRotated(ix, iy, 3.0f, R * 0.62f, 1.5f, ang, rgba(p.indicator));
    if (hot) r.arc(cx, cy, R + 7.5f, 1.2f, -std::numbers::pi_v<float>, std::numbers::pi_v<float>, rgba(p.neon, 0.6f));

    // Caption or live value.
    const bool small = w.kind == WidgetKind::SmallKnob;
    const std::string label = hot ? prm.format() : w.caption;
    r.text(TextSize::Small, label, cx, w.rect.y + w.rect.h + (small ? 14.0f : 18.0f), rgba(hot ? p.text : p.textDim), Align::Center);
}

void MixerView::drawButton(Renderer& r, const Palette& p, const Widget& w, bool hot) const {
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const bool on = prm.isOn();
    Rgb lit = p.neon;
    if (w.caption == "M") lit = p.meterHigh;
    else if (w.caption == "S") lit = p.meterMid;
    if (on) r.glow(w.rect, 6.0f, rgba(lit, 0.55f));
    r.roundRect({w.rect.x, w.rect.y + 1, w.rect.w, w.rect.h}, 5.0f, {0, 0, 0, 0.30f});
    r.roundRect(w.rect, 5.0f, on ? rgba(lit) : rgba(p.buttonOff));
    r.roundRect({w.rect.x + 1.5f, w.rect.y + 1.5f, w.rect.w - 3, w.rect.h * 0.45f}, 4.0f, {1, 1, 1, on ? 0.25f : 0.10f});
    const Rgb txt = on ? (lum(lit) > 0.5f ? Rgb{0.05f, 0.05f, 0.07f} : Rgb{1, 1, 1}) : (hot ? p.text : p.textDim);
    r.text(TextSize::Medium, w.caption, w.rect.cx(), w.rect.cy() + 5.5f, rgba(txt), Align::Center);
}

void MixerView::drawFader(Renderer& r, const Palette& p, const Widget& w, const StripLayout& st, bool hot) const {
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const Rect  t  = st.faderTrack;
    const float n  = prm.normalized();
    const float cy = t.y + t.h * (1.0f - n);
    const float cx = t.cx();
    // Lit portion of the slot.
    if (!prm.isOff()) {
        r.roundRect({t.x + 2, cy, t.w - 4, t.y + t.h - cy}, 2.0f, rgba(p.neon, 0.85f));
        r.glow({t.x + 2, cy, t.w - 4, t.y + t.h - cy}, 5.0f, rgba(p.neon, 0.18f));
    }
    // Cap.
    const Rect cap{cx - 17, cy - 10, 34, 20};
    r.roundRect({cap.x, cap.y + 2, cap.w, cap.h}, 4.0f, {0, 0, 0, 0.35f});
    r.roundRect(cap, 4.0f, rgba(p.knobRim));
    r.roundRect({cap.x + 1.5f, cap.y + 1.5f, cap.w - 3, cap.h - 3}, 3.0f, rgba(p.knobBody));
    r.roundRect({cap.x + 1.5f, cap.y + 1.5f, cap.w - 3, cap.h * 0.4f}, 3.0f, {1, 1, 1, 0.18f});
    r.rect({cap.x + 4, cy - 0.75f, cap.w - 8, 1.5f}, rgba(hot ? p.neon : p.indicator));
    if (hot) r.glow(cap, 6.0f, rgba(p.neon, 0.35f));
    // Value readout under the strip.
    r.text(TextSize::Small, prm.format(), st.panel.cx(), st.panel.y + st.panel.h - 14.0f, rgba(hot ? p.text : p.textDim), Align::Center);
}

void MixerView::drawMeter(Renderer& r, const Palette& p, Rect m, float level, float hold) const {
    const float n = levelToNorm(level);
    const float nm = levelToNorm(0.25119f);   // -12 dBFS
    const float nh = levelToNorm(0.70795f);   // -3 dBFS
    auto seg = [&](float from, float to, Rgb c) {
        if (n <= from) return;
        const float top = std::min(n, to);
        const Rect s{m.x + 1, m.y + m.h * (1.0f - top) , m.w - 2, m.h * (top - from)};
        r.rect(s, rgba(c));
        r.glow(s, 4.0f, rgba(c, 0.22f));
    };
    seg(0.0f, nm, p.meterLow);
    seg(nm, nh, p.meterMid);
    seg(nh, 1.0f, p.meterHigh);
    const float hn = levelToNorm(hold);
    if (hn > 0.0f) {
        const Rgb hc = hn >= nh ? p.meterHigh : (hn >= nm ? p.meterMid : p.meterLow);
        r.rect({m.x + 1, m.y + m.h * (1.0f - hn) - 1, m.w - 2, 2}, rgba(hc));
    }
}

void MixerView::draw(Renderer& r, const ViewState& v, const MeterState& meters) const {
    const Palette& p = palette(v.theme);
    r.faceplate({0, 0, kDesignWidth, kDesignHeight});

    // Header.
    r.text(TextSize::Large, "THE HYBRID CORE", 30, 42, rgba(p.text));
    r.text(TextSize::Small, "HYBRID ELITE MAFIA  \xC2\xB7  13-STEM MATRIX", 32, 62, rgba(p.neon));
    float statusRight = kDesignWidth;
    for (const auto& w : layout_.widgets())
        if (w.kind == WidgetKind::HeaderButton) statusRight = std::min(statusRight, w.rect.x);
    const float statusW = statusRight - 330.0f - 16.0f;
    r.text(TextSize::Medium, fit(r, TextSize::Medium, v.status, statusW), 330, 34, rgba(p.text));
    r.text(TextSize::Small, fit(r, TextSize::Small, v.detail, statusW), 330, 56, rgba(v.alert ? p.neonAlt : p.textDim));

    const auto& ws = layout_.widgets();
    for (std::size_t i = 0; i < ws.size(); ++i) {
        const Widget& w   = ws[i];
        const bool    hot = static_cast<int>(i) == v.hover || static_cast<int>(i) == v.active;
        if (w.kind == WidgetKind::HeaderButton) {
            const std::string& value = w.action == HeaderAction::CycleDevice ? v.deviceLabel
                                     : w.action == HeaderAction::ToggleSource ? v.sourceLabel
                                     : std::string(p.name);
            r.roundRect(w.rect, 6.0f, rgba(p.buttonOff, 0.9f));
            if (hot) r.glow(w.rect, 5.0f, rgba(p.neon, 0.35f));
            r.text(TextSize::Small, w.caption, w.rect.x + 10, w.rect.y + 22, rgba(p.neon));
            const float capW = r.textWidth(TextSize::Small, w.caption) + 18.0f;
            r.text(TextSize::Medium, fit(r, TextSize::Medium, value, w.rect.w - capW - 10), w.rect.x + capW, w.rect.y + 23, rgba(p.text));
        }
    }

    for (const auto& st : layout_.strips()) {
        const std::string title = st.isBus ? st.title : "STEM " + st.title;
        r.text(TextSize::Medium, title, st.panel.cx(), st.panel.y + 24, rgba(st.isBus ? p.neon : p.text), Align::Center);
    }

    for (std::size_t i = 0; i < ws.size(); ++i) {
        const Widget& w   = ws[i];
        const bool    hot = static_cast<int>(i) == v.hover || static_cast<int>(i) == v.active;
        switch (w.kind) {
        case WidgetKind::Knob:
        case WidgetKind::SmallKnob: drawKnob(r, p, w, hot); break;
        case WidgetKind::Button:    drawButton(r, p, w, hot); break;
        case WidgetKind::Fader:
            for (const auto& st : layout_.strips())
                if (st.faderParam == w.param) { drawFader(r, p, w, st, hot); break; }
            break;
        case WidgetKind::StemMeter: drawMeter(r, p, w.rect, meters.stem[w.channel], meters.stemHold[w.channel]); break;
        case WidgetKind::BusMeterL: drawMeter(r, p, w.rect, meters.busL[w.channel], meters.busHoldL[w.channel]); break;
        case WidgetKind::BusMeterR: drawMeter(r, p, w.rect, meters.busR[w.channel], meters.busHoldR[w.channel]); break;
        case WidgetKind::HeaderButton: break;
        }
    }
}

} // namespace hc::ui

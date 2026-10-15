#include "hybrid/ui/MixerView.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace hc::ui {

namespace {
constexpr float kPi      = std::numbers::pi_v<float>;
constexpr float kKnobMin = -0.75f * kPi;   // -135°, 0 = straight up, clockwise
constexpr float kKnobMax = 0.75f * kPi;

// Colours (sRGB).
constexpr Rgba kEngrave   = rgb8(0x24272C, 0.92f);
constexpr Rgba kEngraveLo = rgb8(0x2C3036, 0.95f);
constexpr Rgba kLightText = rgb8(0xE9EDF2);
constexpr Rgba kDimText   = rgb8(0xA9B0B9);
constexpr Rgba kNeon      = rgb8(0x56B6FF);
constexpr Rgba kNeonAlert = rgb8(0xFF5A4A);
constexpr Rgba kGreen     = rgb8(0x39E05A);
constexpr Rgba kYellow    = rgb8(0xF5D02A);
constexpr Rgba kRed       = rgb8(0xFF3B30);
constexpr Rgba kLitGreen  = rgb8(0x3FE07A);
constexpr Rgba kLitAmber  = rgb8(0xFFC21A);
constexpr Rgba kLitRed    = rgb8(0xFF4545);
constexpr Rgba kLitCyan   = rgb8(0x3FD0FF);
constexpr Rgba kTabBlue   = rgb8(0x2F7DFF);

// Shortens text with "..." until it fits maxWidth design units.
std::string fit(const Renderer& r, TextStyle st, std::string s, float maxWidth) {
    if (r.textWidth(st, s) <= maxWidth) return s;
    while (s.size() > 4 && r.textWidth(st, s + "...") > maxWidth) {
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

float dbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

Rgba zoneColour(float segNorm) {
    static const float n3 = MixerView::levelToNorm(dbToLin(-3.0f)), n12 = MixerView::levelToNorm(dbToLin(-12.0f));
    return segNorm > n3 ? kRed : segNorm > n12 ? kYellow : kGreen;
}

bool isBipolar(const params::Parameter& p) {
    const auto& sp = p.spec();
    return sp.kind == params::Kind::Pan || (sp.kind == params::Kind::Decibel && !sp.minIsOff && sp.min < 0.0f && sp.max > 0.0f);
}

std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
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

// =====================================================================================
//  STATIC LAYER
// =====================================================================================
void MixerView::drawChassis(Renderer& r, const Canvas& c) const {
    // 3-slice in height: rows above/below the band 1:1, the band stretches with extraH.
    const float E = c.extraH;
    auto bands = [&](float sx, float sw, float dx, float dw) {
        r.chassis({sx, 0, sw, plate::kStretchY0}, {dx, 0, dw, plate::kStretchY0});
        r.chassis({sx, plate::kStretchY0, sw, plate::kStretchY1 - plate::kStretchY0},
                  {dx, plate::kStretchY0, dw, plate::kStretchY1 - plate::kStretchY0 + E});
        r.chassis({sx, plate::kStretchY1, sw, plate::kHeight - plate::kStretchY1},
                  {dx, plate::kStretchY1 + E, dw, plate::kHeight - plate::kStretchY1});
    };
    bands(0, plate::kWidth, 0, plate::kWidth);
    // Pillarbox (window wider than 16:9): stretch the outermost frame columns outward.
    if (c.visibleLeft() < 0.0f) {
        bands(0, plate::kEdgeColumn, c.visibleLeft(), -c.visibleLeft());
        bands(plate::kWidth - plate::kEdgeColumn, plate::kEdgeColumn, plate::kWidth, c.visibleRight() - plate::kWidth);
    }
}

void MixerView::drawKnobStatic(Renderer& r, const Widget& w) const {
    const float D = w.rect.w, cx = w.rect.cx(), cy = w.rect.cy();
    // The socket sits in the glass; its rim casts a soft shadow down-right.
    r.sprite(Sprite::KnobShadow, {cx - 0.56f * D + 0.035f * D, cy - 0.56f * D + 0.06f * D, 1.12f * D, 1.12f * D}, {1, 1, 1, 0.85f});
    const float sd = D * 200.0f / (2.0f * plate::kSocketOuterRadius);
    r.sprite(Sprite::Socket, {cx - sd / 2, cy - sd / 2, sd, sd});
    r.sprite(Sprite::KnobTrack, {cx - D / 2, cy - D / 2, D, D});
}

void MixerView::drawStrip(Renderer& r, const StripLayout& st) const {
    const float cs = 0.55f;   // glass corners at 55 % of the faceplate's pixel size
    r.nineSlice(Sprite::DropShadow, {st.bed.x - 22 + 6, st.bed.y - 22 + 10, st.bed.w + 44, st.bed.h + 44}, 0.55f, {1, 1, 1, 0.9f});
    r.nineSlice(Sprite::Glass, st.bed, cs);
    drawGlassOverlay(r, st.bed.inset(14));

    // Title + accent light.
    const float cx = st.bed.cx();
    r.fillRound({cx - st.bed.w * 0.28f, st.bed.y + 22, st.bed.w * 0.56f, 7}, 3.5f, st.accent, Blend::Over);
    r.text(TextStyle::Title, st.title, cx, st.bed.y + 84, kLightText, Align::Center, TextFx::Shadowed);

    // Fader slot + scale.
    r.nineSlice(Sprite::Glass, st.faderTrack, 0.22f);
    r.nineSlice(Sprite::InnerShadow, st.faderTrack.inset(4), 0.35f);
    const params::Parameter& fp = tree_.at(static_cast<std::size_t>(layout_.widgets()[static_cast<std::size_t>(st.faderWidget)].param));
    const float travel = st.faderTrack.h - st.capH;
    if (!st.isBus) {   // dB scale between the slot and the meter (bus strips are too narrow for it)
        const float labelX = st.bed.cx() + 22;
        for (float db : {6.0f, 0.0f, -6.0f, -12.0f, -24.0f}) {
            const float y = st.faderTrack.y + st.capH * 0.5f + (1.0f - fp.toNormalized(db)) * travel;
            const std::string t = db > 0 ? "+" + std::to_string(static_cast<int>(db)) : std::to_string(static_cast<int>(db));
            r.text(TextStyle::Caption, t, labelX, y + r.capHeight(TextStyle::Caption) * 0.5f, kDimText, Align::Center, TextFx::Shadowed);
        }
        r.text(TextStyle::Caption, "OFF", labelX, st.faderTrack.y + st.capH * 0.5f + travel + r.capHeight(TextStyle::Caption) * 0.5f, kDimText,
               Align::Center, TextFx::Shadowed);
    }

    // Meter window: glass, dark inner shadow, unlit segments (reflection + rim go on top each frame).
    r.nineSlice(Sprite::Glass, st.meter, 0.25f);
    r.nineSlice(Sprite::InnerShadow, st.meter.inset(5), 0.3f);
    if (st.isBus) {
        const Rect in = st.meter.inset(8, 12);
        const float cw = (in.w - 4) * 0.5f;
        drawMeterColumn(r, {in.x, in.y, cw, in.h}, 0, 0, false);
        drawMeterColumn(r, {in.x + cw + 4, in.y, cw, in.h}, 0, 0, false);
    } else {
        drawMeterColumn(r, st.meter.inset(9, 12), 0, 0, false);
    }

    // Value readout window.
    r.nineSlice(Sprite::Glass, st.valueBox, 0.3f);
}

void MixerView::drawStatic(Renderer& r, const Canvas& canvas) const {
    drawChassis(r, canvas);

    // Engraved titles on the chrome.
    r.text(TextStyle::Heading, "THE HYBRID CORE", 250, 145, kEngrave, Align::Left, TextFx::Engraved);
    r.text(TextStyle::Tab, "HYBRID ELITE MAFIA", 3690, 140, kEngraveLo, Align::Right, TextFx::Engraved);

    for (const StripLayout& st : layout_.strips()) drawStrip(r, st);

    for (const Widget& w : layout_.widgets()) {
        switch (w.kind) {
        case WidgetKind::Knob:
        case WidgetKind::SendKnob: {
            drawKnobStatic(r, w);
            const TextStyle ls = w.kind == WidgetKind::Knob ? TextStyle::Label : TextStyle::Caption;
            r.text(ls, w.caption, w.rect.cx(), w.rect.y - (w.kind == WidgetKind::Knob ? 14.0f : 10.0f), kDimText, Align::Center, TextFx::Shadowed);
            break;
        }
        default: break;
        }
    }

    // Main bus meter in the bottom bar.
    const Rect m = layout_.masterMeter();
    r.nineSlice(Sprite::DropShadow, {m.x - 22 + 5, m.y - 22 + 8, m.w + 44, m.h + 44}, 0.55f, {1, 1, 1, 0.8f});
    r.nineSlice(Sprite::Glass, m, 0.35f);
    r.nineSlice(Sprite::InnerShadow, m.inset(6), 0.35f);
    const Rect in = m.inset(52, 13);
    const float rh = (in.h - 6) * 0.5f;
    drawMeterRow(r, {in.x, in.y, in.w, rh}, 0, 0, false);
    drawMeterRow(r, {in.x, in.y + rh + 6, in.w, rh}, 0, 0, false);
    r.text(TextStyle::Caption, "L", m.x + 30, in.y + rh * 0.5f + r.capHeight(TextStyle::Caption) * 0.5f, kDimText, Align::Center, TextFx::Shadowed);
    r.text(TextStyle::Caption, "R", m.x + 30, in.y + rh * 1.5f + 6 + r.capHeight(TextStyle::Caption) * 0.5f, kDimText, Align::Center, TextFx::Shadowed);
    r.text(TextStyle::Status, "MAIN OUT", m.x - 24, m.cy() + r.capHeight(TextStyle::Status) * 0.5f, kEngrave, Align::Right, TextFx::Engraved);
}

// Emissive parts of the static layer, re-submitted each frame for bloom only.
void MixerView::drawEmissiveStatic(Renderer& r) const {
    for (const StripLayout& st : layout_.strips()) {
        const float cx = st.bed.cx();
        r.fillRound({cx - st.bed.w * 0.28f, st.bed.y + 22, st.bed.w * 0.56f, 7}, 3.5f, st.accent, Blend::Over, true);
    }
}

void MixerView::drawGlassOverlay(Renderer& r, Rect g) const {
    r.sprite(Sprite::GlassSheen, g, {1, 1, 1, 1}, Blend::Over);
    r.nineSlice(Sprite::GlassRim, g, 0.4f);
}

// =====================================================================================
//  DYNAMIC
// =====================================================================================
void MixerView::drawMeterColumn(Renderer& r, Rect col, float level, float hold, bool lit) const {
    const int   n     = std::max(4, static_cast<int>(col.h / 14.0f));
    const float pitch = col.h / static_cast<float>(n);
    const float segH  = pitch * 0.68f;
    const float ln = levelToNorm(level), hn = levelToNorm(hold);
    const int   holdSeg = hold > 0.0f ? std::min(n - 1, static_cast<int>(hn * static_cast<float>(n))) : -1;
    for (int i = 0; i < n; ++i) {
        const float segNorm = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const Rect  seg{col.x, col.bottom() - (static_cast<float>(i) + 1) * pitch + (pitch - segH) * 0.5f, col.w, segH};
        const Rgba  c = zoneColour(segNorm);
        if (!lit) { r.sprite(Sprite::LedSegment, seg, c.withAlpha(0.08f)); continue; }
        if (segNorm <= ln) r.sprite(Sprite::LedSegment, seg, c, Blend::Over, true);
        else if (i == holdSeg) r.sprite(Sprite::LedSegment, seg, c, Blend::Over, true);
    }
}

void MixerView::drawMeterRow(Renderer& r, Rect row, float level, float hold, bool lit) const {
    const int   n     = std::max(4, static_cast<int>(row.w / 17.0f));
    const float pitch = row.w / static_cast<float>(n);
    const float segW  = pitch * 0.70f;
    const float ln = levelToNorm(level), hn = levelToNorm(hold);
    const int   holdSeg = hold > 0.0f ? std::min(n - 1, static_cast<int>(hn * static_cast<float>(n))) : -1;
    for (int i = 0; i < n; ++i) {
        const float segNorm = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const Rect  seg{row.x + static_cast<float>(i) * pitch + (pitch - segW) * 0.5f, row.y, segW, row.h};
        const Rgba  c = zoneColour(segNorm);
        if (!lit) { r.sprite(Sprite::LedSegment, seg, c.withAlpha(0.08f)); continue; }
        if (segNorm <= ln || i == holdSeg) r.sprite(Sprite::LedSegment, seg, c, Blend::Over, true);
    }
}

void MixerView::drawKnob(Renderer& r, const Widget& w, Rgba accent, bool hot) const {
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const float D = w.rect.w, R = D * 0.5f, cx = w.rect.cx(), cy = w.rect.cy();
    const float n   = prm.normalized();
    const float ang = kKnobMin + (kKnobMax - kKnobMin) * n;
    const float a0  = kKnobMin + (kKnobMax - kKnobMin) * (isBipolar(prm) ? prm.toNormalized(0.0f) : 0.0f);
    const bool  off = prm.spec().minIsOff && prm.isOff();

    // Value arc: unlit LED ring, then the lit part (emissive -> bloom) and a hot tip.
    const float ar = knob::kArcRadius * R, aw = knob::kArcWidth * R;
    r.arc(cx, cy, ar, aw, kKnobMin, kKnobMax, accent.withAlpha(0.16f), false);
    if (!off && std::fabs(ang - a0) > 0.004f) {
        r.arc(cx, cy, ar, aw, std::min(a0, ang), std::max(a0, ang), accent, true);
        const float tx = cx + std::sin(ang) * ar, ty = cy - std::cos(ang) * ar;
        r.sprite(Sprite::GlowDot, {tx - aw * 2.2f, ty - aw * 2.2f, aw * 4.4f, aw * 4.4f}, accent.withAlpha(0.9f), Blend::Add, true);
    }
    // Body (rotates), its contact shadow, indicator (rotates), fixed light on top.
    const float bodyR = knob::kBody * R;
    r.sprite(Sprite::KnobShadow, {cx - bodyR * 1.12f + 0.05f * bodyR, cy - bodyR * 1.12f + 0.09f * bodyR, bodyR * 2.24f, bodyR * 2.24f});
    const float bd = bodyR * 2.0f * 256.0f / 254.0f;
    r.spriteRotated(Sprite::KnobBody, cx, cy, bd, bd, ang);
    const float len = (knob::kIndicatorTo - knob::kIndicatorFrom) * bodyR;
    const float mid = 0.5f * (knob::kIndicatorTo + knob::kIndicatorFrom) * bodyR;
    const float ih = len * 96.0f / 72.0f, iw = ih * 24.0f / 96.0f;
    const Rgba  ind{0.5f + 0.5f * accent.r, 0.5f + 0.5f * accent.g, 0.5f + 0.5f * accent.b, 1.0f};
    r.spriteRotated(Sprite::KnobIndicator, cx + std::sin(ang) * mid, cy - std::cos(ang) * mid, iw, ih, ang, ind);
    r.sprite(Sprite::KnobLight, {cx - bd / 2, cy - bd / 2, bd, bd});

    if (hot) {   // value replaces the label while hovered / dragged
        const bool big = w.kind == WidgetKind::Knob;
        const TextStyle ls = big ? TextStyle::Label : TextStyle::Caption;
        const float baseline = w.rect.y - (big ? 14.0f : 10.0f);
        const float tw = r.textWidth(ls, prm.format()) + 24;
        const float ch = r.capHeight(ls);
        r.fillRound({cx - tw / 2, baseline - ch - 10, tw, ch + 20}, 10, rgb8(0x0B0D10, 0.92f), Blend::Over);
        r.text(ls, prm.format(), cx, baseline, accent, Align::Center, TextFx::Neon);
    }
}

void MixerView::drawButton(Renderer& r, const Widget& w, bool pressed) const {
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const bool  on  = prm.isOn();
    const bool  in  = on || pressed;
    const float px  = r.pixelSize();
    const Rect  b   = w.rect;
    // Pushed in: 1.5 px down-right, shadow shrinks.
    const float push = in ? 1.5f * px : 0.0f;
    const float sx = in ? 0.02f : 0.07f, sy = in ? 0.03f : 0.12f, grow = in ? 1.04f : 1.16f;
    r.sprite(Sprite::ButtonShadow, {b.cx() - b.w * grow * 0.5f * 96.0f / 68.0f + b.w * sx, b.cy() - b.h * grow * 0.5f * 72.0f / 44.0f + b.h * sy,
                                    b.w * grow * 96.0f / 68.0f, b.h * grow * 72.0f / 44.0f}, {1, 1, 1, in ? 0.55f : 0.9f});
    const Rect cap{b.x - b.w * (128.0f / 116.0f - 1.0f) * 0.5f + push, b.y - b.h * (96.0f / 84.0f - 1.0f) * 0.5f + push,
                   b.w * 128.0f / 116.0f, b.h * 96.0f / 84.0f};
    r.sprite(Sprite::ButtonCap, cap, in ? Rgba{0.86f, 0.86f, 0.86f, 1} : Rgba{1, 1, 1, 1});
    Rgba lit = kLitRed;
    if (w.caption == "S") lit = kLitAmber;
    else if (w.caption != "M") lit = kLitCyan;
    const float baseline = b.cy() + r.capHeight(TextStyle::Button) * 0.5f + push;
    if (on) {
        r.sprite(Sprite::ButtonLens, cap, lit.withAlpha(0.95f), Blend::Over, true);
        r.sprite(Sprite::ButtonHalo, {b.cx() - b.w * 0.69f + push, b.cy() - b.h * 0.76f + push, b.w * 1.38f, b.h * 1.52f}, lit.withAlpha(0.7f), Blend::Add, true);
        r.text(TextStyle::Button, w.caption, b.cx() + push, baseline, rgb8(0xFFFFFF), Align::Center, TextFx::Plain);
    } else {
        r.text(TextStyle::Button, w.caption, b.cx() + push, baseline, kEngrave, Align::Center, TextFx::Engraved);
    }
}

void MixerView::drawFader(Renderer& r, const StripLayout& st, Rgba lineColour) const {
    const Widget& w = layout_.widgets()[static_cast<std::size_t>(st.faderWidget)];
    const params::Parameter& prm = tree_.at(static_cast<std::size_t>(w.param));
    const float travel = st.faderTrack.h - st.capH;
    const float cy = st.faderTrack.y + st.capH * 0.5f + (1.0f - prm.normalized()) * travel;
    const float cx = st.faderTrack.cx();
    const float cw = st.capW * 256.0f / 248.0f, ch = st.capH * 136.0f / 124.0f;   // sprite margins
    r.sprite(Sprite::FaderShadow, {cx - cw * 0.62f + cw * 0.06f, cy - ch * 0.75f + ch * 0.16f, cw * 1.24f, ch * 1.5f}, {1, 1, 1, 0.95f});
    r.sprite(Sprite::FaderCap, {cx - cw / 2, cy - ch / 2, cw, ch});
    r.sprite(Sprite::FaderLine, {cx - st.capW * 0.43f, cy - st.capH * 0.085f, st.capW * 0.86f, st.capH * 0.17f}, lineColour, Blend::Over, true);

    // Value readout.
    std::string v = prm.format();
    if (r.textWidth(TextStyle::Value, v) > st.valueBox.w - 16 && v.size() > 3 && v.ends_with(" dB")) v.resize(v.size() - 3);
    r.text(TextStyle::Value, fit(r, TextStyle::Value, v, st.valueBox.w - 16), st.valueBox.cx(),
           st.valueBox.cy() + r.capHeight(TextStyle::Value) * 0.5f, lineColour, Align::Center, TextFx::Neon);
}

void MixerView::drawTransport(Renderer& r, const Widget& w, const ViewState& v) const {
    const float cx = w.rect.cx(), cy = w.rect.cy();
    const bool  pressed = v.pressed >= 0 && &layout_.widgets()[static_cast<std::size_t>(v.pressed)] == &w;
    bool anySolo = false, anyMute = false;
    for (const Widget& x : layout_.widgets())
        if (x.kind == WidgetKind::Button && x.param >= 0 && tree_.at(static_cast<std::size_t>(x.param)).isOn()) {
            if (x.caption == "S") anySolo = true;
            if (x.caption == "M") anyMute = true;
        }
    bool active = false;
    Rgba lit = kLitGreen;
    switch (w.action) {
    case UiAction::SourceTones:  active = v.tonesOn; break;
    case UiAction::SourceInputs: active = !v.tonesOn; break;
    case UiAction::ClearSolo:    active = anySolo; lit = kLitAmber; break;
    case UiAction::ClearMute:    active = anyMute; lit = kLitRed; break;
    default: break;
    }
    const float px = r.pixelSize(), push = pressed ? 2.0f * px : 0.0f;
    const float capR = 76.0f, size = capR * 192.0f / 91.0f;
    r.sprite(Sprite::KnobShadow, {cx - capR * 1.1f + (pressed ? 2.0f : 6.0f), cy - capR * 1.1f + (pressed ? 3.0f : 10.0f), capR * 2.2f, capR * 2.2f},
             {1, 1, 1, pressed ? 0.6f : 0.9f});
    r.sprite(Sprite::CapDark, {cx - size / 2 + push, cy - size / 2 + push, size, size});
    if (active) {
        const float gs = 79.0f / 0.86f * 2.0f;
        r.sprite(Sprite::RingGlow, {cx - gs / 2, cy - gs / 2, gs, gs}, lit.withAlpha(0.85f), Blend::Add, true);
    }
    const Rgba ic = active ? lit : rgb8(0xD9DDE3);
    const float iy = cy - 12 + push, ix = cx + push;
    const float is = 62.0f;
    switch (w.action) {
    case UiAction::PrevDevice:   r.sprite(Sprite::IconPrev, {ix - is / 2, iy - is / 2, is, is}, ic, Blend::Over, active); break;
    case UiAction::NextDevice:   r.sprite(Sprite::IconNext, {ix - is / 2, iy - is / 2, is, is}, ic, Blend::Over, active); break;
    case UiAction::SourceTones:  r.sprite(Sprite::IconSine, {ix - is / 2, iy - is / 2, is, is}, ic, Blend::Over, active); break;
    case UiAction::SourceInputs: r.sprite(Sprite::IconInput, {ix - is / 2, iy - is / 2, is, is}, ic, Blend::Over, active); break;
    case UiAction::ClearSolo:
    case UiAction::ClearMute:
        r.text(TextStyle::Panel, w.action == UiAction::ClearSolo ? "S" : "M", ix, iy + r.capHeight(TextStyle::Panel) * 0.5f, ic, Align::Center,
               active ? TextFx::Neon : TextFx::Plain);
        break;
    default: break;
    }
    const char* cap = w.action == UiAction::ClearSolo ? "CLR SOLO" : w.action == UiAction::ClearMute ? "CLR MUTE" : w.caption.c_str();
    r.text(TextStyle::Caption, cap, ix, cy + 44 + push, active ? lit : rgb8(0x9DA3AB), Align::Center, active ? TextFx::Neon : TextFx::Plain);
}

void MixerView::drawDisplay(Renderer& r, const ViewState& v) const {
    const Rect g = plate::kDisplayGlass;
    const EngineReadout& e = v.engine;
    const Rgba c = v.alert ? kNeonAlert : kNeon;
    const Rgba dim = c.withAlpha(0.75f);
    const float capY = g.y + 52, bigY = g.y + 150;

    auto bigNumber = [&](float x, const std::string& num, const char* unit, const char* caption) {
        r.text(TextStyle::DisplayCap, caption, x, capY, dim, Align::Left, TextFx::Neon);
        r.text(TextStyle::Display, num, x, bigY, c, Align::Left, TextFx::Neon);
        r.text(TextStyle::DisplayCap, unit, x + r.textWidth(TextStyle::Display, num) + 10, bigY, dim, Align::Left, TextFx::Neon);
    };
    if (e.valid) {
        bigNumber(g.x + 46, e.latencyMs >= 0 ? fmt("%.2f", e.latencyMs) : fmt("%.2f", 1000.0 * e.bufferFrames / std::max(1.0, e.sampleRate)),
                  "ms", e.latencyMs >= 0 ? "OUTPUT LATENCY" : "BUFFER TIME");
        bigNumber(g.x + 430, fmt("%.1f", e.loadPct), "%", "DSP LOAD");
    } else {
        bigNumber(g.x + 46, "--", "ms", "OUTPUT LATENCY");
        bigNumber(g.x + 430, "--", "%", "DSP LOAD");
    }
    const float rx = g.x + 760;
    const std::string l1 = e.valid ? e.driver : "NO STREAM";
    const std::string l2 = e.valid ? fmt("%.1f kHz", e.sampleRate / 1000.0) + "   " + std::to_string(e.bufferFrames) + " SMP" : "--";
    const std::string l3 = e.valid ? "PEAK " + fmt("%.1f%%", e.peakLoadPct) : "";
    const std::string l4 = e.valid ? "DROPOUTS " + std::to_string(e.dropouts) : "";
    r.text(TextStyle::DisplayCap, fit(r, TextStyle::DisplayCap, l1, g.right() - rx - 30), rx, capY, c, Align::Left, TextFx::Neon);
    r.text(TextStyle::DisplayCap, l2, rx, capY + 40, dim, Align::Left, TextFx::Neon);
    r.text(TextStyle::DisplayCap, l3, rx, capY + 80, dim, Align::Left, TextFx::Neon);
    r.text(TextStyle::DisplayCap, l4, rx, capY + 120, e.dropouts ? kNeonAlert : dim, Align::Left, TextFx::Neon);
}

void MixerView::drawDynamic(Renderer& r, const ViewState& v, const MeterState& m) const {
    r.setBloomOnly(true);
    drawEmissiveStatic(r);
    r.setBloomOnly(false);

    // Status lamp in the logo socket.
    {
        const Circle s = plate::kLogoSocket;
        const Rgba lamp = v.alert ? kLitAmber : v.running ? kLitGreen : kLitRed;
        const float lr = s.r * 0.78f;
        r.sprite(Sprite::GlowDot, {s.cx - s.r * 1.6f, s.cy - s.r * 1.6f, s.r * 3.2f, s.r * 3.2f}, lamp.withAlpha(0.55f), Blend::Add, true);
        r.sprite(Sprite::Lamp, {s.cx - lr * 128 / 120, s.cy - lr * 128 / 120, lr * 256 / 120, lr * 256 / 120}, lamp, Blend::Over, true);
        r.sprite(Sprite::GlowDot, {s.cx - lr * 0.75f, s.cy - lr * 0.8f, lr * 0.7f, lr * 0.7f}, rgb8(0xFFFFFF, 0.9f), Blend::Add);
    }

    // Name panel (engraved on brushed chrome).
    {
        const Rect p = plate::kNamePanel;
        r.text(TextStyle::Panel, fit(r, TextStyle::Panel, v.deviceName, p.w - 90), p.x + 46, p.y + 92, kEngrave, Align::Left, TextFx::Engraved);
        r.text(TextStyle::Sub, fit(r, TextStyle::Sub, v.deviceFormat, p.w - 90), p.x + 46, p.y + 146, kEngraveLo, Align::Left, TextFx::Engraved);
    }

    drawDisplay(r, v);

    // Widgets.
    const std::uint32_t bank = layout_.faderBank();
    for (std::size_t i = 0; i < layout_.widgets().size(); ++i) {
        const Widget& w = layout_.widgets()[i];
        const bool hot = static_cast<int>(i) == v.hover || static_cast<int>(i) == v.active;
        switch (w.kind) {
        case WidgetKind::Knob:
        case WidgetKind::SendKnob: {
            Rgba acc = stemAccent(w.index);
            if (w.kind == WidgetKind::SendKnob) acc = busAccent(static_cast<std::uint32_t>(w.caption[1] - '0'));
            drawKnob(r, w, acc, hot);
            break;
        }
        case WidgetKind::Button: drawButton(r, w, static_cast<int>(i) == v.pressed); break;
        case WidgetKind::Transport: drawTransport(r, w, v); break;
        case WidgetKind::Tab: {
            const bool sel = w.index == bank;
            const float baseline = w.rect.cy() + r.capHeight(TextStyle::Tab) * 0.5f;
            if (sel) {
                const Rect f = w.rect.inset(8, 9);
                r.fillRound(f, plate::kTabFaceRadius - 6, kTabBlue.withAlpha(0.62f), Blend::Over);
                r.glow(f, plate::kTabFaceRadius - 6, 9, kTabBlue.withAlpha(0.45f), true);
                r.text(TextStyle::Tab, w.caption, w.rect.cx(), baseline, rgb8(0xFFFFFF), Align::Center, TextFx::Shadowed);
            } else {
                r.text(TextStyle::Tab, w.caption, w.rect.cx(), baseline, kEngrave, Align::Center, TextFx::Engraved);
            }
            break;
        }
        default: break;
        }
    }

    for (const StripLayout& st : layout_.strips()) {
        const Rgba line = (!st.isBus && bank > 0) ? busAccent(bank) : st.accent;
        drawFader(r, st, line);
        if (st.isBus) {
            const Rect in = st.meter.inset(8, 12);
            const float cw = (in.w - 4) * 0.5f;
            drawMeterColumn(r, {in.x, in.y, cw, in.h}, m.busL[st.index], m.busHoldL[st.index], true);
            drawMeterColumn(r, {in.x + cw + 4, in.y, cw, in.h}, m.busR[st.index], m.busHoldR[st.index], true);
        } else {
            drawMeterColumn(r, st.meter.inset(9, 12), m.stem[st.index], m.stemHold[st.index], true);
        }
        drawGlassOverlay(r, st.meter.inset(5));
        drawGlassOverlay(r, st.valueBox.inset(5));
    }

    // Main bus meter + status line.
    const Rect mm = layout_.masterMeter();
    const Rect in = mm.inset(52, 13);
    const float rh = (in.h - 6) * 0.5f;
    drawMeterRow(r, {in.x, in.y, in.w, rh}, m.busL[0], m.busHoldL[0], true);
    drawMeterRow(r, {in.x, in.y + rh + 6, in.w, rh}, m.busR[0], m.busHoldR[0], true);
    drawGlassOverlay(r, mm.inset(6));
    const Rect sa = layout_.statusArea();
    r.text(TextStyle::Status, fit(r, TextStyle::Status, v.status, sa.w - 300), sa.x, sa.cy() + r.capHeight(TextStyle::Status) * 0.5f,
           v.alert ? rgb8(0x8A1C14) : kEngrave, Align::Left, TextFx::Engraved);
}

} // namespace hc::ui

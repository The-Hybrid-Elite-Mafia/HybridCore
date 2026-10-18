#include "hybrid/ui/FxPanelView.hpp"
#include "hybrid/ui/Parts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace hc::ui {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kKnobMin = -0.75f * kPi, kKnobMax = 0.75f * kPi;
const Rgba kText    = rgb8(0xE8E6F2);
const Rgba kTextDim = rgb8(0x8C88A0);
const Rgba kGlass   = rgb8(0x050507, 0.86f);

Rect at(Rect r, float ox, float oy) { return r.offset(ox, oy); }


} // namespace

void FxPanelView::drawCrown(Renderer& r, Rect a) const {
    // A simple five-point crown in neon (our own mark for the HEM plug-ins).
    const float x0 = a.x + a.w * 0.18f, x1 = a.right() - a.w * 0.18f, base = a.y + a.h * 0.68f, top = a.y + a.h * 0.30f, mid = a.y + a.h * 0.50f;
    const float w = x1 - x0;
    const float xs[] = {x0, x0, x0 + w * 0.25f, x0 + w * 0.5f, x0 + w * 0.75f, x1, x1, x0};
    const float ys[] = {base, top, mid, top - a.h * 0.06f, mid, top, base, base};
    r.polyline(xs, ys, 8, 7.0f, pal_.lime.withAlpha(0.35f), Blend::Add, true);
    r.polyline(xs, ys, 8, 3.5f, pal_.lime, Blend::Over, true);
    for (int i : {1, 3, 5}) r.sprite(Sprite::GlowDot, {xs[i] - 9, ys[i] - 9, 18, 18}, pal_.lime, Blend::Add, true);
    r.fillRound({x0, base + 8, w, 6}, 3, pal_.violet, Blend::Over, true);
}

void FxPanelView::drawKnob(Renderer& r, const PanelKnob& k, const PanelHost& host, bool hot, float ox, float oy) const {
    const PanelParam& prm = spec_.params[static_cast<std::size_t>(k.param)];
    const Rect well = at(k.well, ox, oy);
    const float cx = well.cx(), cy = well.cy();
    const float R = well.w * 0.5f * 0.92f;
    const float n = std::clamp(host.normalized(k.param), 0.0f, 1.0f);
    const float ang = kKnobMin + (kKnobMax - kKnobMin) * n;
    const float a0 = kKnobMin + (kKnobMax - kKnobMin) * (prm.bipolar ? 0.5f : 0.0f);
    const float ar = knob::kArcRadius * R, aw = knob::kArcWidth * R * 1.15f;
    r.sprite(Sprite::KnobTrack, {cx - R, cy - R, 2 * R, 2 * R});
    r.arc(cx, cy, ar, aw, kKnobMin, kKnobMax, pal_.lime.withAlpha(0.16f), false);
    if (std::fabs(ang - a0) > 0.004f) {
        r.arc(cx, cy, ar, aw, std::min(a0, ang), std::max(a0, ang), pal_.lime, true);
        const float tx = cx + std::sin(ang) * ar, ty = cy - std::cos(ang) * ar;
        r.sprite(Sprite::GlowDot, {tx - aw * 2.4f, ty - aw * 2.4f, aw * 4.8f, aw * 4.8f}, pal_.lime.withAlpha(0.95f), Blend::Add, true);
    }
    const float bodyR = knob::kBody * R;
    r.sprite(Sprite::KnobShadow, {cx - bodyR * 1.14f + 0.06f * bodyR, cy - bodyR * 1.14f + 0.10f * bodyR, bodyR * 2.28f, bodyR * 2.28f});
    const float bd = bodyR * 2.0f * 256.0f / 254.0f;
    r.spriteRotated(Sprite::KnobBody, cx, cy, bd, bd, ang);
    const float len = (knob::kIndicatorTo - knob::kIndicatorFrom) * bodyR;
    const float mid = 0.5f * (knob::kIndicatorTo + knob::kIndicatorFrom) * bodyR;
    const float ih = len * 96.0f / 72.0f, iw = ih * 24.0f / 96.0f;
    r.spriteRotated(Sprite::KnobIndicator, cx + std::sin(ang) * mid, cy - std::cos(ang) * mid, iw, ih, ang,
                    {0.55f + 0.45f * pal_.lime.r, 0.55f + 0.45f * pal_.lime.g, 0.55f + 0.45f * pal_.lime.b, 1.0f}, Blend::Over, true);
    r.sprite(Sprite::KnobLight, {cx - bd / 2, cy - bd / 2, bd, bd});
    if (hot) r.glow(well.inset(well.w * 0.12f), well.w * 0.4f, 18.0f, pal_.violet.withAlpha(0.35f), true);

    // value window
    const Rect v = at(k.value, ox, oy);
    r.fillRound(v.inset(3), 7, kGlass, Blend::Over);
    const std::string t = host.text(k.param);
    r.text(TextStyle::Value, t, v.cx(), v.cy() + r.capHeight(TextStyle::Value) * 0.5f, hot ? kText : pal_.lime, Align::Center, TextFx::Neon);
}

void FxPanelView::drawButton(Renderer& r, Rect b, const std::string& text, bool lit, bool hot, Rgba colour) const {
    if (lit) {
        r.glow(b.inset(4), 10, 16.0f, colour.withAlpha(0.55f), true);
        r.fillRound(b.inset(2), 10, colour.withAlpha(0.85f), Blend::Over, true);
        r.fillRound({b.x + 8, b.y + 5, b.w - 16, b.h * 0.35f}, 8, {1, 1, 1, 0.16f}, Blend::Add);
        r.text(TextStyle::Caption, text, b.cx(), b.cy() + r.capHeight(TextStyle::Caption) * 0.5f, rgb8(0x0A0812), Align::Center, TextFx::Plain);
    } else {
        r.fillRound(b.inset(2), 10, kGlass, Blend::Over);
        r.fillRound({b.x + 2, b.y + 2, b.w - 4, 2}, 1, colour.withAlpha(hot ? 0.8f : 0.35f), Blend::Over, true);
        r.text(TextStyle::Caption, text, b.cx(), b.cy() + r.capHeight(TextStyle::Caption) * 0.5f, hot ? kText : kTextDim, Align::Center, TextFx::Shadowed);
    }
}

void FxPanelView::drawMeter(Renderer& r, const PanelMeterSlot& s, float value, float ox, float oy) const {
    const PanelMeter& m = spec_.meters[static_cast<std::size_t>(s.meter)];
    const Rect bar = at(s.bar, ox, oy);
    r.fillRound(bar, 8, rgb8(0x020203, 0.95f), Blend::Over);
    // scale ticks
    for (int t = 1; t < 8; ++t) {
        const float y = bar.y + bar.h * static_cast<float>(t) / 8.0f;
        r.fillRound({bar.x + 4, y - 0.75f, bar.w - 8, 1.5f}, 0.75f, {1, 1, 1, 0.07f}, Blend::Over);
    }
    const float f = panelMeterFraction(m, value);
    const Rect inner = bar.inset(5);
    if (m.centred) {
        const float midY = inner.cy();
        r.fillRound({inner.x - 3, midY - 1, inner.w + 6, 2}, 1, {1, 1, 1, 0.35f}, Blend::Over);
        if (std::fabs(f) > 0.002f) {
            const float h = inner.h * 0.5f * std::fabs(f);
            const Rect fill = f > 0 ? Rect{inner.x, midY - h, inner.w, h} : Rect{inner.x, midY, inner.w, h};
            const Rgba c = f > 0 ? pal_.lime : pal_.violet;
            r.glow(fill, 6, 10.0f, c.withAlpha(0.45f), true);
            r.fillRound(fill, 5, c, Blend::Over, true);
        }
    } else if (f > 0.002f) {
        const bool fromTop = m.unit == "dB" && m.max <= 0.0f;   // reduction hangs from the top
        const float h = inner.h * f;
        const Rect fill = fromTop ? Rect{inner.x, inner.y, inner.w, h} : Rect{inner.x, inner.bottom() - h, inner.w, h};
        const Rgba c = fromTop ? pal_.violet : pal_.lime;
        r.glow(fill, 6, 10.0f, c.withAlpha(0.45f), true);
        r.fillRound(fill, 5, c, Blend::Over, true);
    }
    const Rect vbox = at(s.value, ox, oy);
    r.fillRound(vbox.inset(2), 7, kGlass, Blend::Over);
    r.text(TextStyle::Caption, formatPanelMeter(m, value), vbox.cx(), vbox.cy() + r.capHeight(TextStyle::Caption) * 0.5f, pal_.lime, Align::Center, TextFx::Neon);
    const Rect cap = at(s.caption, ox, oy);
    r.text(TextStyle::Caption, m.name, cap.cx(), cap.cy() + r.capHeight(TextStyle::Caption) * 0.5f, kText, Align::Center, TextFx::Shadowed);
}

void FxPanelView::draw(Renderer& r, const PanelHost& host, PanelHit hover, PanelHit active, float ox, float oy, bool plate) const {
    const PanelLayout& L = layout_;
    if (plate) r.panel({ox, oy, L.width, L.height});

    // neon tubes in their channels
    for (const PanelGroove& g : L.grooves) {
        const Rgba c = g.lime ? pal_.lime : pal_.violet;
        const float xs[2] = {g.x0 + ox, g.x1 + ox}, ys[2] = {g.y0 + oy, g.y1 + oy};
        r.glow({std::min(xs[0], xs[1]), std::min(ys[0], ys[1]) - 1, std::fabs(xs[1] - xs[0]), std::fabs(ys[1] - ys[0]) + 2}, 2, 14.0f, c.withAlpha(0.55f), true);
        r.polyline(xs, ys, 2, 4.0f, c, Blend::Over, true);
        r.polyline(xs, ys, 2, 1.6f, {1, 1, 1, 0.75f}, Blend::Add, true);
    }
    // header: crown, name, blurb
    drawCrown(r, at(L.logo, ox, oy).inset(12));
    const Rect ta = at(L.titleArea, ox, oy);
    const TextStyle ts = r.textWidth(TextStyle::Display, L.name) <= ta.w ? TextStyle::Display : TextStyle::Heading;
    r.text(ts, L.name, ta.x, ta.y + 92, pal_.lime, Align::Left, TextFx::Neon);
    r.text(TextStyle::Sub, L.blurb, ta.x + 4, ta.y + 136, kText, Align::Left, TextFx::Shadowed);
    // section captions
    for (const PanelSection& s : L.sections)
        if (!s.caption.empty()) {
            const Rect c = at(s.captionArea, ox, oy);
            r.text(TextStyle::Label, s.caption, c.x + 8, c.y + 32, pal_.violet, Align::Left, TextFx::Neon);
        }
    // control captions
    for (const PanelCaption& c : L.captions) {
        const Rect a = at(c.r, ox, oy);
        std::string name = spec_.params[static_cast<std::size_t>(c.param)].name;
        for (char& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        r.text(TextStyle::Label, name, a.cx(), a.y + 32, kText, Align::Center, TextFx::Shadowed);
    }
    // knobs
    for (std::size_t i = 0; i < L.knobs.size(); ++i) {
        const bool hot = (hover.kind == PanelHit::Kind::Knob && hover.index == static_cast<int>(i)) ||
                         (active.kind == PanelHit::Kind::Knob && active.index == static_cast<int>(i));
        drawKnob(r, L.knobs[i], host, hot, ox, oy);
    }
    // buttons
    for (std::size_t i = 0; i < L.buttons.size(); ++i) {
        const PanelButton& b = L.buttons[i];
        const PanelParam& p = spec_.params[static_cast<std::size_t>(b.param)];
        const float n = host.normalized(b.param);
        const bool hot = hover.kind == PanelHit::Kind::Button && hover.index == static_cast<int>(i);
        if (b.choice < 0) {
            drawButton(r, at(b.rect, ox, oy), n >= 0.5f ? "ON" : "OFF", n >= 0.5f, hot, pal_.lime);
        } else {
            const int sel = static_cast<int>(std::lround(n * static_cast<float>(std::max<std::size_t>(1, p.choices.size()) - 1)));
            drawButton(r, at(b.rect, ox, oy), b.caption, sel == b.choice, hot, pal_.violet);
        }
    }
    // selectors
    for (std::size_t i = 0; i < L.selectors.size(); ++i) {
        const PanelSelector& s = L.selectors[i];
        const Rect box = at(s.box, ox, oy);
        r.fillRound(box.inset(3), 9, kGlass, Blend::Over);
        r.text(TextStyle::Title, host.text(s.param), box.cx(), box.cy() + r.capHeight(TextStyle::Title) * 0.5f, pal_.lime, Align::Center, TextFx::Neon);
        for (int side = 0; side < 2; ++side) {
            const Rect b = at(side == 0 ? s.prev : s.next, ox, oy);
            const bool hot = hover.index == static_cast<int>(i) &&
                             hover.kind == (side == 0 ? PanelHit::Kind::SelectorPrev : PanelHit::Kind::SelectorNext);
            drawButton(r, b, "", false, hot, pal_.violet);
            const float dir = side == 0 ? -1.0f : 1.0f, cx = b.cx(), cy = b.cy(), sz = 11.0f;
            const float xs[4] = {cx - dir * sz * 0.7f, cx + dir * sz * 0.9f, cx - dir * sz * 0.7f, cx - dir * sz * 0.7f};
            const float ys[4] = {cy - sz, cy, cy + sz, cy - sz};
            r.polyline(xs, ys, 4, 3.0f, hot ? pal_.lime : pal_.violet, Blend::Over, true);
        }
    }
    // meters
    for (const PanelMeterSlot& m : L.meters) drawMeter(r, m, host.meter(m.meter), ox, oy);
    // footer
    const Rect ft = at(L.footer, ox, oy);
    r.text(TextStyle::Status, "HYBRID ELITE MAFIA  -  STUDIO PLUG-INS", ft.x + 24, ft.cy() + 10, pal_.violet, Align::Left, TextFx::Neon);
    if (host.hasBypass()) {
        const bool byp = host.bypassed();
        drawButton(r, at(L.bypass, ox, oy), byp ? "BYPASSED" : "BYPASS", byp, hover.kind == PanelHit::Kind::Bypass, pal_.violet);
    }
}

} // namespace hc::ui

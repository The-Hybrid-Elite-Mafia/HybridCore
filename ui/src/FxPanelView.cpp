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

// Text sizes the faceplate may use, smallest first (semi-bold / bold faces only).
constexpr TextStyle kLadder[] = {TextStyle::Caption, TextStyle::Value, TextStyle::Label, TextStyle::Title,
                                 TextStyle::Tab,     TextStyle::Panel, TextStyle::Heading, TextStyle::Display};
float em(TextStyle s) { return kTextStyles[static_cast<std::size_t>(s)].em; }

} // namespace

TextStyle FxPanelView::ts(TextStyle base) const {
    const float want = em(base) * layout_.zoom * 1.04f;
    TextStyle best = kLadder[0];
    for (TextStyle t : kLadder) if (em(t) <= want) best = t;
    return best;
}

TextStyle FxPanelView::fit(const Renderer& r, TextStyle base, std::string_view text, float width) const {
    TextStyle t = ts(base);
    while (t != kLadder[0] && r.textWidth(t, text) > width) {
        for (std::size_t i = 1; i < std::size(kLadder); ++i)
            if (kLadder[i] == t) { t = kLadder[i - 1]; break; }
    }
    return t;
}

float FxPanelView::smoothed(int meter, float v) const {
    const std::size_t i = static_cast<std::size_t>(meter);
    if (bar_.size() <= i) { bar_.resize(i + 1, v); shown_.resize(i + 1, v); }
    const PanelMeter& m = spec_.meters[i];
    if (m.unit == "note" || ((m.unit == "Hz") && v < 1.0f)) bar_[i] = v;           // a note is a note: no in-between values
    else if (std::fabs(v) >= std::fabs(bar_[i]) && !m.centred) bar_[i] = v;          // rises at once
    else bar_[i] += (v - bar_[i]) * 0.18f;                                         // falls back smoothly
    if (frame_ % 8 == 0) shown_[i] = bar_[i];                                        // the number changes ~7 times a second
    return bar_[i];
}

void FxPanelView::drawCrown(Renderer& r, Rect a) const {
    // A simple five-point crown in neon (our own mark for the HEM plug-ins).
    const float x0 = a.x + a.w * 0.18f, x1 = a.right() - a.w * 0.18f, base = a.y + a.h * 0.68f, top = a.y + a.h * 0.30f, mid = a.y + a.h * 0.50f;
    const float w = x1 - x0;
    const float xs[] = {x0, x0, x0 + w * 0.25f, x0 + w * 0.5f, x0 + w * 0.75f, x1, x1, x0};
    const float ys[] = {base, top, mid, top - a.h * 0.06f, mid, top, base, base};
    const float z = layout_.zoom;
    r.polyline(xs, ys, 8, 7.0f * z, pal_.lime.withAlpha(0.35f), Blend::Add, true);
    r.polyline(xs, ys, 8, 3.5f * z, pal_.lime, Blend::Over, true);
    for (int i : {1, 3, 5}) r.sprite(Sprite::GlowDot, {xs[i] - 9 * z, ys[i] - 9 * z, 18 * z, 18 * z}, pal_.lime, Blend::Add, true);
    r.fillRound({x0, base + 8 * z, w, 6 * z}, 3 * z, pal_.violet, Blend::Over, true);
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
    const bool on = host.active(k.param);
    if (!on) r.fillRound(well.inset(well.w * 0.04f), well.w * 0.46f, rgb8(0x000000, 0.62f), Blend::Over);   // not in use right now
    if (hot) r.glow(well.inset(well.w * 0.12f), well.w * 0.4f, 18.0f, pal_.violet.withAlpha(0.35f), true);

    // value window
    const Rect v = at(k.value, ox, oy);
    r.fillRound(v.inset(3 * layout_.zoom), 7 * layout_.zoom, kGlass, Blend::Over);
    const std::string t = on ? host.text(k.param) : std::string("--");
    const TextStyle vs = fit(r, TextStyle::Title, t, v.w - 12 * layout_.zoom);
    r.text(vs, t, v.cx(), v.cy() + r.capHeight(vs) * 0.5f, !on ? kTextDim : hot ? kText : pal_.lime, Align::Center, on ? TextFx::NeonCrisp : TextFx::Shadowed);
}

void FxPanelView::drawButton(Renderer& r, Rect b, const std::string& text, bool lit, bool hot, Rgba colour) const {
    const float z = layout_.zoom;
    const TextStyle bs = fit(r, TextStyle::Caption, text, b.w - 14 * z);
    if (lit) {
        r.glow(b.inset(4 * z), 10 * z, 12.0f * z, colour.withAlpha(0.40f), true);
        r.fillRound(b.inset(2 * z), 10 * z, colour.withAlpha(0.92f), Blend::Over, false);
        r.fillRound({b.x + 8 * z, b.y + 5 * z, b.w - 16 * z, b.h * 0.35f}, 8 * z, {1, 1, 1, 0.16f}, Blend::Add);
        r.text(bs, text, b.cx(), b.cy() + r.capHeight(bs) * 0.5f, rgb8(0x0A0812), Align::Center, TextFx::Plain, b.w - 14 * z);
    } else {
        r.fillRound(b.inset(2 * z), 10 * z, kGlass, Blend::Over);
        r.fillRound({b.x + 2 * z, b.y + 2 * z, b.w - 4 * z, 2 * z}, 1 * z, colour.withAlpha(hot ? 0.8f : 0.35f), Blend::Over, true);
        r.text(bs, text, b.cx(), b.cy() + r.capHeight(bs) * 0.5f, hot ? kText : kTextDim, Align::Center, TextFx::Shadowed, b.w - 14 * z);
    }
}

void FxPanelView::drawMeter(Renderer& r, const PanelMeterSlot& s, float raw, float ox, float oy) const {
    const PanelMeter& m = spec_.meters[static_cast<std::size_t>(s.meter)];
    const float z = layout_.zoom;
    const float value = smoothed(s.meter, raw), number = shown_[static_cast<std::size_t>(s.meter)];
    const Rect bar = at(s.bar, ox, oy);
    r.fillRound(bar, 8 * z, rgb8(0x020203, 0.95f), Blend::Over);
    // scale ticks
    for (int t = 1; t < 8; ++t) {
        const float y = bar.y + bar.h * static_cast<float>(t) / 8.0f;
        r.fillRound({bar.x + 4 * z, y - 0.75f * z, bar.w - 8 * z, 1.5f * z}, 0.75f * z, {1, 1, 1, 0.07f}, Blend::Over);
    }
    const float f = panelMeterFraction(m, value);
    const Rect inner = bar.inset(5 * z);
    if (m.centred) {
        const float midY = inner.cy();
        r.fillRound({inner.x - 3, midY - 1, inner.w + 6, 2}, 1, {1, 1, 1, 0.35f}, Blend::Over);
        if (std::fabs(f) > 0.002f) {
            const float h = inner.h * 0.5f * std::fabs(f);
            const Rect fill = f > 0 ? Rect{inner.x, midY - h, inner.w, h} : Rect{inner.x, midY, inner.w, h};
            const Rgba c = f > 0 ? pal_.lime : pal_.violet;
            r.glow(fill, 6 * z, 10.0f * z, c.withAlpha(0.45f), true);
            r.fillRound(fill, 5 * z, c, Blend::Over, true);
        }
    } else if (f > 0.002f) {
        const bool fromTop = m.unit == "dB" && m.max <= 0.0f;   // reduction hangs from the top
        const float h = inner.h * f;
        const Rect fill = fromTop ? Rect{inner.x, inner.y, inner.w, h} : Rect{inner.x, inner.bottom() - h, inner.w, h};
        const Rgba c = fromTop ? pal_.violet : pal_.lime;
        r.glow(fill, 6 * z, 10.0f * z, c.withAlpha(0.45f), true);
        r.fillRound(fill, 5 * z, c, Blend::Over, true);
    }
    const Rect vbox = at(s.value, ox, oy);
    r.fillRound(vbox.inset(2 * z), 7 * z, kGlass, Blend::Over);
    const std::string num = formatPanelMeter(m, number);
    const TextStyle ns = fit(r, TextStyle::Title, num, vbox.w - 10 * z);
    r.text(ns, num, vbox.cx(), vbox.cy() + r.capHeight(ns) * 0.5f, pal_.lime, Align::Center, TextFx::NeonCrisp);
    const Rect cap = at(s.caption, ox, oy);
    const TextStyle cs = fit(r, TextStyle::Caption, m.name, cap.w);
    r.text(cs, m.name, cap.cx(), cap.cy() + r.capHeight(cs) * 0.5f, kText, Align::Center, TextFx::Shadowed);
}

void FxPanelView::draw(Renderer& r, const PanelHost& host, PanelHit hover, PanelHit active, float ox, float oy, bool plate) const {
    const PanelLayout& L = layout_;
    const float z = L.zoom;
    ++frame_;
    if (plate) r.panel({ox, oy, L.width, L.height});

    // neon tubes in their channels
    for (const PanelGroove& g : L.grooves) {
        const Rgba c = g.lime ? pal_.lime : pal_.violet;
        const float xs[2] = {g.x0 + ox, g.x1 + ox}, ys[2] = {g.y0 + oy, g.y1 + oy};
        r.glow({std::min(xs[0], xs[1]), std::min(ys[0], ys[1]) - 1 * z, std::fabs(xs[1] - xs[0]), std::fabs(ys[1] - ys[0]) + 2 * z}, 2 * z, 14.0f * z,
               c.withAlpha(0.55f), true);
        r.polyline(xs, ys, 2, 4.0f * z, c, Blend::Over, true);
        r.polyline(xs, ys, 2, 1.6f * z, {1, 1, 1, 0.75f}, Blend::Add, true);
    }
    // header: crown, name, blurb
    drawCrown(r, at(L.logo, ox, oy).inset(12 * z));
    const Rect ta = at(L.titleArea, ox, oy);
    const TextStyle nameStyle = fit(r, TextStyle::Display, L.name, ta.w);
    r.text(nameStyle, L.name, ta.x, ta.y + 92 * z, pal_.lime, Align::Left, TextFx::Neon);
    {   // the description: one line if it fits, else two smaller lines, shortened only as a last resort
        const float room = L.header.right() + ox - 30 * z - (ta.x + 4 * z);
        const TextStyle one = ts(TextStyle::Sub);
        if (r.textWidth(one, L.blurb) <= room) {
            r.text(one, L.blurb, ta.x + 4 * z, ta.y + 140 * z, kText, Align::Left, TextFx::Shadowed);
        } else {
            const TextStyle two = ts(TextStyle::Caption);
            // split at the space that leaves the first line as full as possible
            std::size_t cut = std::string::npos;
            for (std::size_t p = L.blurb.find(' '); p != std::string::npos; p = L.blurb.find(' ', p + 1))
                if (r.textWidth(two, std::string_view(L.blurb).substr(0, p)) <= room) cut = p;
            std::string a = cut == std::string::npos ? L.blurb : L.blurb.substr(0, cut);
            std::string b = cut == std::string::npos ? std::string() : L.blurb.substr(cut + 1);
            while (b.size() > 8 && r.textWidth(two, b) > room) b = b.substr(0, b.size() - 4) + "...";
            r.text(two, a, ta.x + 4 * z, ta.y + 130 * z, kText, Align::Left, TextFx::Shadowed);
            if (!b.empty()) r.text(two, b, ta.x + 4 * z, ta.y + 160 * z, kText, Align::Left, TextFx::Shadowed);
        }
    }
    // section captions
    for (const PanelSection& s : L.sections)
        if (!s.caption.empty()) {
            const Rect c = at(s.captionArea, ox, oy);
            r.text(ts(TextStyle::Label), s.caption, c.x + 8 * z, c.y + 30 * z, pal_.violet, Align::Left, TextFx::Neon);
        }
    // control captions
    for (const PanelCaption& c : L.captions) {
        const Rect a = at(c.r, ox, oy);
        std::string name = spec_.params[static_cast<std::size_t>(c.param)].name;
        for (char& ch : name) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const Rgba col = host.active(c.param) ? kText : kTextDim;
        // Fit the cell: full size, then smaller, then two lines (split at the middle space).
        const TextStyle big = ts(TextStyle::Label), small = ts(TextStyle::Caption);
        if (r.textWidth(big, name) <= a.w) {
            r.text(big, name, a.cx(), a.y + 32 * z, col, Align::Center, TextFx::Shadowed);
        } else if (r.textWidth(small, name) <= a.w) {
            r.text(small, name, a.cx(), a.y + 30 * z, col, Align::Center, TextFx::Shadowed);
        } else {
            std::size_t cut = std::string::npos, best = std::string::npos;
            for (std::size_t p = name.find(' '); p != std::string::npos; p = name.find(' ', p + 1)) {
                const std::size_t d = p > name.size() / 2 ? p - name.size() / 2 : name.size() / 2 - p;
                if (best == std::string::npos || d < best) { best = d; cut = p; }
            }
            if (cut == std::string::npos) {
                r.text(small, name, a.cx(), a.y + 30 * z, col, Align::Center, TextFx::Shadowed);
            } else {
                r.text(small, name.substr(0, cut), a.cx(), a.y + 14 * z, col, Align::Center, TextFx::Shadowed);
                r.text(small, name.substr(cut + 1), a.cx(), a.y + 40 * z, col, Align::Center, TextFx::Shadowed);
            }
        }
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
            drawButton(r, at(b.rect, ox, oy), b.caption, sel == b.choice && host.active(b.param), hot, pal_.violet);
        }
    }
    // selectors
    for (std::size_t i = 0; i < L.selectors.size(); ++i) {
        const PanelSelector& s = L.selectors[i];
        const bool on = host.active(s.param);
        const Rect box = at(s.box, ox, oy);
        r.fillRound(box.inset(3 * z), 9 * z, kGlass, Blend::Over);
        const std::string t = host.text(s.param);
        const TextStyle st = fit(r, TextStyle::Title, t, box.w - 12 * z);
        r.text(st, t, box.cx(), box.cy() + r.capHeight(st) * 0.5f, on ? pal_.lime : kTextDim, Align::Center, on ? TextFx::NeonCrisp : TextFx::Shadowed);
        for (int side = 0; side < 2; ++side) {
            const Rect b = at(side == 0 ? s.prev : s.next, ox, oy);
            const bool hot = hover.index == static_cast<int>(i) &&
                             hover.kind == (side == 0 ? PanelHit::Kind::SelectorPrev : PanelHit::Kind::SelectorNext);
            drawButton(r, b, "", false, hot, pal_.violet);
            const float dir = side == 0 ? -1.0f : 1.0f, cx = b.cx(), cy = b.cy(), sz = 11.0f * z;
            const float xs[4] = {cx - dir * sz * 0.7f, cx + dir * sz * 0.9f, cx - dir * sz * 0.7f, cx - dir * sz * 0.7f};
            const float ys[4] = {cy - sz, cy, cy + sz, cy - sz};
            r.polyline(xs, ys, 4, 3.0f * z, hot ? pal_.lime : pal_.violet, Blend::Over, true);
        }
    }
    // meters
    for (const PanelMeterSlot& m : L.meters) drawMeter(r, m, host.meter(m.meter), ox, oy);
    // footer: brand, how to use the knobs (when there is room), bypass
    const Rect ft = at(L.footer, ox, oy);
    const TextStyle fs = ts(TextStyle::Label);
    const std::string brand = "HYBRID ELITE MAFIA  -  STUDIO PLUG-INS";
    r.text(fs, brand, ft.x + 24 * z, ft.cy() + r.capHeight(fs) * 0.5f, pal_.violet, Align::Left, TextFx::Neon);
    {
        const float x0 = ft.x + 24 * z + r.textWidth(fs, brand) + 40 * z;
        const float x1 = (host.hasBypass() && L.hasBypass ? at(L.bypass, ox, oy).x : ft.right()) - 30 * z;
        const std::string hint = "DRAG A KNOB  -  SHIFT = FINE  -  DOUBLE-CLICK = RESET";
        const TextStyle hs = ts(TextStyle::Caption);
        if (x1 - x0 >= r.textWidth(hs, hint))
            r.text(hs, hint, (x0 + x1) * 0.5f, ft.cy() + r.capHeight(hs) * 0.5f, kTextDim, Align::Center, TextFx::Shadowed);
    }
    if (host.hasBypass() && L.hasBypass) {
        const bool byp = host.bypassed();
        drawButton(r, at(L.bypass, ox, oy), byp ? "BYPASSED" : "BYPASS", byp, hover.kind == PanelHit::Kind::Bypass, pal_.violet);
    }
}

} // namespace hc::ui

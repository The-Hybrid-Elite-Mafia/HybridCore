// HEM COMPRESSOR faceplate: drawing (layout, meters and brand art are in FxPanelCompressor.cpp).
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelView.hpp"
#include "hybrid/ui/Parts.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>

namespace hc::ui {

// =============================================================================================
//  Drawing
// =============================================================================================
namespace {
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kKnobMin = -0.75f * kPi, kKnobMax = 0.75f * kPi;
const Rgba kEngrave = rgb8(0xEDEBE6);          // engraved white lettering
const Rgba kEngraveDim = rgb8(0x8E8C88);
const Rgba kTeal  = rgb8(0x3FE3DA);            // value read-outs
const Rgba kAmber = rgb8(0xFFAE2E);            // accents, IN lamp, needle
const Rgba kRed   = rgb8(0xFF4A3A);
const Rgba kGlassC = rgb8(0x040506, 0.90f);

Rect at(Rect r, float ox, float oy) { return r.offset(ox, oy); }

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
// Letter-spaced text (engraved captions), centred on cx.
void spaced(Renderer& r, TextStyle st, const std::string& s, float cx, float baseline, float gap, Rgba c, TextFx fx) {
    float total = 0;
    for (char ch : s) total += r.textWidth(st, std::string_view(&ch, 1)) + gap;
    total -= gap;
    float x = cx - total * 0.5f;
    for (char ch : s) {
        const std::string_view g(&ch, 1);
        r.text(st, g, x, baseline, c, Align::Left, fx);
        x += r.textWidth(st, g) + gap;
    }
}
// The value under a knob, in the plate's own words ("OFF" for the SC filter below 20 Hz, whole seconds for RATE-S).
std::string knobText(const PanelParam& p, const PanelHost& host, int param) {
    std::string t = host.text(param);
    if (p.id == "scfilter" && std::atof(t.c_str()) < 20.0) return "OFF";
    if (p.id == "fadetime") {
        const double v = std::atof(t.c_str());
        char b[24];
        std::snprintf(b, sizeof b, v < 9.95 ? "%.1f s" : "%.0f s", v);
        return b;
    }
    return t;
}
} // namespace

void FxPanelView::drawCompressorKnob(Renderer& r, const PanelKnob& k, const PanelHost& host, bool hot, float ox, float oy) const {
    const PanelParam& prm = spec_.params[static_cast<std::size_t>(k.param)];
    const float z = layout_.zoom;
    const Rect well = at(k.well, ox, oy);
    const float cx = well.cx(), cy = well.cy(), R = well.w * 0.5f;
    const float n = std::clamp(host.normalized(k.param), 0.0f, 1.0f);
    const float ang = kKnobMin + (kKnobMax - kKnobMin) * n;

    // engraved scale marks just outside the well: one per step on stepped knobs, 11 on smooth ones
    const int marks = k.steps > 1 ? k.steps : 11;
    for (int i = 0; i < marks; ++i) {
        const float f = static_cast<float>(i) / static_cast<float>(marks - 1);
        const float a = kKnobMin + (kKnobMax - kKnobMin) * f;
        const bool major = k.steps > 1 || i % 5 == 0;
        const float r0 = R + 5 * z, r1 = R + (major ? 19.0f : 12.0f) * z;
        const float xs[2] = {cx + std::sin(a) * r0, cx + std::sin(a) * r1}, ys[2] = {cy - std::cos(a) * r0, cy - std::cos(a) * r1};
        const bool on = k.steps > 1 ? std::fabs(f - n) < 0.5f / static_cast<float>(marks - 1) : false;
        r.polyline(xs, ys, 2, (major ? 3.2f : 2.0f) * z, on ? kAmber : kEngrave.withAlpha(major ? 0.85f : 0.55f), Blend::Over, on);
    }
    // the knob: dark spun cap on a knurled silver skirt, in its well
    const float bodyR = R * 0.80f;
    r.sprite(Sprite::KnobShadow, {cx - bodyR * 1.16f + 0.07f * bodyR, cy - bodyR * 1.16f + 0.12f * bodyR, bodyR * 2.32f, bodyR * 2.32f});
    // thin amber arc from the start to the value (centre start on bipolar knobs)
    const float a0 = kKnobMin + (kKnobMax - kKnobMin) * (prm.bipolar ? 0.5f : 0.0f);
    const float ar = R * 0.93f, aw = 2.6f * z;
    if (std::fabs(ang - a0) > 0.004f) r.arc(cx, cy, ar, aw, std::min(a0, ang), std::max(a0, ang), kAmber.withAlpha(0.85f), true);
    const float bd = bodyR * 2.0f * 256.0f / 254.0f;
    r.spriteRotated(Sprite::KnobBody, cx, cy, bd, bd, ang);
    const float len = (knob::kIndicatorTo - knob::kIndicatorFrom) * bodyR;
    const float mid = 0.5f * (knob::kIndicatorTo + knob::kIndicatorFrom) * bodyR;
    const float ih = len * 96.0f / 72.0f, iw = ih * 24.0f / 96.0f;
    r.spriteRotated(Sprite::KnobIndicator, cx + std::sin(ang) * mid, cy - std::cos(ang) * mid, iw, ih, ang, {1.0f, 0.97f, 0.92f, 1.0f}, Blend::Over, true);
    r.sprite(Sprite::KnobLight, {cx - bd / 2, cy - bd / 2, bd, bd});
    // a stepped knob always shows its own step (RELEASE at AUTO is still a choice, not an unused knob)
    const bool on = host.active(k.param) || prm.kind == PanelParam::Kind::Choice;
    if (!on) r.fillRound(well.inset(well.w * 0.06f), well.w * 0.44f, rgb8(0x000000, 0.6f), Blend::Over);
    if (hot) r.glow(well.inset(well.w * 0.14f), well.w * 0.36f, 16.0f * z, kTeal.withAlpha(0.28f), true);

    // caption (engraved) and value window (teal)
    const Rect c = at(k.caption, ox, oy);
    const std::string cap = upper(prm.name);
    const TextStyle cs = fit(r, k.steps > 1 || well.w > 150 * z ? TextStyle::Label : TextStyle::Caption, cap, c.w);
    spaced(r, cs, cap, c.cx(), c.bottom() - 10 * z, 3.0f * z, on ? kEngrave : kEngraveDim, TextFx::Shadowed);
    const Rect v = at(k.value, ox, oy);
    r.fillRound(v.inset(3 * z), 8 * z, kGlassC, Blend::Over);
    const std::string t = on ? knobText(prm, host, k.param) : std::string("--");
    const TextStyle vs = fit(r, TextStyle::Title, t, v.w - 14 * z);
    r.text(vs, t, v.cx(), v.cy() + r.capHeight(vs) * 0.5f, on ? (hot ? kEngrave : kTeal) : kEngraveDim, Align::Center, on ? TextFx::NeonCrisp : TextFx::Shadowed);
}

void FxPanelView::drawCompressor(Renderer& r, const PanelHost& host, PanelHit hover, PanelHit active, float ox, float oy, bool plate) const {
    namespace cf = compressor_face;
    const PanelLayout& L = layout_;
    const float z = L.zoom;
    ++frame_;
    if (plate) r.panel({ox, oy, L.width, L.height});

    // meter readings (through the host only) with the plate's ballistics
    const auto now = std::chrono::steady_clock::now();
    const float dt = last_.time_since_epoch().count() == 0 ? 0.0f : std::chrono::duration<float>(now - last_).count();
    last_ = now;
    const std::size_t nm = spec_.meters.size();
    const float inDb = nm > 0 ? host.meter(0) : -120.0f, outDb = nm > 1 ? host.meter(1) : -120.0f;
    const float grDb = nm > 2 ? -host.meter(2) : 0.0f;
    cmeters_.update(grDb, inDb, outDb, dt);

    // amber neon in the routed channels
    for (const PanelGroove& g : L.grooves) {
        const float xs[2] = {g.x0 + ox, g.x1 + ox}, ys[2] = {g.y0 + oy, g.y1 + oy};
        r.glow({std::min(xs[0], xs[1]), ys[0] - 1 * z, std::fabs(xs[1] - xs[0]), 2 * z}, 2 * z, 12.0f * z, kAmber.withAlpha(0.45f), true);
        r.polyline(xs, ys, 2, 3.5f * z, kAmber, Blend::Over, true);
        r.polyline(xs, ys, 2, 1.3f * z, {1, 1, 1, 0.7f}, Blend::Add, true);
    }

    // title
    {
        const Rect ta = at(L.titleArea, ox, oy);
        const TextStyle big = ts(TextStyle::Display);
        spaced(r, big, "HEM", ta.cx(), ta.y + 150 * z, 14.0f * z, kAmber, TextFx::Neon);
        const TextStyle sub = ts(TextStyle::Panel);
        spaced(r, sub, "COMPRESSOR", ta.cx(), ta.y + 222 * z, 9.0f * z, kEngrave, TextFx::Shadowed);
    }
    // without the brand pictures, the badge spots still say who made it
    if (!panelBrandArt(0) && L.badgeLeft.w > 0) drawCrown(r, at(L.badgeLeft, ox, oy).inset(30 * z));

    // ---- needle meter
    for (const PanelMeterSlot& s : L.meters) {
        if (s.meter != 2) continue;
        const Rect g = at(s.bar, ox, oy);
        r.fillRound(g, 18 * z, rgb8(0x0B0A08, 1.0f), Blend::Over);
        r.glow(g.inset(g.w * 0.16f, g.h * 0.22f), g.h * 0.25f, 60.0f * z, kAmber.withAlpha(0.16f), false);   // warm backlight
        r.pushClip(g);
        const float px = g.cx(), py = g.bottom() + g.h * 0.20f;
        const float arcR = g.h * 0.98f;
        // scale arc, red zone from 12 dB up
        const int segs = 48;
        float xs[segs + 1], ys[segs + 1];
        for (int i = 0; i <= segs; ++i) {
            const float a = cf::needleAngle(cf::kNeedleMaxDb * static_cast<float>(i) / segs);
            xs[i] = px + std::sin(a) * arcR; ys[i] = py - std::cos(a) * arcR;
        }
        r.polyline(xs, ys, segs + 1, 3.0f * z, kEngrave.withAlpha(0.9f), Blend::Over);
        {
            const int from = static_cast<int>(segs * 12.0f / cf::kNeedleMaxDb);
            r.polyline(xs + from, ys + from, segs + 1 - from, 7.0f * z, kRed.withAlpha(0.85f), Blend::Over);
        }
        for (int d = 0; d <= static_cast<int>(cf::kNeedleMaxDb); ++d) {
            const float a = cf::needleAngle(static_cast<float>(d));
            const bool major = d % 2 == 0;
            const float r0 = arcR, r1 = arcR + (major ? 22.0f : 12.0f) * z;
            const float tx[2] = {px + std::sin(a) * r0, px + std::sin(a) * r1}, ty[2] = {py - std::cos(a) * r0, py - std::cos(a) * r1};
            r.polyline(tx, ty, 2, (major ? 3.0f : 2.0f) * z, d >= 12 ? kRed : kEngrave, Blend::Over);
            if (d % 4 == 0) {
                char b[8];
                std::snprintf(b, sizeof b, "%d", d);
                const TextStyle ls = ts(TextStyle::Label);
                const float lr = arcR + 48.0f * z;
                r.text(ls, b, px + std::sin(a) * lr, py - std::cos(a) * lr + r.capHeight(ls) * 0.5f, d >= 12 ? kRed : kEngrave, Align::Center, TextFx::Shadowed);
            }
        }
        const TextStyle mcap = ts(TextStyle::Label);
        spaced(r, mcap, "dB COMPRESSION", g.cx(), g.y + g.h * 0.70f, 4.0f * z, kEngrave.withAlpha(0.9f), TextFx::Shadowed);
        // the needle
        const float a = cf::needleAngle(cmeters_.needleDb);
        const float nx[2] = {px + std::sin(a) * (g.h * 0.32f), px + std::sin(a) * (arcR + 26.0f * z)};
        const float ny[2] = {py - std::cos(a) * (g.h * 0.32f), py - std::cos(a) * (arcR + 26.0f * z)};
        r.polyline(nx, ny, 2, 9.0f * z, rgb8(0x000000, 0.35f), Blend::Over);
        r.polyline(nx, ny, 2, 4.0f * z, kAmber, Blend::Over, true);
        r.polyline(nx, ny, 2, 1.5f * z, {1, 1, 1, 0.8f}, Blend::Add, true);
        r.popClip();
        // glass: sheen and a bright rim
        r.fillRound({g.x + 14 * z, g.y + 10 * z, g.w - 28 * z, g.h * 0.30f}, 14 * z, {1, 1, 1, 0.045f}, Blend::Add);
        r.fillRound({g.x + 4 * z, g.y + 3 * z, g.w - 8 * z, 2 * z}, 1 * z, {1, 1, 1, 0.22f}, Blend::Add);
        // number
        const Rect v = at(s.value, ox, oy);
        r.fillRound(v.inset(3 * z), 8 * z, kGlassC, Blend::Over);
        char b[24];
        std::snprintf(b, sizeof b, "%.1f dB", static_cast<double>(std::round(cmeters_.needleDb * 10.0f) / 10.0f));
        const TextStyle vs = fit(r, TextStyle::Title, b, v.w - 14 * z);
        r.text(vs, b, v.cx(), v.cy() + r.capHeight(vs) * 0.5f, kAmber, Align::Center, TextFx::NeonCrisp);
        const Rect c = at(s.caption, ox, oy);
        const TextStyle cs = ts(TextStyle::Caption);
        r.text(cs, "GAIN REDUCTION", c.right() - 6 * z, c.cy() + r.capHeight(cs) * 0.5f, kEngraveDim, Align::Right, TextFx::Shadowed);
    }

    // ---- LED ladders
    for (const PanelMeterSlot& s : L.meters) {
        if (s.meter > 1) continue;
        const float lvl = s.meter == 0 ? cmeters_.inDb : cmeters_.outDb;
        const int lit = cf::ledsLit(lvl);
        const Rect bar = at(s.bar, ox, oy);
        r.fillRound(bar, 10 * z, rgb8(0x020203, 0.96f), Blend::Over);
        const float pad = 8 * z, gap = 5 * z;
        const float lh = (bar.h - 2 * pad - gap * static_cast<float>(cf::kLeds - 1)) / static_cast<float>(cf::kLeds);
        for (int i = 0; i < cf::kLeds; ++i) {
            const float top = cf::ledThreshold(i);
            const Rgba col = top > -3.0f ? kRed : top > -12.0f ? kAmber : kTeal;
            const Rect led{bar.x + pad, bar.bottom() - pad - static_cast<float>(i + 1) * lh - static_cast<float>(i) * gap, bar.w - 2 * pad, lh};
            if (i < lit) {
                r.glow(led, 3 * z, 8.0f * z, col.withAlpha(0.45f), true);
                r.fillRound(led, 3 * z, col, Blend::Over, true);
                r.fillRound({led.x + 2 * z, led.y + 1 * z, led.w - 4 * z, led.h * 0.35f}, 2 * z, {1, 1, 1, 0.22f}, Blend::Add, true);
            } else {
                r.fillRound(led, 3 * z, col.withAlpha(0.10f), Blend::Over);
            }
        }
        // dB marks beside the ladder, on the knob side
        const bool right = s.meter == 0;
        const TextStyle ms = ts(TextStyle::Caption);
        for (const int db : {0, -6, -12, -18, -24, -36, -54}) {
            const float f = (static_cast<float>(db) - cf::kLadderMinDb) / (cf::kLadderMaxDb - cf::kLadderMinDb);
            const float y = bar.bottom() - pad - f * (bar.h - 2 * pad);
            char b[8];
            std::snprintf(b, sizeof b, "%d", db);
            r.text(ms, b, right ? bar.right() + 10 * z : bar.x - 10 * z, y + r.capHeight(ms) * 0.5f, kEngraveDim, right ? Align::Left : Align::Right,
                   TextFx::Shadowed);
        }
        const Rect v = at(s.value, ox, oy);
        r.fillRound(v.inset(3 * z), 8 * z, kGlassC, Blend::Over);
        char b[16];
        if (lvl <= -60.0f) std::snprintf(b, sizeof b, "--");
        else std::snprintf(b, sizeof b, "%.1f", static_cast<double>(std::round(lvl * 10.0f) / 10.0f));
        const TextStyle vs = fit(r, TextStyle::Title, b, v.w - 12 * z);
        r.text(vs, b, v.cx(), v.cy() + r.capHeight(vs) * 0.5f, kTeal, Align::Center, TextFx::NeonCrisp);
        const Rect c = at(s.caption, ox, oy);
        spaced(r, ts(TextStyle::Label), s.meter == 0 ? "IN" : "OUT", c.cx(), c.cy() + r.capHeight(ts(TextStyle::Label)) * 0.5f, 3.0f * z, kEngrave,
               TextFx::Shadowed);
    }

    // ---- knobs
    for (std::size_t i = 0; i < L.knobs.size(); ++i) {
        const bool hot = (hover.kind == PanelHit::Kind::Knob && hover.index == static_cast<int>(i)) ||
                         (active.kind == PanelHit::Kind::Knob && active.index == static_cast<int>(i));
        drawCompressorKnob(r, L.knobs[i], host, hot, ox, oy);
    }

    // ---- buttons: lit squares with the state inside, the name engraved above
    for (std::size_t i = 0; i < L.buttons.size(); ++i) {
        const PanelButton& b = L.buttons[i];
        const PanelParam& p = spec_.params[static_cast<std::size_t>(b.param)];
        const bool hot = hover.kind == PanelHit::Kind::Button && hover.index == static_cast<int>(i);
        const Rect br = at(b.rect, ox, oy);
        std::string state;
        bool lit;
        if (p.kind == PanelParam::Kind::Choice) {
            const int n = static_cast<int>(p.choices.size());
            const int sel = static_cast<int>(std::lround(host.normalized(b.param) * static_cast<float>(std::max(1, n - 1))));
            lit = sel != 0;
            state = sel == 0 ? "OFF" : p.id == "fade" ? (sel == 1 ? "OUT" : "IN") : upper(p.choices[static_cast<std::size_t>(sel)]);
        } else {
            lit = host.normalized(b.param) >= 0.5f;
            state = lit ? "ON" : "OFF";
        }
        const Rgba lamp = p.id == "in" ? kAmber : p.id == "analog" ? kAmber : kTeal;
        r.fillRound(br, 12 * z, rgb8(0x101113, 1.0f), Blend::Over);
        r.fillRound({br.x + 2 * z, br.y + 2 * z, br.w - 4 * z, 3 * z}, 1.5f * z, {1, 1, 1, hot ? 0.40f : 0.16f}, Blend::Add);
        const Rect lens = br.inset(12 * z);
        const TextStyle st = fit(r, TextStyle::Title, state, lens.w - 10 * z);
        if (lit) {
            r.glow(lens, 8 * z, 16.0f * z, lamp.withAlpha(0.45f), true);
            r.fillRound(lens, 8 * z, lamp.withAlpha(0.95f), Blend::Over, true);
            r.fillRound({lens.x + 4 * z, lens.y + 3 * z, lens.w - 8 * z, lens.h * 0.38f}, 6 * z, {1, 1, 1, 0.20f}, Blend::Add, true);
            r.text(st, state, lens.cx(), lens.cy() + r.capHeight(st) * 0.5f, rgb8(0x14100A), Align::Center, TextFx::Plain);
        } else {
            r.fillRound(lens, 8 * z, lamp.withAlpha(0.10f), Blend::Over);
            r.text(st, state, lens.cx(), lens.cy() + r.capHeight(st) * 0.5f, hot ? kEngrave : kEngraveDim, Align::Center, TextFx::Shadowed);
        }
    }
    for (const PanelCaption& c : L.captions) {
        bool isButton = false;
        for (const PanelButton& b : L.buttons) isButton = isButton || b.param == c.param;
        if (!isButton) continue;
        std::string cap;
        for (const PanelButton& b : L.buttons) if (b.param == c.param) cap = b.caption;
        const Rect a = at(c.r, ox, oy);
        spaced(r, ts(TextStyle::Label), cap, a.cx(), a.bottom() - 6 * z, 3.0f * z, kEngrave, TextFx::Shadowed);
    }

    // ---- footer
    const Rect ft = at(L.footer, ox, oy);
    const TextStyle fs = ts(TextStyle::Caption);
    r.text(fs, "HYBRID ELITE MAFIA", ft.x + 30 * z, ft.cy() + r.capHeight(fs) * 0.5f, kAmber, Align::Left, TextFx::NeonCrisp);
    {
        const float x0 = ft.x + 30 * z + r.textWidth(fs, "HYBRID ELITE MAFIA") + 30 * z;
        const float x1 = (host.hasBypass() && L.hasBypass ? at(L.bypass, ox, oy).x : ft.right()) - 20 * z;
        const std::string hint = "DRAG = TURN   SHIFT = FINE   DOUBLE-CLICK = RESET";
        if (x1 - x0 >= r.textWidth(fs, hint))
            r.text(fs, hint, (x0 + x1) * 0.5f, ft.cy() + r.capHeight(fs) * 0.5f, kEngraveDim, Align::Center, TextFx::Shadowed);
    }
    if (host.hasBypass() && L.hasBypass) {
        const bool byp = host.bypassed();
        drawButton(r, at(L.bypass, ox, oy), byp ? "BYPASSED" : "BYPASS", byp, hover.kind == PanelHit::Kind::Bypass, kAmber);
    }
}

} // namespace hc::ui

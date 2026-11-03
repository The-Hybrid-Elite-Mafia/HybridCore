// Plug-in faceplates (hc::ui::FxPanel): every effect lays out cleanly, the controls work through a
// PanelHost, meters read right, and the obsidian plate renders (CPU part; the GPU part is checked by
// hc_fx_snapshot in CI).
#include "TestHarness.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/ui/FxPanel.hpp"

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

namespace hc::test {
void testFxPanelCompressor();
namespace {
bool overlaps(const ui::Rect& a, const ui::Rect& b) {
    return a.x < b.right() - 0.5f && b.x < a.right() - 0.5f && a.y < b.bottom() - 0.5f && b.y < a.bottom() - 0.5f;
}
bool inside(const ui::Rect& a, float w, float h) { return a.x >= 0 && a.y >= 0 && a.right() <= w && a.bottom() <= h; }
} // namespace

void testFxPanel() {
    std::printf("[ui] plug-in faceplates: layout of all 23 effects, knobs / buttons / selectors, meters, obsidian plate\n");
    int clean = 0;
    for (const fx::EffectInfo& info : fx::effects()) {
        auto e = info.create();
        const ui::PanelSpec spec = ui::panelSpecFor(info, *e);
        const ui::PanelLayout L = ui::layoutFxPanel(spec);
        std::vector<ui::Rect> controls;
        for (const auto& k : L.knobs) { controls.push_back(k.well); controls.push_back(k.value); }
        for (const auto& b : L.buttons) controls.push_back(b.rect);
        for (const auto& s : L.selectors) { controls.push_back(s.box); controls.push_back(s.prev); controls.push_back(s.next); }
        for (const auto& m : L.meters) { controls.push_back(m.bar); controls.push_back(m.value); controls.push_back(m.caption); }
        controls.push_back(L.bypass);
        bool ok = L.width >= 1000 && L.height >= 600 && L.width <= 4096 && L.height <= 4096;
        for (std::size_t i = 0; i < controls.size(); ++i) {
            ok = ok && inside(controls[i], L.width, L.height);
            for (std::size_t j = i + 1; j < controls.size(); ++j) ok = ok && !overlaps(controls[i], controls[j]);
        }
        // every parameter has exactly one place on the plate, every meter a bar
        std::vector<int> seen(spec.params.size(), 0);
        for (const auto& k : L.knobs) ++seen[static_cast<std::size_t>(k.param)];
        for (const auto& s : L.selectors) ++seen[static_cast<std::size_t>(s.param)];
        for (const auto& b : L.buttons) if (b.choice <= 0) ++seen[static_cast<std::size_t>(b.param)];
        for (int v : seen) ok = ok && v == 1;
        ok = ok && L.meters.size() == spec.meters.size() && L.captions.size() == spec.params.size();
        if (!ok) std::printf("  FAIL detail: %s layout\n", info.name);
        clean += ok;
    }
    CHECK(clean == static_cast<int>(fx::effects().size()));

    // Zoomed plates (the FX rack fills its space): every rectangle scales, nothing starts to overlap,
    // and dragging a knob the same distance on screen does the same thing at any zoom.
    int zoomedOk = 0;
    for (const fx::EffectInfo& info : fx::effects()) {
        auto e = info.create();
        const ui::PanelSpec spec = ui::panelSpecFor(info, *e);
        const ui::PanelLayout a = ui::layoutFxPanel(spec), b = ui::layoutFxPanel(spec, 1.7f), c = ui::layoutFxPanel(spec, 0.6f);
        bool ok = b.zoom == 1.7f && std::fabs(b.width - a.width * 1.7f) < 0.01f && std::fabs(c.height - a.height * 0.6f) < 0.01f;
        ok = ok && b.knobs.size() == a.knobs.size() && b.pockets.size() == a.pockets.size() && b.meters.size() == a.meters.size();
        for (std::size_t i = 0; i < a.knobs.size() && ok; ++i)
            ok = std::fabs(b.knobs[i].well.x - a.knobs[i].well.x * 1.7f) < 0.01f && std::fabs(b.knobs[i].value.w - a.knobs[i].value.w * 1.7f) < 0.01f;
        for (std::size_t i = 0; i < a.buttons.size() && ok; ++i) ok = std::fabs(b.buttons[i].rect.bottom() - a.buttons[i].rect.bottom() * 1.7f) < 0.01f;
        ok = ok && std::fabs(b.bypass.right() - a.bypass.right() * 1.7f) < 0.01f;
        zoomedOk += ok;
    }
    CHECK(zoomedOk == static_cast<int>(fx::effects().size()));

    // Controls through a host, on the real HEM TUNE.
    const auto list = fx::effects();
    std::size_t tuneIdx = 0;
    for (std::size_t i = 0; i < list.size(); ++i) if (std::string(list[i].name) == "HEM TUNE") tuneIdx = i;
    auto tune = list[tuneIdx].create();
    const ui::PanelSpec spec = ui::panelSpecFor(list[tuneIdx], *tune);
    const ui::PanelLayout L = ui::layoutFxPanel(spec);
    ui::EffectPanelHost host(*tune);
    ui::FxPanelController c(L, spec, host);
    // knob: drag up a quarter of the full range, then double-click back to the default
    const ui::PanelKnob* speed = nullptr;
    for (const auto& k : L.knobs) if (spec.params[static_cast<std::size_t>(k.param)].name == "Retune Speed") speed = &k;
    CHECK(speed != nullptr);
    if (speed) {
        const float before = host.normalized(speed->param);
        c.mouseDown(speed->well.cx(), speed->well.cy(), false, false);
        c.mouseMove(speed->well.cx(), speed->well.cy() - ui::FxPanelController::kDragUnitsForFullRange * 0.25f, false);
        c.mouseUp(speed->well.cx(), speed->well.cy());
        CHECK(std::fabs(host.normalized(speed->param) - (before + 0.25f)) < 1e-3f);
        c.mouseDown(speed->well.cx(), speed->well.cy(), true, false);
        CHECK(std::fabs(host.normalized(speed->param) - spec.params[static_cast<std::size_t>(speed->param)].defNormalized) < 1e-5f);
        c.wheel(speed->well.cx(), speed->well.cy(), 2.0f, false);
        CHECK(host.normalized(speed->param) > spec.params[static_cast<std::size_t>(speed->param)].defNormalized);
    }
    // scale buttons (3 options) and the 12-key selector
    for (const auto& b : L.buttons)
        if (b.choice == 2) { c.mouseDown(b.rect.cx(), b.rect.cy(), false, false); CHECK(tune->get(b.param) == 2.0f); }
    CHECK(L.selectors.size() == 1);
    if (!L.selectors.empty()) {
        const auto& s = L.selectors[0];
        c.mouseDown(s.next.cx(), s.next.cy(), false, false);
        CHECK(host.text(s.param) == "C#");
        c.mouseDown(s.prev.cx(), s.prev.cy(), false, false);
        c.mouseDown(s.prev.cx(), s.prev.cy(), false, false);
        CHECK(host.text(s.param) == "B");   // wraps around
    }
    c.mouseDown(L.bypass.cx(), L.bypass.cy(), false, false);
    CHECK(host.bypassed());

    // The same knob drag on a zoomed plate: twice the zoom, twice the distance for the same change.
    {
        auto t2 = list[tuneIdx].create();
        const ui::PanelLayout Z = ui::layoutFxPanel(spec, 2.0f);
        ui::EffectPanelHost h2(*t2);
        ui::FxPanelController c2(Z, spec, h2);
        const ui::PanelKnob* k = nullptr;
        for (const auto& kk : Z.knobs) if (spec.params[static_cast<std::size_t>(kk.param)].name == "Retune Speed") k = &kk;
        CHECK(k != nullptr);
        if (k) {
            const float before = h2.normalized(k->param);
            c2.mouseDown(k->well.cx(), k->well.cy(), false, false);
            c2.mouseMove(k->well.cx(), k->well.cy() - ui::FxPanelController::kDragUnitsForFullRange * 2.0f * 0.25f, false);
            c2.mouseUp(k->well.cx(), k->well.cy());
            CHECK(std::fabs(h2.normalized(k->param) - (before + 0.25f)) < 1e-3f);
        }
    }

    // Controls that do nothing right now are reported (and dimmed): HEM ECHO's TIME vs TEMPO SYNC.
    {
        std::size_t echoIdx = 0;
        for (std::size_t i = 0; i < list.size(); ++i) if (std::string(list[i].name) == "HEM ECHO") echoIdx = i;
        auto echo = list[echoIdx].create();
        ui::EffectPanelHost eh(*echo);
        int time = -1, sync = -1, note = -1;
        const auto ps = echo->params();
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (std::string(ps[i].id) == "time") time = static_cast<int>(i);
            if (std::string(ps[i].id) == "sync") sync = static_cast<int>(i);
            if (std::string(ps[i].id) == "note") note = static_cast<int>(i);
        }
        CHECK(time >= 0 && sync >= 0 && note >= 0);
        echo->set(sync, 1.0f);
        CHECK(!eh.active(time) && eh.active(note) && eh.active(sync));
        echo->set(sync, 0.0f);
        CHECK(eh.active(time) && !eh.active(note));
    }

    // meter text
    const ui::PanelMeter note{"NOTE", "note", 36, 84}, cents{"C", "cents", -100, 100, true}, gr{"GR", "dB", -24, 0};
    CHECK(ui::formatPanelMeter(note, 69.0f) == "A4" && ui::formatPanelMeter(note, 0.0f) == "--");
    CHECK(ui::formatPanelMeter(cents, -31.3f) == "-31" && std::fabs(ui::panelMeterFraction(cents, -50.0f) + 0.5f) < 1e-6f);
    CHECK(std::fabs(ui::panelMeterFraction(gr, -12.0f) - 0.5f) < 1e-6f);
    // zero never shows a sign; silence shows "--" on loudness meters
    const ui::PanelMeter lufs{"I", "LUFS", -36, 0}, tp{"TP", "dBTP", -24, 3}, centred{"S", "dB", -15, 15, true};
    CHECK(ui::formatPanelMeter(gr, -0.0f) == "0.0" && ui::formatPanelMeter(gr, -0.04f) == "0.0" && ui::formatPanelMeter(gr, -0.06f) == "-0.1");
    CHECK(ui::formatPanelMeter(centred, 0.0f) == "0.0" && ui::formatPanelMeter(centred, 1.0f) == "+1.0");
    CHECK(ui::formatPanelMeter(cents, -0.2f) == "0");
    CHECK(ui::formatPanelMeter(lufs, -120.0f) == "--" && ui::formatPanelMeter(lufs, -14.04f) == "-14.0" && ui::formatPanelMeter(tp, -120.0f) == "--");

    // the obsidian plate: right size, opaque, not flat, pockets darker than the slab
    const ui::Image img = ui::renderFxPlate(L, ui::FxPalette{}, 0.5f);
    CHECK(img.width == static_cast<std::uint32_t>(std::lround(L.width * 0.5f)) && img.height == static_cast<std::uint32_t>(std::lround(L.height * 0.5f)));
    bool opaque = true;
    double sum = 0, sq = 0;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) {
        opaque = opaque && img.rgba[i + 3] == 255;
        const double l = img.rgba[i + 1];
        sum += l; sq += l * l;
    }
    const double n = static_cast<double>(img.rgba.size() / 4), mean = sum / n, sd = std::sqrt(std::max(0.0, sq / n - mean * mean));
    std::printf("  plate %ux%u: mean %.1f, spread %.1f (8-bit green)\n", img.width, img.height, mean, sd);
    CHECK(opaque && mean > 3.0 && mean < 90.0 && sd > 2.0);
    testFxPanelCompressor();
}

// HEM COMPRESSOR's own faceplate: every control is where it should be, hit-testable, and moves the
// right parameter of the real engine; the needle and LED ladders follow the engine's own readings.
void testFxPanelCompressor() {
    std::printf("[ui] HEM COMPRESSOR faceplate: controls, needle, LED ladders, brand art\n");
    namespace cf = ui::compressor_face;
    const auto list = fx::effects();
    std::size_t ci = list.size();
    for (std::size_t i = 0; i < list.size(); ++i) if (std::string(list[i].name) == "HEM COMPRESSOR") ci = i;
    CHECK(ci < list.size());
    if (ci >= list.size()) return;
    auto e = list[ci].create();
    e->prepare(48000.0, 512);
    const ui::PanelSpec spec = ui::panelSpecFor(list[ci], *e);
    const ui::PanelLayout L = ui::layoutFxPanel(spec);
    CHECK(spec.style == ui::PanelStyle::Compressor && L.style == ui::PanelStyle::Compressor);
    CHECK(L.height > L.width && L.knobs.size() == 9 && L.buttons.size() == 3 && L.meters.size() == 3 && spec.meters.size() == 3);
    CHECK(L.badgeLeft.w > 100 && L.badgeRight.w > 100);
    // the knob grid: columns left / centre / right, rows top to bottom, as Steven's plate
    const char* const grid[3][3] = {{"threshold", "attack", "ratio"}, {"scfilter", "mix", "trim"}, {"makeup", "release", "fadetime"}};
    auto knobOf = [&](const char* id) -> const ui::PanelKnob* {
        for (const auto& k : L.knobs) if (spec.params[static_cast<std::size_t>(k.param)].id == id) return &k;
        return nullptr;
    };
    bool gridOk = true;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r) {
            const ui::PanelKnob* k = knobOf(grid[c][r]);
            gridOk = gridOk && k != nullptr;
            if (!k) continue;
            if (c > 0) { const ui::PanelKnob* l = knobOf(grid[c - 1][r]); gridOk = gridOk && l && l->well.cx() < k->well.cx(); }
            if (r > 0) { const ui::PanelKnob* u = knobOf(grid[c][r - 1]); gridOk = gridOk && u && u->well.cy() < k->well.cy(); }
            if (c == 1) { const ui::PanelKnob* l = knobOf(grid[0][r]); gridOk = gridOk && l && k->well.w < l->well.w; }   // centre knobs are small
        }
    CHECK(gridOk);
    {   // no caption sits on another control's value window, well or button (captions are drawn inside their rects)
        bool clear = true;
        for (const auto& cap : L.captions) {
            for (const auto& k : L.knobs) clear = clear && !overlaps(cap.r, k.value) && !overlaps(cap.r, k.well);
            for (const auto& b : L.buttons) clear = clear && !overlaps(cap.r, b.rect);
            for (const auto& m : L.meters) clear = clear && !overlaps(cap.r, m.bar) && !overlaps(cap.r, m.value);
        }
        CHECK(clear);
    }
    CHECK(knobOf("ratio") && knobOf("ratio")->steps == 3 && knobOf("attack")->steps == 6 && knobOf("release")->steps == 5 && knobOf("threshold")->steps == 0);
    // IN ladder left of everything, OUT right of everything, needle glass between them
    CHECK(L.meters[0].bar.right() < L.knobs[0].well.x && L.meters[1].bar.x > knobOf("makeup")->well.right());
    CHECK(L.meters[2].bar.x > L.meters[0].bar.right() && L.meters[2].bar.right() < L.meters[1].bar.x);

    // every control: hit-test lands on it, and turning it moves that parameter and nothing else
    ui::EffectPanelHost host(*e);
    ui::FxPanelController c(L, spec, host);
    const auto ps = e->params();
    auto snapshot = [&] { std::vector<float> v; for (std::size_t i = 0; i < ps.size(); ++i) v.push_back(e->get(static_cast<int>(i))); return v; };
    int knobsOk = 0;
    for (std::size_t i = 0; i < L.knobs.size(); ++i) {
        const ui::PanelKnob& k = L.knobs[i];
        const ui::PanelHit h = ui::hitTestPanel(L, k.well.cx(), k.well.cy());
        const ui::PanelHit hv = ui::hitTestPanel(L, k.value.cx(), k.value.cy());
        e->setDefaults();
        const auto before = snapshot();
        const auto& p = ps[static_cast<std::size_t>(k.param)];
        // drag: down to the bottom first, then up a third of the way
        c.mouseDown(k.well.cx(), k.well.cy(), false, false);
        c.mouseMove(k.well.cx(), k.well.cy() + ui::FxPanelController::kDragUnitsForFullRange * 1.2f, false);
        const float low = e->get(k.param);
        c.mouseMove(k.well.cx(), k.well.cy() + ui::FxPanelController::kDragUnitsForFullRange * 0.87f, false);
        c.mouseUp(k.well.cx(), k.well.cy());
        const float up = e->get(k.param);
        auto after = snapshot();
        bool others = true;
        for (std::size_t j = 0; j < ps.size(); ++j) if (static_cast<int>(j) != k.param) others = others && after[j] == before[j];
        bool ok = h.kind == ui::PanelHit::Kind::Knob && h.index == static_cast<int>(i) && hv.kind == ui::PanelHit::Kind::Knob && hv.index == static_cast<int>(i);
        ok = ok && low == p.min && up > low && others;
        if (k.steps > 1) ok = ok && up == std::round(up);   // stepped knobs land on a step
        // wheel: one notch; stepped knobs move exactly one step
        const float w0 = e->get(k.param);
        c.wheel(k.well.cx(), k.well.cy(), 1.0f, false);
        const float w1 = e->get(k.param);
        ok = ok && w1 > w0;
        if (k.steps > 1) ok = ok && w1 == w0 + 1.0f;
        // fine drag (Shift) moves less than a normal drag of the same distance
        e->set(k.param, p.min);
        c.mouseDown(k.well.cx(), k.well.cy(), false, true);
        c.mouseMove(k.well.cx(), k.well.cy() - ui::FxPanelController::kDragUnitsForFullRange * 0.4f, true);
        c.mouseUp(k.well.cx(), k.well.cy());
        const float fineV = e->get(k.param);
        e->set(k.param, p.min);
        c.mouseDown(k.well.cx(), k.well.cy(), false, false);
        c.mouseMove(k.well.cx(), k.well.cy() - ui::FxPanelController::kDragUnitsForFullRange * 0.4f, false);
        c.mouseUp(k.well.cx(), k.well.cy());
        const float coarse = e->get(k.param);
        ok = ok && fineV <= coarse && coarse > p.min;
        // double-click: back to the default
        c.mouseDown(k.well.cx(), k.well.cy(), true, false);
        ok = ok && e->get(k.param) == p.def;
        if (!ok) std::printf("  FAIL detail: knob %s (low %g up %g wheel %g->%g fine %g coarse %g)\n", p.id, low, up, w0, w1, fineV, coarse);
        knobsOk += ok;
    }
    CHECK(knobsOk == 9);
    // buttons: IN and ANALOG toggle, FADE cycles OFF -> FADE OUT -> FADE IN -> OFF
    e->setDefaults();
    for (const auto& b : L.buttons) {
        const ui::PanelHit h = ui::hitTestPanel(L, b.rect.cx(), b.rect.cy());
        CHECK(h.kind == ui::PanelHit::Kind::Button && L.buttons[static_cast<std::size_t>(h.index)].param == b.param);
        const std::string id = ps[static_cast<std::size_t>(b.param)].id;
        if (id == "fade") {
            std::string seq = host.text(b.param);
            for (int k = 0; k < 3; ++k) { c.mouseDown(b.rect.cx(), b.rect.cy(), false, false); c.mouseUp(b.rect.cx(), b.rect.cy()); seq += ">" + host.text(b.param); }
            std::printf("  FADE button: %s\n", seq.c_str());
            CHECK(seq == "OFF>FADE OUT>FADE IN>OFF");
        } else {
            const float v0 = e->get(b.param);
            c.mouseDown(b.rect.cx(), b.rect.cy(), false, false); c.mouseUp(b.rect.cx(), b.rect.cy());
            const float v1 = e->get(b.param);
            c.mouseDown(b.rect.cx(), b.rect.cy(), false, false); c.mouseUp(b.rect.cx(), b.rect.cy());
            CHECK(v0 == 1.0f && v1 == 0.0f && e->get(b.param) == 1.0f);
        }
    }
    c.mouseDown(L.bypass.cx(), L.bypass.cy(), false, false);
    CHECK(host.bypassed());

    // needle and ladders follow the engine's own readings
    {
        auto x = list[ci].create();
        x->prepare(48000.0, 512);
        ui::EffectPanelHost xh(*x);
        auto setId = [&](const char* id, float v) { for (std::size_t i = 0; i < ps.size(); ++i) if (std::string(ps[i].id) == id) x->set(static_cast<int>(i), v); };
        setId("threshold", -10.0f); setId("ratio", 2.0f); setId("analog", 0.0f);
        x->reset();
        std::vector<float> l(512), r(512);
        const float amp = std::pow(10.0f, -6.0f / 20.0f);
        for (int b = 0; b < 94; ++b) {
            for (int i = 0; i < 512; ++i) l[static_cast<std::size_t>(i)] = r[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * 1000.0 * (b * 512 + i) / 48000.0));
            x->process(l.data(), r.data(), 512);
        }
        cf::MeterState m;
        m.update(-xh.meter(2), xh.meter(0), xh.meter(1), 0.0f);
        std::printf("  engine: GR %.2f dB, IN %.2f dBFS, OUT %.2f dBFS -> needle %.2f dB (%.3f rad), IN LEDs %d, OUT LEDs %d of %d\n",
                    -x->reductionDb(), x->meter(0), x->meter(1), m.needleDb, cf::needleAngle(m.needleDb), cf::ledsLit(m.inDb), cf::ledsLit(m.outDb), cf::kLeds);
        CHECK(m.needleDb > 5.0f && std::fabs(m.needleDb + x->reductionDb()) < 1e-4f);
        CHECK(std::fabs(m.inDb - x->meter(0)) < 1e-4f && std::fabs(m.outDb - x->meter(1)) < 1e-4f);
        CHECK(cf::ledsLit(m.inDb) == 21 && cf::ledsLit(m.outDb) < cf::ledsLit(m.inDb));
        // the needle swings from the left end (0 dB) to the right end (20 dB) and stops there
        CHECK(std::fabs(cf::needleAngle(0.0f) + cf::kNeedleSweep) < 1e-6f && std::fabs(cf::needleAngle(20.0f) - cf::kNeedleSweep) < 1e-6f);
        CHECK(cf::needleAngle(35.0f) == cf::needleAngle(20.0f) && cf::needleAngle(m.needleDb) > cf::needleAngle(0.0f));
        CHECK(cf::ledsLit(0.0f) == cf::kLeds && cf::ledsLit(-120.0f) == 0 && cf::ledsLit(-53.0f) == 0);
        // ballistics: back toward 0 slower than it rises, LEDs fall at a fixed rate
        cf::MeterState b = m;
        b.update(0.0f, -120.0f, -120.0f, 0.1f);
        cf::MeterState up; up.update(0.0f, -120.0f, -120.0f, 0.0f); up.update(m.needleDb, -6.0f, -6.0f, 0.1f);
        std::printf("  ballistics after 100 ms: falling needle %.2f dB (from %.2f), rising needle %.2f dB (to %.2f), IN LEDs fall to %.1f dBFS\n",
                    b.needleDb, m.needleDb, up.needleDb, m.needleDb, b.inDb);
        CHECK(b.needleDb < m.needleDb && b.needleDb > 0.5f * m.needleDb && up.needleDb > 0.7f * m.needleDb);
        CHECK(std::fabs(b.inDb - (m.inDb - cf::kLedFallDbPerS * 0.1f)) < 1e-3f);
        for (int k = 0; k < 60; ++k) b.update(0.0f, -120.0f, -120.0f, 0.05f);
        CHECK(b.needleDb < 0.01f && cf::ledsLit(b.inDb) == 0);
    }

    // brand art: Steven's crown and ghost are found and baked into the plate
    ui::setPanelArtDir(assetDir());
    const ui::Image* crown = ui::panelBrandArt(0);
    const ui::Image* ghost = ui::panelBrandArt(1);
    CHECK(crown != nullptr && ghost != nullptr);
    if (crown && ghost) {
        const ui::Image plate = ui::renderFxPlate(L, ui::FxPalette{}, 0.5f);
        // the crown is gold: in the middle of the left badge, red clearly beats blue; the ghost's eyes are cyan-blue
        double rs = 0, bs = 0, gb = 0, gr = 0;
        const ui::Rect bl = L.badgeLeft, br = L.badgeRight;
        for (float yy = bl.y + bl.h * 0.3f; yy < bl.y + bl.h * 0.6f; yy += 2.0f)
            for (float xx = bl.x + bl.w * 0.3f; xx < bl.x + bl.w * 0.7f; xx += 2.0f) {
                const std::uint8_t* px = plate.at(static_cast<std::uint32_t>(xx * 0.5f), static_cast<std::uint32_t>(yy * 0.5f));
                rs += px[0]; bs += px[2];
            }
        for (float yy = br.y; yy < br.bottom(); yy += 2.0f)
            for (float xx = br.x; xx < br.right(); xx += 2.0f) {
                const std::uint8_t* px = plate.at(static_cast<std::uint32_t>(xx * 0.5f), static_cast<std::uint32_t>(yy * 0.5f));
                gb += px[2]; gr += px[0];
            }
        std::printf("  brand art: crown %ux%u, ghost %ux%u; crown area red/blue %.2f, ghost area blue/red %.2f\n", crown->width, crown->height,
                    ghost->width, ghost->height, rs / std::max(bs, 1.0), gb / std::max(gr, 1.0));
        CHECK(rs > bs * 1.3 && gb > gr * 1.3);
        CHECK(crown->width <= 1024 && crown->height <= 1024);   // masters kept small in RAM; the plate itself is window-sized
    }
    // zoomed (FX rack): the badges scale with everything else
    const ui::PanelLayout Z = ui::layoutFxPanel(spec, 0.6f);
    CHECK(std::fabs(Z.badgeLeft.w - L.badgeLeft.w * 0.6f) < 0.01f && std::fabs(Z.meters[2].bar.x - L.meters[2].bar.x * 0.6f) < 0.01f);
}

} // namespace hc::test

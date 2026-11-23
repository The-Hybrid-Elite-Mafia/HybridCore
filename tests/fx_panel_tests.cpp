// Plug-in faceplates (hc::ui::FxPanel): every effect lays out cleanly, the controls work through a
// PanelHost, meters read right, and the obsidian plate renders (CPU part; the GPU part is checked by
// hc_fx_snapshot in CI).
#include "TestHarness.hpp"
#include "hybrid/fx/Effects.hpp"
#include "hybrid/synth/Wave.hpp"
#include "hybrid/ui/FxPanel.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hc::test {
namespace {
bool overlaps(const ui::Rect& a, const ui::Rect& b) {
    return a.x < b.right() - 0.5f && b.x < a.right() - 0.5f && a.y < b.bottom() - 0.5f && b.y < a.bottom() - 0.5f;
}
bool inside(const ui::Rect& a, float w, float h) { return a.x >= 0 && a.y >= 0 && a.right() <= w && a.bottom() <= h; }
} // namespace

void testFxPanel() {
    std::printf("[ui] plug-in faceplates: layout of all 22 effects, knobs / buttons / selectors, meters, obsidian plate\n");
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
}


namespace {
// HEM WAVE's faceplate description (same groups as the plug-in and the snapshot tool).
ui::PanelSpec framedWaveSpec() {
    ui::PanelSpec s;
    s.name = "HEM WAVE";
    s.blurb = "Morphing wavetable synth: 8 tables, unison, sub, filter, two envelopes, LFO";
    s.hasBypass = false;
    for (const synth::ParamInfo& p : synth::WaveSynth::params()) {
        ui::PanelParam pp;
        pp.name = p.name;
        if (p.taper == synth::Taper::Toggle) pp.kind = ui::PanelParam::Kind::Toggle;
        else if (p.taper == synth::Taper::Choice) {
            pp.kind = ui::PanelParam::Kind::Choice;
            for (int c = 0; c <= static_cast<int>(p.max); ++c) pp.choices.emplace_back(p.choices ? p.choices[c] : std::to_string(c));
        }
        pp.defNormalized = synth::toNormalized(p, p.def);
        s.params.push_back(std::move(pp));
    }
    ui::PanelParam preset;
    preset.name = "Preset";
    preset.kind = ui::PanelParam::Kind::Choice;
    for (int i = 0; i < synth::WaveSynth::presetCount(); ++i) preset.choices.emplace_back(synth::WaveSynth::presetName(i));
    s.params.push_back(std::move(preset));
    using P = synth::Param;
    s.groups = {{"OSCILLATOR", P::Table, P::FilterType - P::Table}, {"FILTER", P::FilterType, P::AmpAttack - P::FilterType},
                {"AMP ENVELOPE", P::AmpAttack, P::ModAttack - P::AmpAttack}, {"MOD ENVELOPE", P::ModAttack, P::LfoShape - P::ModAttack},
                {"LFO", P::LfoShape, P::VelAmp - P::LfoShape}, {"VOICE", P::VelAmp, P::kParamCount - P::VelAmp + 1}};
    return s;
}
bool within(const ui::Rect& a, const ui::Rect& box) {
    return a.x >= box.x - 0.5f && a.y >= box.y - 0.5f && a.right() <= box.right() + 0.5f && a.bottom() <= box.bottom() + 0.5f;
}
} // namespace

// The HEM frame look (Steven's template): every faceplate scaled into the glass bed. Same controls,
// same parameters, same order; nothing under the emblem, nothing outside the bed, the top-strip
// bypass where the frame has room for it, and clicks still land on the right control.
void testFxPanelFramed() {
    std::printf("[ui] plug-in faceplates in the HEM frame: all 22 effects + HEM WAVE fit the glass bed\n");
    std::vector<ui::PanelSpec> specs;
    for (const fx::EffectInfo& info : fx::effects()) {
        auto e = info.create();
        specs.push_back(ui::panelSpecFor(info, *e));
    }
    specs.push_back(framedWaveSpec());
    int clean = 0;
    float smallest = 99.0f, largest = 0.0f;
    for (const ui::PanelSpec& spec : specs) {
        const ui::PanelLayout F = ui::layoutFxPanelFramed(spec);
        const ui::PanelLayout A = ui::layoutFxPanel(spec);
        bool ok = F.framed && F.width == ui::fxframe::kW && F.height == ui::fxframe::kH && F.zoom > 0.3f && F.zoom <= ui::fxframe::kMaxScale + 1e-4f;
        smallest = std::min(smallest, F.zoom);
        largest = std::max(largest, F.zoom);
        // same controls in the same order
        ok = ok && F.knobs.size() == A.knobs.size() && F.buttons.size() == A.buttons.size() && F.selectors.size() == A.selectors.size()
                && F.meters.size() == A.meters.size() && F.captions.size() == A.captions.size() && F.sections.size() == A.sections.size();
        for (std::size_t i = 0; ok && i < F.knobs.size(); ++i) ok = F.knobs[i].param == A.knobs[i].param;
        for (std::size_t i = 0; ok && i < F.buttons.size(); ++i) ok = F.buttons[i].param == A.buttons[i].param && F.buttons[i].choice == A.buttons[i].choice;
        for (std::size_t i = 0; ok && i < F.selectors.size(); ++i) ok = F.selectors[i].param == A.selectors[i].param;
        // inside the content box, clear of the emblem, no two controls on top of each other
        std::vector<ui::Rect> controls;
        for (const auto& k : F.knobs) { controls.push_back(k.well); controls.push_back(k.value); }
        for (const auto& b : F.buttons) controls.push_back(b.rect);
        for (const auto& sel : F.selectors) { controls.push_back(sel.box); controls.push_back(sel.prev); controls.push_back(sel.next); }
        for (const auto& m : F.meters) { controls.push_back(m.bar); controls.push_back(m.value); controls.push_back(m.caption); }
        for (const auto& sec : F.sections) ok = ok && within(sec.bay, ui::fxframe::kContent) && !overlaps(sec.bay, ui::fxframe::kEmblem);
        if (F.meterBay.w > 0) ok = ok && within(F.meterBay, ui::fxframe::kContent);
        for (std::size_t i = 0; i < controls.size(); ++i) {
            ok = ok && within(controls[i], ui::fxframe::kContent) && !overlaps(controls[i], ui::fxframe::kEmblem);
            for (std::size_t j = i + 1; j < controls.size(); ++j) ok = ok && !overlaps(controls[i], controls[j]);
        }
        for (const auto& p : F.pockets) ok = ok && (within(p.r, ui::fxframe::kBed) || (spec.hasBypass && within(p.r, ui::fxframe::kBypass.inset(-7))));
        // bypass in the top strip (effects only), never on a control
        if (spec.hasBypass) {
            ok = ok && F.bypass.x == ui::fxframe::kBypass.x && F.bypass.w == ui::fxframe::kBypass.w;
            for (const auto& c : controls) ok = ok && !overlaps(c, F.bypass);
        } else {
            ok = ok && F.bypass.w == 0;
        }
        // clicks land on the right control
        for (std::size_t i = 0; ok && i < F.knobs.size(); ++i) {
            const ui::PanelHit h = ui::hitTestPanel(F, F.knobs[i].well.cx(), F.knobs[i].well.cy());
            ok = h.kind == ui::PanelHit::Kind::Knob && h.index == static_cast<int>(i);
        }
        for (std::size_t i = 0; ok && i < F.buttons.size(); ++i) {
            const ui::PanelHit h = ui::hitTestPanel(F, F.buttons[i].rect.cx(), F.buttons[i].rect.cy());
            ok = h.kind == ui::PanelHit::Kind::Button && h.index == static_cast<int>(i);
        }
        if (spec.hasBypass) ok = ok && ui::hitTestPanel(F, ui::fxframe::kBypass.cx(), ui::fxframe::kBypass.cy()).kind == ui::PanelHit::Kind::Bypass;
        if (!ok) std::printf("  FAIL detail: %s framed layout (zoom %.2f)\n", spec.name.c_str(), F.zoom);
        clean += ok;
    }
    std::printf("  %zu faceplates framed; control scale %.2f .. %.2f\n", specs.size(), smallest, largest);
    CHECK(clean == static_cast<int>(specs.size()));

    // the framed plate: frame size, the art untouched outside the bed, pockets cut into the bed
    {
        const ui::PanelLayout F = ui::layoutFxPanelFramed(specs.front());
        ui::Image art;
        art.width = 384; art.height = 216;
        art.rgba.assign(static_cast<std::size_t>(art.width) * art.height * 4, 255);
        for (std::size_t i = 0; i < art.rgba.size(); i += 4) { art.rgba[i] = 120; art.rgba[i + 1] = 124; art.rgba[i + 2] = 130; }
        const float ppu = 0.25f;
        const ui::Image img = ui::renderFxPlateFramed(F, ui::hemFramePalette(), ppu, art);
        CHECK(img.width == 960 && img.height == 540);
        auto px = [&](float x, float y) { return img.at(static_cast<std::uint32_t>(x * ppu), static_cast<std::uint32_t>(y * ppu)); };
        const std::uint8_t* corner = px(60, 60);   // the chrome frame, outside the bed
        CHECK(corner[0] == 120 && corner[1] == 124 && corner[2] == 130 && corner[3] == 255);
        const ui::Rect& w = F.knobs.front().well;
        const std::uint8_t* well = px(w.cx(), w.cy());   // a knob well: machined obsidian, far darker than the art
        CHECK(well[1] < 60);
        const ui::Rect& bay = F.sections.front().bay;
        const std::uint8_t* glass = px(bay.x + 12, bay.bottom() - 12);   // smoked glass panel: darker than the art, not black
        CHECK(glass[1] < 100 && glass[1] > 5);
        const ui::Image none = ui::renderFxPlateFramed(F, ui::hemFramePalette(), ppu, ui::Image{});   // no art on disk: still a plate
        CHECK(none.width == 960 && none.height == 540 && none.rgba[3] == 255);
    }
}

} // namespace hc::test

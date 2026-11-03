// HEM COMPRESSOR's own faceplate (PanelStyle::Compressor): a portrait plate of dark brushed titanium.
//   top    : Steven's crown badge (left), "HEM" / "COMPRESSOR", his hooded ghost (right)
//   middle : a glass needle meter "dB COMPRESSION" (0..20 dB of gain reduction) + a number,
//            tall IN (left) and OUT (right) LED ladders
//   knobs  : THRESHOLD / ATTACK / RATIO (left), SC FILTER / MIX / TRIM (centre, small),
//            MAKE-UP / RELEASE / RATE-S (right); engraved scale marks, value under each knob
// This file: layout, meter ballistics, brand art (pure, unit-tested). Drawing: FxPanelCompressorView.cpp.
//   bottom : lit square buttons IN, ANALOG, FADE (OFF / OUT / IN), then BYPASS
// Layout, ballistics and brand art are pure (unit-tested); drawing uses the Renderer only.
// Every control goes through the same PanelHost / FxPanelController as the automatic plates.
#include "hybrid/ui/FxPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <numbers>
#include <optional>

namespace hc::ui {

// =============================================================================================
//  Meters: needle angle, LED ladder, ballistics
// =============================================================================================
namespace compressor_face {

float needleAngle(float reductionDb) noexcept {
    const float f = std::clamp(reductionDb / kNeedleMaxDb, 0.0f, 1.0f);
    return -kNeedleSweep + 2.0f * kNeedleSweep * f;
}

float ledThreshold(int i) noexcept {
    return kLadderMinDb + (kLadderMaxDb - kLadderMinDb) * static_cast<float>(i + 1) / static_cast<float>(kLeds);
}

int ledsLit(float dbfs) noexcept {
    int n = 0;
    for (int i = 0; i < kLeds; ++i) if (dbfs >= ledThreshold(i) - 1e-4f) n = i + 1;
    return n;
}

void MeterState::update(float reductionDb, float inDbfs, float outDbfs, float dt) noexcept {
    const float gr = std::clamp(reductionDb, 0.0f, 40.0f);
    inDbfs = std::max(inDbfs, -120.0f);
    outDbfs = std::max(outDbfs, -120.0f);
    if (!primed) {
        needleDb = gr; inDb = inDbfs; outDb = outDbfs; primed = true;
        return;
    }
    dt = std::clamp(dt, 0.0f, 0.25f);
    const float tau = gr > needleDb ? kNeedleRiseS : kNeedleFallS;
    needleDb += (gr - needleDb) * (1.0f - std::exp(-dt / tau));
    const float drop = kLedFallDbPerS * dt;
    inDb = inDbfs >= inDb ? inDbfs : std::max(inDbfs, inDb - drop);
    outDb = outDbfs >= outDb ? outDbfs : std::max(outDbfs, outDb - drop);
}

} // namespace compressor_face

// =============================================================================================
//  Brand art (crown badge, ghost): decoded once, baked into the plate
// =============================================================================================
namespace {
struct BrandArt {
    std::mutex            mutex;
    std::filesystem::path dir;
    bool                  tried[2] = {false, false};
    std::optional<Image>  img[2];
};
BrandArt& brand() {
    static BrandArt b;
    return b;
}
} // namespace

void setPanelArtDir(const std::filesystem::path& assetDir) {
    BrandArt& b = brand();
    std::lock_guard lock(b.mutex);
    if (b.dir == assetDir) return;
    b.dir = assetDir;
    b.tried[0] = b.tried[1] = false;
    b.img[0].reset(); b.img[1].reset();
}

const Image* panelBrandArt(int which) {
    if (which < 0 || which > 1) return nullptr;
    BrandArt& b = brand();
    std::lock_guard lock(b.mutex);
    const auto i = static_cast<std::size_t>(which);
    if (!b.tried[i] && !b.dir.empty()) {
        b.tried[i] = true;
        if (auto im = loadImageFile(b.dir / "brand" / (which == 0 ? "crown_badge.png" : "ghost.png"))) {
            // Keep the master small: the plate never needs more than 1024 px for a badge.
            if (im->width > 1024 || im->height > 1024) {
                const float s = 1024.0f / static_cast<float>(std::max(im->width, im->height));
                *im = resizeSrgb(*im, std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(im->width) * s))),
                                 std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(im->height) * s))));
            }
            b.img[i] = std::move(*im);
        }
    }
    return b.img[i] ? &*b.img[i] : nullptr;
}

// Alpha-blends a picture (fit inside `area`, centred; `round` = cut to a circle) onto the opaque plate, in linear light.
void blendArt(Image& plate, const Image& art, Rect area, float ppu, bool round) {
    const float x0 = area.x * ppu, y0 = area.y * ppu, w = area.w * ppu, h = area.h * ppu;
    const float s = std::min(w / static_cast<float>(art.width), h / static_cast<float>(art.height));
    const auto tw = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(art.width) * s)));
    const auto th = std::max(1u, static_cast<std::uint32_t>(std::lround(static_cast<float>(art.height) * s)));
    const Image src = resizeSrgb(art, tw, th);
    const int ox = static_cast<int>(std::lround(x0 + (w - static_cast<float>(tw)) * 0.5f));
    const int oy = static_cast<int>(std::lround(y0 + (h - static_cast<float>(th)) * 0.5f));
    const float rad = 0.5f * static_cast<float>(std::min(tw, th)) * 0.985f;
    for (std::uint32_t y = 0; y < th; ++y) {
        const int py = oy + static_cast<int>(y);
        if (py < 0 || py >= static_cast<int>(plate.height)) continue;
        for (std::uint32_t x = 0; x < tw; ++x) {
            const int px = ox + static_cast<int>(x);
            if (px < 0 || px >= static_cast<int>(plate.width)) continue;
            const std::uint8_t* sp = src.at(x, y);
            float a = static_cast<float>(sp[3]) / 255.0f;
            if (round) {
                const float dx = static_cast<float>(x) + 0.5f - static_cast<float>(tw) * 0.5f, dy = static_cast<float>(y) + 0.5f - static_cast<float>(th) * 0.5f;
                a *= std::clamp(rad - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            }
            if (a <= 0.0f) continue;
            std::uint8_t* d = &plate.rgba[(static_cast<std::size_t>(py) * plate.width + static_cast<std::size_t>(px)) * 4];
            for (int c = 0; c < 3; ++c)
                d[c] = linearToSrgb8(srgb8ToLinear(sp[c]) * a + srgb8ToLinear(d[c]) * (1.0f - a));
        }
    }
}

void compositeBrandArt(const PanelLayout& L, Image& img, float ppu) {
    if (L.badgeLeft.w > 0) if (const Image* a = panelBrandArt(0)) blendArt(img, *a, L.badgeLeft, ppu, true);
    if (L.badgeRight.w > 0) if (const Image* a = panelBrandArt(1)) blendArt(img, *a, L.badgeRight, ppu, false);
}

// =============================================================================================
//  Layout (design units at zoom 1; layoutFxPanel() scales it)
// =============================================================================================
namespace {
constexpr float kW = 1200.0f, kH = 1880.0f;
constexpr float kWell = 14.0f, kWin = 7.0f, kBtn = 8.0f, kBay = 5.0f;

int paramById(const PanelSpec& spec, const char* id) {
    for (std::size_t i = 0; i < spec.params.size(); ++i) if (spec.params[i].id == id) return static_cast<int>(i);
    return -1;
}
} // namespace

PanelLayout layoutCompressorPanel(const PanelSpec& spec) {
    PanelLayout L;
    L.style = PanelStyle::Compressor;
    L.name = spec.name;
    L.blurb = spec.blurb;
    L.width = kW;
    L.height = kH;
    L.hasBypass = spec.hasBypass;

    // ---- header: badges and title
    L.badgeLeft  = {62, 58, 226, 226};
    L.badgeRight = {912, 52, 226, 238};
    L.logo = {};
    L.titleArea = {300, 56, 600, 240};
    L.header = {40, 40, kW - 80, 262};
    L.pockets.push_back({L.badgeLeft.inset(-8), L.badgeLeft.w * 0.5f + 8, 9, true, true});
    L.pockets.push_back({L.badgeRight.inset(-8), 28, 9, false, false});
    L.grooves.push_back({330, 300, kW - 330, 300, false});

    // ---- meters: IN ladder (0), OUT ladder (1), needle glass (2)
    auto ladder = [&](int m, float x) {
        PanelMeterSlot s;
        s.meter = m;
        s.value = {x, 320, 112, 44};
        s.bar = {x + 21, 374, 70, 1186};
        s.caption = {x, 1570, 112, 40};
        L.pockets.push_back({s.bar.inset(-8), 14, kBay + 10, false, false});
        L.pockets.push_back({s.value, 10, kWin, false, false});
        L.meters.push_back(s);
    };
    ladder(0, 46);
    ladder(1, kW - 46 - 112);
    {
        PanelMeterSlot s;
        s.meter = 2;
        s.bar = {204, 322, 792, 362};
        s.value = {512, 698, 176, 50};
        s.caption = {300, 698, 200, 50};
        L.pockets.push_back({s.bar.inset(-9), 22, kBay + 12, false, false});
        L.pockets.push_back({s.value, 10, kWin, false, false});
        L.meters.push_back(s);
    }
    L.meterBay = L.meters.back().bar;

    // ---- knobs
    struct K { const char* id; float cx, cy; bool small; };
    const K ks[] = {
        {"threshold", 340, 912, false}, {"attack", 340, 1182, false}, {"ratio", 340, 1452, false},
        {"scfilter", 600, 912, true},   {"mix", 600, 1182, true},     {"trim", 600, 1452, true},
        {"makeup", 860, 912, false},    {"release", 860, 1182, false}, {"fadetime", 860, 1452, false},
    };
    for (const K& k : ks) {
        const int p = paramById(spec, k.id);
        if (p < 0) continue;
        PanelKnob kn;
        kn.param = p;
        const float r = k.small ? 64.0f : 94.0f;
        kn.well = {k.cx - r, k.cy - r, 2 * r, 2 * r};
        kn.caption = k.small ? Rect{k.cx - 110, k.cy - 124, 220, 44} : Rect{k.cx - 130, k.cy - 152, 260, 44};
        kn.value = k.small ? Rect{k.cx - 82, k.cy + 80, 164, 44} : Rect{k.cx - 90, k.cy + 104, 180, 46};
        const PanelParam& pp = spec.params[static_cast<std::size_t>(p)];
        if (pp.kind == PanelParam::Kind::Choice) kn.steps = static_cast<int>(pp.choices.size());
        L.pockets.push_back({kn.well, r, kWell, true, true});
        L.pockets.push_back({kn.value, 10, kWin, false, false});
        L.captions.push_back({p, kn.caption});
        L.knobs.push_back(kn);
    }

    // ---- buttons: IN, ANALOG, FADE (one lit square each; FADE cycles its three steps)
    struct B { const char* id; float cx; const char* caption; };
    const B bs[] = {{"in", 360, "IN"}, {"analog", 600, "ANALOG"}, {"fade", 840, "FADE"}};
    for (const B& b : bs) {
        const int p = paramById(spec, b.id);
        if (p < 0) continue;
        PanelButton pb{p, -1, {b.cx - 92, 1650, 184, 92}, b.caption};
        L.pockets.push_back({pb.rect.inset(-7), 14, kBtn, false, false});
        L.captions.push_back({p, {b.cx - 110, 1610, 220, 36}});
        L.buttons.push_back(pb);
    }
    // anything the plate does not place by name still gets a knob (no control may go missing)
    int extra = 0;
    for (std::size_t i = 0; i < spec.params.size(); ++i) {
        bool placed = false;
        for (const auto& k : L.knobs) placed = placed || k.param == static_cast<int>(i);
        for (const auto& b : L.buttons) placed = placed || b.param == static_cast<int>(i);
        if (placed) continue;
        const float cx = 160.0f + 150.0f * static_cast<float>(extra++);
        PanelKnob kn;
        kn.param = static_cast<int>(i);
        kn.well = {cx - 50, 1770, 100, 100};
        kn.value = {cx - 60, 1870, 120, 40};
        kn.caption = {cx - 70, 1730, 140, 40};
        L.captions.push_back({kn.param, kn.caption});
        L.knobs.push_back(kn);
        L.height = std::max(L.height, 1960.0f);
    }

    // ---- footer, bypass, screws
    L.footer = {40, 1776, kW - 80, 70};
    L.bypass = {950, 1780, 196, 62};
    if (spec.hasBypass) L.pockets.push_back({L.bypass.inset(-6), 14, kBtn, false, false});
    L.grooves.push_back({80, 1764, kW - 80, 1764, false});
    const float si = 30.0f;
    L.screws = {{si, si}, {L.width - si, si}, {si, L.height - si}, {L.width - si, L.height - si}};
    return L;
}

} // namespace hc::ui

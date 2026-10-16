// UI tests (CPU side of the GPU UI): linear-light image pipeline, chassis bake and VRAM
// caps, faceplate measurements checked against the real Main-Faceplate.png, canvas fitting,
// parts atlas (cut-outs equal the PNG, lighting direction, determinism), layout geometry,
// mouse interaction and the Poppins atlas.
#include "TestHarness.hpp"

#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/MixerView.hpp"
#include "hybrid/ui/Parts.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace hc::test {

namespace {
std::filesystem::path gExeDir;

ui::Image solid(std::uint32_t w, std::uint32_t h, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
    ui::Image img;
    img.width = w; img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) { img.rgba[i] = r; img.rgba[i + 1] = g; img.rgba[i + 2] = b; img.rgba[i + 3] = a; }
    return img;
}
ui::Image gradient(std::uint32_t w, std::uint32_t h) {
    ui::Image img = solid(w, h, 0, 0, 0, 255);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / std::max(1u, w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / std::max(1u, h - 1));
            p[2] = static_cast<std::uint8_t>((x + y) & 255);
        }
    return img;
}
double luma(const ui::Image& img, float x0, float y0, float x1, float y1) {
    double s = 0;
    std::size_t n = 0;
    for (auto y = static_cast<std::uint32_t>(y0); y < static_cast<std::uint32_t>(y1); ++y)
        for (auto x = static_cast<std::uint32_t>(x0); x < static_cast<std::uint32_t>(x1); ++x) {
            const std::uint8_t* p = img.at(x, y);
            s += 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            ++n;
        }
    return n ? s / static_cast<double>(n) : 0.0;
}
// Pixel of a sprite in the atlas (level 0), sprite-local coordinates.
const std::uint8_t* spritePx(const ui::PartsAtlas& a, ui::Sprite s, std::uint32_t x, std::uint32_t y) {
    const ui::SpriteInfo& i = a[s];
    const auto ax = static_cast<std::uint32_t>(std::lround(i.u0 * a.texture.width)) + x;
    const auto ay = static_cast<std::uint32_t>(std::lround(i.v0 * a.texture.height)) + y;
    return &a.texture.levels[0][(static_cast<std::size_t>(ay) * a.texture.width + ax) * 4];
}
std::optional<ui::FaceplateArt> realArt() {
    std::string err;
    auto art = ui::loadFaceplateArt(assetDir() / "faceplates" / ui::kFaceplateFile, &err);
    if (!art) std::printf("  faceplate not found: %s\n", err.c_str());
    return art;
}
} // namespace

void setExePath(const char* argv0) {
    std::error_code ec;
    gExeDir = std::filesystem::absolute(std::filesystem::path(argv0), ec).parent_path();
}
std::filesystem::path assetDir() {
    std::error_code ec;
    for (const auto& d : {gExeDir / "assets", gExeDir.parent_path() / "assets"})
        if (std::filesystem::is_directory(d / "faceplates", ec)) return d;
    return gExeDir / "assets";
}

void testUiImage() {
    std::printf("[ui image pipeline]\n");
    int roundTrip = 0;
    for (int v = 0; v < 256; ++v) roundTrip += ui::linearToSrgb8(ui::srgb8ToLinear(static_cast<std::uint8_t>(v))) == v ? 1 : 0;
    CHECK(roundTrip == 256);

    // Premultiplied in LINEAR light: 50 % white is linear 0.5 -> sRGB 188, not 128.
    const ui::Image pm = ui::premultiply(solid(4, 4, 255, 255, 255, 128));
    CHECK(pm.rgba[0] == 188 && pm.rgba[3] == 128);

    // Mip of half opaque white / half transparent: premultiplied average = linear 0.5.
    ui::Image chk = solid(2, 2, 0, 0, 0, 0);
    for (int i : {0, 3}) { chk.rgba[i * 4] = chk.rgba[i * 4 + 1] = chk.rgba[i * 4 + 2] = 255; chk.rgba[i * 4 + 3] = 255; }
    const ui::TextureLevels cm = ui::buildMips(chk);
    CHECK(cm.levels.size() == 2 && cm.levels[1][0] == 188 && cm.levels[1][3] == 128);

    const ui::TextureLevels g = ui::buildMips(solid(3840, 2160, 120, 120, 120, 255));
    CHECK(g.levels.size() == 12);                      // 3840x2160 -> 1x1
    CHECK(g.levels.back()[0] == 120 && g.levels.back()[3] == 255);

    // Chassis copy for the GPU: never more than the window needs, never above 4096 px.
    CHECK(ui::chassisTextureWidth(0.25f) == 960);
    CHECK(ui::chassisTextureWidth(1389.0f / 3840.0f) == 1920);
    CHECK(ui::chassisTextureWidth(0.5f) == 1920);
    CHECK(ui::chassisTextureWidth(2560.0f / 3840.0f) == 2560);
    CHECK(ui::chassisTextureWidth(1.0f) == 3840);
    CHECK(ui::chassisTextureWidth(2.0f) == 3840);
    ui::FaceplateArt fake;
    fake.master = gradient(3840, 2160);
    const auto t0 = std::chrono::steady_clock::now();
    const ui::TextureLevels laptop = ui::bakeChassis(fake, 1920);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(laptop.width == 1920 && laptop.height == 1080 && laptop.levels.size() == 11);
    const ui::TextureLevels full = ui::bakeChassis(fake, 3840);
    CHECK(full.width <= ui::kMaxTextureSide && full.height <= ui::kMaxTextureSide);
    std::printf("  chassis for a 1389-wide window: %ux%u + mips = %.1f MB (bake %.0f ms); for 4K: %.1f MB\n", laptop.width, laptop.height,
                static_cast<double>(laptop.bytes()) / 1048576.0, ms, static_cast<double>(full.bytes()) / 1048576.0);
    CHECK(full.bytes() < 48ull * 1024 * 1024);

    ui::TextureBudget budget(100);
    CHECK(budget.reserve(60));
    CHECK(!budget.reserve(41));
    budget.release(60);
    CHECK(budget.reserve(100) && budget.used() == 100);
}

void testUiFaceplate() {
    std::printf("[ui faceplate + canvas]\n");
    // Canvas fitting.
    const ui::Canvas k4 = ui::Canvas::fit(3840, 2160);
    CHECK(k4.scale == 1.0f && k4.extraH == 0.0f && k4.offsetX == 0.0f && k4.offsetY == 0.0f);
    const ui::Canvas lap = ui::Canvas::fit(1389, 868);
    CHECK(std::fabs(lap.scale - 1389.0f / 3840.0f) < 1e-6f);
    CHECK(std::fabs(lap.extraH - (868.0f / lap.scale - 2160.0f)) < 0.01f && lap.extraH > 230.0f);
    CHECK(std::fabs(lap.toPixelY(lap.height()) - 868.0f) < 0.5f);   // the canvas fills the window exactly
    CHECK(lap.plateY(500) == 500 && std::fabs(lap.plateY(2100) - (2100 + lap.extraH)) < 1e-3f);
    const ui::Canvas wide = ui::Canvas::fit(2560, 1080);          // 21:9 -> pillarbox
    CHECK(wide.scale == 0.5f && wide.offsetX == 320.0f && wide.visibleLeft() == -640.0f && wide.extraH == 0.0f);
    const ui::Canvas p1440 = ui::Canvas::fit(2560, 1440);
    CHECK(std::fabs(p1440.scale - 2.0f / 3.0f) < 1e-6f && p1440.extraH == 0.0f);

    // The measurements must match the real art (guards against a changed PNG).
    const auto art = realArt();
    CHECK(art.has_value());
    if (!art) return;
    CHECK(art->master.width == 3840 && art->master.height == 2160);
    const auto& m = art->master;
    using namespace ui::plate;
    const double glass = luma(m, kDisplayGlass.x + 300, kDisplayGlass.y + 60, kDisplayGlass.right() - 40, kDisplayGlass.bottom() - 30);
    const double name  = luma(m, kNamePanel.x + 40, kNamePanel.y + 30, kNamePanel.right() - 40, kNamePanel.bottom() - 30);
    const double main  = luma(m, kMainPanel.x + 40, kMainPanel.y + 40, kMainPanel.right() - 40, kMainPanel.bottom() - 40);
    const double left  = luma(m, kLeftPanel.x + 30, kLeftPanel.y + 40, kLeftPanel.right() - 30, kLeftPanel.bottom() - 40);
    std::printf("  measured regions: glass %.0f, name panel %.0f, main panel %.0f, left panel %.0f (luma 0..255)\n", glass, name, main, left);
    CHECK(glass < 45.0 && name > 120.0 && main > 80.0 && main < 140.0 && left > 80.0 && left < 140.0);
    for (const auto& t : kTabFaces) {
        const double tab = luma(m, t.x + 40, t.y + 30, t.right() - 40, t.bottom() - 30);
        CHECK(tab > 100.0 && tab < 200.0);
    }
    for (const auto& s : kSockets) {   // dark rim at the inner radius, lighter cup inside
        double rim = 0;
        for (int a = 0; a < 72; ++a) {
            const float th = static_cast<float>(a) * 0.0872665f;
            rim += luma(m, s.cx + s.r * std::cos(th), s.cy + s.r * std::sin(th), s.cx + s.r * std::cos(th) + 1, s.cy + s.r * std::sin(th) + 1);
        }
        CHECK(rim / 72.0 < 40.0 && luma(m, s.cx - 20, s.cy - 20, s.cx + 20, s.cy + 20) > 100.0);
    }

    // Skins: a user file in <data>/skins wins; wrong aspect is refused; small skins are scaled up.
    const auto dir = std::filesystem::temp_directory_path() / "hc_ui_skin_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir / "skins", ec);
    CHECK(ui::resolveFaceplate("A", dir) == std::filesystem::path("A") / "faceplates" / ui::kFaceplateFile);
    CHECK(ui::writePng(dir / "skins" / "Main-Faceplate.png", gradient(1920, 1080)));
    CHECK(ui::resolveFaceplate("A", dir) == dir / "skins" / "Main-Faceplate.png");
    std::string err;
    const auto skin = ui::loadFaceplateArt(dir / "skins" / "Main-Faceplate.png", &err);
    CHECK(skin && skin->master.width == 3840 && skin->master.height == 2160);
    CHECK(ui::writePng(dir / "square.png", gradient(1000, 1000)));
    CHECK(!ui::loadFaceplateArt(dir / "square.png", &err) && !err.empty());
    std::filesystem::remove_all(dir, ec);
}

void testUiParts() {
    std::printf("[ui parts atlas]\n");
    const auto art = realArt();
    CHECK(art.has_value());
    if (!art) return;
    const auto t0 = std::chrono::steady_clock::now();
    const ui::PartsAtlas a = ui::buildPartsAtlas(art->master);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  atlas %ux%u, %zu sprites, %.1f MB with mips, built in %.0f ms\n", a.texture.width, a.texture.height, ui::kSpriteCount,
                static_cast<double>(a.texture.bytes()) / 1048576.0, ms);
    CHECK(a.texture.width <= 1024 && a.texture.height <= 1024);
    int bad = 0;
    for (std::size_t i = 0; i < ui::kSpriteCount; ++i) {
        const ui::SpriteInfo& s = a.sprites[i];
        if (!(s.width > 0 && s.height > 0 && s.u0 >= 0 && s.v0 >= 0 && s.u1 <= 1 && s.v1 <= 1 && s.u1 > s.u0 && s.v1 > s.v0)) ++bad;
        for (std::size_t j = i + 1; j < ui::kSpriteCount; ++j) {
            const ui::SpriteInfo& o = a.sprites[j];
            if (s.u0 < o.u1 && o.u0 < s.u1 && s.v0 < o.v1 && o.v0 < s.v1) ++bad;   // overlap
        }
    }
    CHECK(bad == 0);
    CHECK(a[ui::Sprite::Glass].slice[0] == 60.0f && a[ui::Sprite::Glass].width == 136.0f);

    // Cut-outs are the faceplate's own pixels.
    const std::uint8_t* sc = spritePx(a, ui::Sprite::Socket, 100, 100);
    const std::uint8_t* fp = art->master.at(static_cast<std::uint32_t>(ui::plate::kSockets[2].cx), static_cast<std::uint32_t>(ui::plate::kSockets[2].cy));
    CHECK(std::abs(sc[0] - fp[0]) <= 1 && std::abs(sc[1] - fp[1]) <= 1 && sc[3] == 255);
    const std::uint8_t* gl = spritePx(a, ui::Sprite::Glass, 68, 68);   // middle sample: x 3350+8, y 340+8
    const std::uint8_t* gp = art->master.at(3358, 348);
    CHECK(std::abs(gl[0] - gp[0]) <= 1 && std::abs(gl[2] - gp[2]) <= 1 && gl[3] == 255);
    CHECK(spritePx(a, ui::Sprite::Glass, 1, 1)[3] < 40);   // outside the rounded bezel corner is transparent

    // One light from the top-left: the fixed knob light darkens the lower-right skirt more and
    // puts more specular on the upper-left skirt.
    const std::uint8_t* tl = spritePx(a, ui::Sprite::KnobLight, 128 - 80, 128 - 80);
    const std::uint8_t* br = spritePx(a, ui::Sprite::KnobLight, 128 + 80, 128 + 80);
    CHECK(br[3] > tl[3] && tl[0] > br[0]);
    const std::uint8_t* fct = spritePx(a, ui::Sprite::FaderCap, 128, 26);    // top bevel
    const std::uint8_t* fcb = spritePx(a, ui::Sprite::FaderCap, 128, 110);   // bottom bevel
    CHECK(fct[1] > fcb[1]);

    // Deterministic: the same art gives the same atlas, byte for byte.
    const ui::PartsAtlas b = ui::buildPartsAtlas(art->master);
    CHECK(a.texture.levels[0] == b.texture.levels[0]);
}

void testUiLayout() {
    std::printf("[ui layout + interaction]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    for (float extra : {0.0f, 240.0f}) {
        ui::Layout L(tree, extra);
        CHECK(L.strips().size() == kStemCount + kBusCount);
        const ui::Rect mainP{ui::plate::kMainPanel.x, ui::plate::kMainPanel.y, ui::plate::kMainPanel.w, ui::plate::kMainPanel.h + extra};
        const ui::Rect leftP{ui::plate::kLeftPanel.x, ui::plate::kLeftPanel.y, ui::plate::kLeftPanel.w, ui::plate::kLeftPanel.h + extra};
        auto inside = [](ui::Rect in, ui::Rect out) { return in.x >= out.x && in.y >= out.y && in.right() <= out.right() + 0.01f && in.bottom() <= out.bottom() + 0.01f; };
        int outside = 0, overlap = 0;
        for (std::size_t i = 0; i < L.strips().size(); ++i) {
            const auto& s = L.strips()[i];
            if (!inside(s.bed, s.isBus ? leftP : mainP)) ++outside;
            if (!inside(s.faderTrack, s.bed) || !inside(s.meter, s.bed) || !inside(s.valueBox, s.bed)) ++outside;
            if (s.faderTrack.right() > s.meter.x) ++overlap;
            for (std::size_t j = i + 1; j < L.strips().size(); ++j)
                if (s.bed.x < L.strips()[j].bed.right() && L.strips()[j].bed.x < s.bed.right()) ++overlap;
        }
        CHECK(outside == 0 && overlap == 0);
        int knobs = 0, knobsOut = 0;
        for (const auto& w : L.widgets()) {
            if (w.kind != ui::WidgetKind::Knob && w.kind != ui::WidgetKind::SendKnob && w.kind != ui::WidgetKind::Button) continue;
            if (w.caption == "LIM") continue;   // bottom bar
            ++knobs;
            if (!inside(w.rect, L.strips()[w.kind == ui::WidgetKind::Button && w.caption == "M" && w.rect.x < mainP.x ? kStemCount + w.index : w.index].bed)) ++knobsOut;
        }
        CHECK(knobs == static_cast<int>(kStemCount * 8 + kBusCount) && knobsOut == 0);
        CHECK(L.strips()[0].bed.h == ui::plate::kMainPanel.h - 28.0f + extra);
    }

    ui::Layout L(tree, 0.0f);
    ui::Interaction in(L, tree);
    // Sockets and tabs sit on the measured faceplate positions.
    std::uint32_t idx = 99;
    {
        const ui::UiAction want[6] = {ui::UiAction::LoadStems, ui::UiAction::ToStart, ui::UiAction::PlayPause,
                                      ui::UiAction::Stop, ui::UiAction::ToggleLoop, ui::UiAction::CycleSource};
        int ok = 0;
        for (std::uint32_t i = 0; i < 6; ++i) {
            idx = 99;
            if (in.mouseDown(ui::plate::kSockets[i].cx + 50, ui::plate::kSockets[i].cy, false, false, &idx) == want[i] && idx == i) ++ok;
            in.mouseUp();
        }
        CHECK(ok == 6);
    }
    CHECK(in.mouseDown(ui::plate::kNamePanel.cx(), ui::plate::kNamePanel.cy(), false, false) == ui::UiAction::NextDevice);
    in.mouseUp();
    {   // Song overview: click = seek to that fraction, drag keeps seeking, release stops.
        const ui::Rect o = L.overview();
        float f = -1.0f;
        CHECK(!in.takeSeek(f));
        CHECK(in.mouseDown(o.x + o.w * 0.25f, o.cy(), false, false) == ui::UiAction::Seek);
        CHECK(in.takeSeek(f) && std::fabs(f - 0.25f) < 1e-4f);
        in.mouseMove(o.x + o.w * 2.0f, o.cy(), false);   // past the end clamps to 1
        CHECK(in.takeSeek(f) && f == 1.0f);
        in.mouseUp();
        in.mouseMove(o.x + o.w * 0.5f, o.cy(), false);
        CHECK(!in.takeSeek(f));
        CHECK(o.x >= ui::plate::kBottomBar.x && o.right() <= L.masterMeter().x);
    }
    CHECK(in.mouseDown(ui::plate::kTabFaces[2].cx(), ui::plate::kTabFaces[2].cy(), false, false, &idx) == ui::UiAction::SelectPage && idx == 2);
    in.mouseUp();
    CHECK(in.mouseDown(ui::plate::kTabFaces[3].cx(), ui::plate::kTabFaces[3].cy(), false, false, &idx) == ui::UiAction::FaderBank && idx == 3);
    in.mouseUp();
    {   // LIM button in the bottom bar toggles the main limiter.
        const ui::Rect lb = L.limiterButton();
        params::Parameter* lim = tree.find("main.limit");
        const bool was = lim->isOn();
        in.mouseDown(lb.cx(), lb.cy(), false, false);
        in.mouseUp();
        CHECK(lim->isOn() != was && lb.x > L.overview().right() && lb.right() < L.masterMeter().x);
        lim->setPlain(was ? 1.0f : 0.0f);
    }

    // Knob pages: every stem knob rebinds to that page's parameter and caption, and back.
    {
        int ok = 0, total = 0;
        for (std::uint32_t pg = 0; pg < ui::kPages; ++pg) {
            L.setPage(static_cast<ui::Page>(pg));
            for (const auto& w : L.widgets()) {
                if (w.kind != ui::WidgetKind::Knob && w.kind != ui::WidgetKind::SendKnob) continue;
                const float kx = w.rect.cx() - L.strips()[w.index].bed.cx();
                const std::uint32_t k = w.kind == ui::WidgetKind::Knob ? (w.rect.cy() < L.strips()[w.index].bed.y + 300 ? 0u : 1u)
                                                                        : (kx < -1 ? 2u : kx > 1 ? 4u : 3u);
                ++total;
                if (tree.at(static_cast<std::size_t>(w.param)).spec().id == params::stemParamId(w.index, ui::kPageFields[pg][k]) &&
                    w.caption == ui::kPageCaptions[pg][k]) ++ok;
            }
        }
        L.setPage(ui::Page::Mix);
        CHECK(total == static_cast<int>(3 * kStemCount * 5) && ok == total);
    }

    // Fader bank: the 13 stem faders follow the selected send.
    L.setFaderBank(2);
    const int stem4Fader = L.widgets()[static_cast<std::size_t>(L.strips()[3].faderWidget)].param;
    CHECK(stem4Fader == static_cast<int>(tree.find(params::stemParamId(3, "aux2"))->index()));
    L.setFaderBank(0);
    CHECK(L.widgets()[static_cast<std::size_t>(L.strips()[3].faderWidget)].param == static_cast<int>(tree.find(params::stemParamId(3, "main"))->index()));

    // Knob: round hit area, drag of kKnobUnitsFullRange covers the range, double-click resets.
    const ui::Widget* trim = nullptr;
    for (const auto& w : L.widgets())
        if (w.kind == ui::WidgetKind::Knob && w.index == 4 && w.caption == "TRIM") trim = &w;
    CHECK(trim != nullptr);
    if (!trim) return;
    CHECK(L.hitTest(trim->rect.x + 2, trim->rect.y + 2) != L.hitTest(trim->rect.cx(), trim->rect.cy()));
    params::Parameter& tp = tree.at(static_cast<std::size_t>(trim->param));
    in.mouseDown(trim->rect.cx(), trim->rect.cy(), false, false);
    in.mouseMove(trim->rect.cx(), trim->rect.cy() - ui::Interaction::kKnobUnitsFullRange, false);
    CHECK(std::fabs(tp.normalized() - 1.0f) < 1e-4f);
    in.mouseUp();
    in.mouseDown(trim->rect.cx(), trim->rect.cy(), true, false);
    CHECK(tp.plain() == 0.0f);
    in.mouseUp();
    in.wheel(trim->rect.cx(), trim->rect.cy(), 1.0f, false);
    CHECK(std::fabs(tp.normalized() - 0.52f) < 1e-3f);

    // Fader: the cap follows the mouse (travel = slot - cap).
    const auto& st = L.strips()[6];
    const ui::Widget& fw = L.widgets()[static_cast<std::size_t>(st.faderWidget)];
    params::Parameter& fp = tree.at(static_cast<std::size_t>(fw.param));
    const float before = fp.normalized();
    in.mouseDown(fw.rect.cx(), fw.rect.cy(), false, false);
    in.mouseMove(fw.rect.cx(), fw.rect.cy() + (st.faderTrack.h - st.capH) * 0.25f, false);
    CHECK(std::fabs(fp.normalized() - (before - 0.25f)) < 1e-3f);
    in.mouseUp();

    // Buttons toggle.
    for (const auto& w : L.widgets())
        if (w.kind == ui::WidgetKind::Button && w.index == 2 && w.caption == "S") {
            in.mouseDown(w.rect.cx(), w.rect.cy(), false, false);
            in.mouseUp();
            CHECK(tree.at(static_cast<std::size_t>(w.param)).isOn());
        }

    // Meter scale lines up with the fader taper.
    CHECK(std::fabs(ui::MixerView::levelToNorm(1.0f) - 0.75f) < 1e-4f);
    CHECK(ui::MixerView::levelToNorm(0.0f) == 0.0f && std::fabs(ui::MixerView::levelToNorm(std::pow(10.0f, 6.0f / 20.0f)) - 1.0f) < 1e-4f);
}

void testUiFont() {
    std::printf("[ui font]\n");
    std::string err;
    const auto fonts = ui::FontSet::load(assetDir() / "fonts", &err);
    CHECK(fonts.has_value());
    if (!fonts) { std::printf("  %s\n", err.c_str()); return; }
    ui::FontAtlas big, lap;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(big.bake(*fonts, 1.0f));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(lap.bake(*fonts, 1389.0f / 3840.0f));
    std::printf("  Poppins atlas: %u px at 4K (bake %.0f ms), %u px at 1389 wide\n", big.atlasSize(), ms, lap.atlasSize());
    CHECK(big.atlasSize() <= 2048 && lap.atlasSize() <= 1024);
    const float w1 = big.measure(ui::TextStyle::Title, "STEM 13"), w2 = lap.measure(ui::TextStyle::Title, "STEM 13");
    CHECK(w1 > 50.0f && std::fabs(w1 - w2) / w1 < 0.08f);     // same design width at any scale
    const float cap = big.capHeight(ui::TextStyle::Label);
    CHECK(cap > 18.0f && cap < 24.0f);
    // Blur channel exists around the coverage channel.
    const auto& px = big.pixels();
    std::size_t covered = 0, haloOnly = 0;
    for (std::size_t i = 0; i < px.size(); i += 2) {
        if (px[i] > 0) ++covered;
        else if (px[i + 1] > 0) ++haloOnly;
    }
    CHECK(covered > 1000 && haloOnly > covered / 4);
    int quads = 0;
    float firstX0 = 0, padX0 = 0;
    big.layout(ui::TextStyle::Label, "M\xC3\x98", 100, 200, 0, [&](const ui::GlyphQuad& q) { if (!quads++) firstX0 = q.x0; });
    big.layout(ui::TextStyle::Label, "M", 100, 200, 4, [&](const ui::GlyphQuad& q) { padX0 = q.x0; });
    CHECK(quads == 2 && std::fabs((firstX0 - padX0) - 4.0f) < 0.01f);
    std::size_t i = 0;
    CHECK(ui::nextCodepoint("\xC3\x98", i) == 0xD8 && i == 2);
}

} // namespace hc::test

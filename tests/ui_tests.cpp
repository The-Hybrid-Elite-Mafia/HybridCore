// Phase 4 tests (CPU side of the GPU UI): image pipeline never exceeds the texture cap,
// DXT5 round-trip quality, VRAM budget, disk cache, procedural faceplates, layout,
// mouse interaction and font baking.
#include "TestHarness.hpp"

#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/Theme.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace hc::test {

namespace {
// Mean absolute error per channel (0..255) between two same-size images.
double meanError(const ui::Image& a, const ui::Image& b) {
    double e = 0;
    for (std::size_t i = 0; i < a.rgba.size(); ++i) e += std::abs(static_cast<int>(a.rgba[i]) - static_cast<int>(b.rgba[i]));
    return e / static_cast<double>(a.rgba.size());
}
ui::Image gradient(std::uint32_t w, std::uint32_t h) {
    ui::Image img;
    img.width = w; img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            std::uint8_t* p = &img.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 255 / std::max(1u, w - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / std::max(1u, h - 1));
            p[2] = static_cast<std::uint8_t>((x + y) & 255);
            p[3] = 255;
        }
    return img;
}
} // namespace

void testUiImage() {
    std::printf("[ui image pipeline]\n");
    CHECK(ui::chooseTier(1673, 940).width == 1676);
    CHECK(ui::chooseTier(2560, 1440).width == 3348);

    // An "8K master" is baked to the 2x tier: nothing above 4096 px reaches the GPU.
    const ui::Image master = gradient(7680, 4320);
    const auto t0 = std::chrono::steady_clock::now();
    const ui::CompressedTexture tex = ui::bakeForTier(master, ui::kTier2x);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(tex.width == 3348 && tex.height == 1880);
    CHECK(tex.width <= ui::kMaxTextureSide && tex.height <= ui::kMaxTextureSide);
    CHECK(tex.levels.size() == 12);   // 3348 -> 1
    const double mb = static_cast<double>(tex.bytes()) / (1024.0 * 1024.0);
    const double rawMb = 7680.0 * 4320.0 * 4.0 * 4.0 / 3.0 / (1024.0 * 1024.0);
    CHECK(mb < 9.0);
    std::printf("  8K master -> %ux%u DXT5 + %zu mips: %.2f MB on the GPU (raw 8K RGBA + mips would be %.1f MB); bake %.0f ms\n",
                tex.width, tex.height, tex.levels.size(), mb, rawMb, ms);

    // DXT5 round trip on a smooth image stays close.
    const ui::Image small = gradient(64, 48);
    const ui::CompressedTexture c = ui::compressDxt5({small});
    const ui::Image back = ui::decompressDxt5(c.levels[0].data(), 64, 48);
    const double err = meanError(small, back);
    CHECK(err < 6.0);
    std::printf("  DXT5 round-trip mean error: %.2f / 255\n", err);

    // Odd sizes pad to whole 4x4 blocks.
    const ui::CompressedTexture odd = ui::compressDxt5({gradient(1673, 3)});
    CHECK(odd.width == 1676 && odd.height == 4 && odd.levels[0].size() == (1676 / 4) * 1 * 16);

    // Cover resize keeps aspect and fills exactly.
    const ui::Image wide = ui::coverResize(gradient(400, 100), 100, 100);
    CHECK(wide.width == 100 && wide.height == 100);

    // Budget.
    ui::TextureBudget budget;
    CHECK(budget.reserve(tex.bytes()));
    CHECK(!budget.reserve(ui::kVramBudgetBytes));
    budget.release(tex.bytes());
    CHECK(budget.used() == 0);

    // Disk cache round trip, corrupted file rejected.
    const auto dir = std::filesystem::temp_directory_path() / "hc_texcache_test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    CHECK(ui::saveTextureCache(dir / "a.hctx", tex));
    const auto loaded = ui::loadTextureCache(dir / "a.hctx");
    CHECK(loaded && loaded->width == tex.width && loaded->levels.size() == tex.levels.size() && loaded->levels[3] == tex.levels[3]);
    { std::FILE* f = std::fopen((dir / "b.hctx").string().c_str(), "wb"); if (f) { std::fputs("HCTXjunk", f); std::fclose(f); } }
    CHECK(!ui::loadTextureCache(dir / "b.hctx"));
    std::filesystem::remove_all(dir, ec);

    // PNG write + decode path.
    const auto png = std::filesystem::temp_directory_path() / "hc_png_test.png";
    CHECK(ui::writePng(png, small));
    const auto rd = ui::loadImageFile(png);
    CHECK(rd && rd->width == 64 && rd->height == 48 && meanError(*rd, small) == 0.0);
    std::filesystem::remove(png, ec);
    CHECK(!ui::loadImageFile(std::filesystem::temp_directory_path() / "does_not_exist.png"));
}

void testUiLayout() {
    std::printf("[ui layout + interaction]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    const ui::Layout layout(tree);

    // Every parameter has exactly one control.
    std::set<int> bound;
    std::size_t meters = 0, headers = 0;
    for (const auto& w : layout.widgets()) {
        if (w.param >= 0) CHECK(bound.insert(w.param).second);
        if (w.kind == ui::WidgetKind::StemMeter || w.kind == ui::WidgetKind::BusMeterL || w.kind == ui::WidgetKind::BusMeterR) ++meters;
        if (w.kind == ui::WidgetKind::HeaderButton) ++headers;
        CHECK(w.rect.x >= 0 && w.rect.y >= 0 && w.rect.x + w.rect.w <= ui::kDesignWidth && w.rect.y + w.rect.h <= ui::kDesignHeight);
    }
    CHECK(bound.size() == tree.size());
    CHECK(meters == hc::kStemCount + 2 * hc::kBusCount && headers == 3);
    CHECK(layout.strips().size() == hc::kStemCount + hc::kBusCount);
    std::printf("  %zu widgets, %zu parameters bound, %zu meters\n", layout.widgets().size(), bound.size(), meters);

    // No two interactive widgets overlap.
    bool overlap = false;
    const auto& ws = layout.widgets();
    for (std::size_t i = 0; i < ws.size(); ++i)
        for (std::size_t j = i + 1; j < ws.size(); ++j) {
            if (ws[i].param < 0 || ws[j].param < 0) continue;
            const auto& a = ws[i].rect; const auto& b = ws[j].rect;
            if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h) overlap = true;
        }
    CHECK(!overlap);

    ui::Interaction in(layout, tree);
    auto find = [&](const std::string& id) -> const ui::Widget* {
        const auto* p = tree.find(id);
        for (const auto& w : layout.widgets()) if (p && w.param == static_cast<int>(p->index())) return &w;
        return nullptr;
    };
    // Button toggles.
    const ui::Widget* mute = find("stem03.mute");
    CHECK(mute != nullptr);
    if (mute) {
        in.mouseDown(mute->rect.cx(), mute->rect.cy(), false, false); in.mouseUp();
        CHECK(tree.find("stem03.mute")->isOn());
        in.mouseDown(mute->rect.cx(), mute->rect.cy(), false, false); in.mouseUp();
        CHECK(!tree.find("stem03.mute")->isOn());
    }
    // Knob drag: up 125 px = +0.5 normalized; fine mode is 10x slower; double-click resets.
    const ui::Widget* pan = find("stem01.pan");
    CHECK(pan != nullptr);
    if (pan) {
        in.mouseDown(pan->rect.cx(), pan->rect.cy(), false, false);
        in.mouseMove(pan->rect.cx(), pan->rect.cy() - 125.0f, false);
        in.mouseUp();
        CHECK(std::fabs(tree.find("stem01.pan")->plain() - 1.0f) < 1e-4f);
        in.mouseDown(pan->rect.cx(), pan->rect.cy(), false, false);
        in.mouseMove(pan->rect.cx(), pan->rect.cy() + 125.0f, true);
        in.mouseUp();
        CHECK(std::fabs(tree.find("stem01.pan")->plain() - 0.9f) < 1e-3f);
        in.mouseDown(pan->rect.cx(), pan->rect.cy(), true, false);
        in.mouseUp();
        CHECK(tree.find("stem01.pan")->plain() == 0.0f);
    }
    // Fader follows the mouse: dragging the full slot length goes bottom -> top.
    const ui::Widget* fader = find("stem02.main");
    CHECK(fader != nullptr);
    if (fader) {
        tree.find("stem02.main")->setNormalized(0.0f);
        in.mouseDown(fader->rect.cx(), fader->rect.cy(), false, false);
        in.mouseMove(fader->rect.cx(), fader->rect.cy() - 492.0f, false);
        in.mouseUp();
        CHECK(std::fabs(tree.find("stem02.main")->normalized() - 1.0f) < 1e-4f);
    }
    // Header buttons return actions; wheel nudges.
    for (const auto& w : layout.widgets())
        if (w.kind == ui::WidgetKind::HeaderButton)
            CHECK(in.mouseDown(w.rect.cx(), w.rect.cy(), false, false) == w.action);
    if (pan) {
        in.wheel(pan->rect.cx(), pan->rect.cy(), 5.0f, false);
        CHECK(std::fabs(tree.find("stem01.pan")->plain() - 0.2f) < 1e-3f);
    }

    const ui::Viewport v = ui::Viewport::fit(1673.0f * 2.0f, 940.0f * 2.0f + 100.0f);
    CHECK(std::fabs(v.scale - 2.0f) < 1e-5f && std::fabs(v.offsetY - 50.0f) < 1e-4f);
    CHECK(std::fabs(v.toDesignX(2.0f * 100.0f) - 100.0f) < 1e-4f);
}

void testUiFaceplate() {
    std::printf("[ui faceplates]\n");
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    const ui::Layout layout(tree);
    for (std::uint32_t t = 0; t < ui::kThemeCount; ++t) {
        const auto id = static_cast<ui::ThemeId>(t);
        const auto t0 = std::chrono::steady_clock::now();
        const ui::Image img = ui::generateFaceplate(id, layout, ui::kTier1x.width, ui::kTier1x.height);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(img.width == ui::kTier1x.width && img.height == ui::kTier1x.height);
        // Deterministic: same inputs, same pixels.
        const ui::Image again = ui::generateFaceplate(id, layout, 64, 36);
        const ui::Image again2 = ui::generateFaceplate(id, layout, 64, 36);
        CHECK(again.rgba == again2.rgba);
        // Not flat: panels and gradient produce real variation.
        double mean = 0, var = 0;
        const std::size_t n = img.rgba.size() / 4;
        for (std::size_t i = 0; i < n; ++i) mean += img.rgba[i * 4];
        mean /= static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) { const double d = img.rgba[i * 4] - mean; var += d * d; }
        const double sd = std::sqrt(var / static_cast<double>(n));
        CHECK(sd > 2.0);
        std::printf("  %-12s 1676x940 generated in %4.0f ms, mean %5.1f, std-dev %4.1f\n", ui::palette(id).name, ms, mean, sd);
    }
    CHECK(ui::themeFromName("obsidian") == ui::ThemeId::Obsidian && ui::themeFromName("nope") == ui::ThemeId::Pearl);
    CHECK(ui::nextTheme(ui::ThemeId::Titanium) == ui::ThemeId::Pearl);
}

void testUiFont() {
    std::printf("[ui font atlas]\n");
    std::size_t i = 0;
    const std::string s = "A\xC3\x98" "B";
    CHECK(ui::nextCodepoint(s, i) == 'A' && ui::nextCodepoint(s, i) == 0xD8 && ui::nextCodepoint(s, i) == 'B');

    const auto font = ui::FontAtlas::loadSystemFont();
    if (!font) {
        std::printf("  no system font found on this machine: font baking not tested here\n");
        return;
    }
    ui::FontAtlas atlas;
    CHECK(atlas.bake(*font, 1.0f));
    CHECK(atlas.bake(*font, 2.0f));   // high-DPI bake (may use the 2048² atlas)
    const float wide = atlas.measure(ui::TextSize::Large, "THE HYBRID CORE");
    const float nar  = atlas.measure(ui::TextSize::Small, "THE HYBRID CORE");
    CHECK(wide > nar && nar > 50.0f);
    int quads = 0;
    atlas.layout(ui::TextSize::Medium, "Stem \xC3\x98", 10, 20, [&](const ui::GlyphQuad& q) {
        ++quads;
        CHECK(q.x1 > q.x0 && q.y1 > q.y0 && q.s1 > q.s0 && q.t1 > q.t0);
    });
    CHECK(quads == 5);   // space has no quad
    std::size_t lit = 0;
    for (auto p : atlas.pixels()) lit += p > 0;
    CHECK(lit > 10000);
    std::printf("  baked 3 sizes at 2x scale into %u^2 atlas, %zu lit pixels\n", atlas.atlasSize(), lit);
}

} // namespace hc::test

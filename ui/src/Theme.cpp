#include "hybrid/ui/Theme.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hc::ui {

namespace {
constexpr Rgb c(int r, int g, int b) { return {r / 255.0f, g / 255.0f, b / 255.0f}; }

const std::array<Palette, kThemeCount> kPalettes{{
    {"PEARL WHITE", "pearl",
     c(240, 238, 234), c(212, 210, 207), c(222, 220, 216), c(128, 126, 124),
     c(248, 247, 245), c(170, 168, 165), c(24, 24, 30),
     c(44, 44, 52), c(118, 118, 128),
     c(0, 214, 255), c(255, 60, 170),
     c(232, 230, 226),
     c(0, 214, 255), c(255, 196, 0), c(255, 48, 64),
     0.10f, 0.20f, 1.0f},
    {"OBSIDIAN", "obsidian",
     c(34, 33, 40), c(9, 9, 13), c(18, 18, 23), c(0, 0, 0),
     c(44, 44, 52), c(96, 96, 112), c(255, 255, 255),
     c(226, 226, 236), c(132, 132, 150),
     c(255, 36, 140), c(0, 200, 255),
     c(30, 30, 37),
     c(255, 36, 140), c(255, 190, 0), c(255, 40, 40),
     0.06f, 0.55f, 0.0f},
    {"TITANIUM", "titanium",
     c(162, 166, 170), c(116, 120, 124), c(140, 144, 148), c(44, 46, 50),
     c(188, 192, 196), c(92, 96, 100), c(18, 18, 18),
     c(20, 22, 26), c(58, 62, 68),
     c(255, 168, 0), c(0, 255, 170),
     c(150, 154, 158),
     c(255, 168, 0), c(255, 220, 0), c(255, 48, 48),
     0.55f, 0.15f, 0.0f},
}};

std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float rnd(std::uint32_t x) { return static_cast<float>(hash(x) & 0xFFFFFF) / 16777215.0f; }

float srgbToLinear(float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// Linear-light float canvas with simple anti-aliased primitives.
struct Canvas {
    std::uint32_t      w, h;
    float              sx, sy;   // design -> pixel scale (tier width is padded to a multiple of 4)
    std::vector<float> px;       // RGB linear

    Canvas(std::uint32_t W, std::uint32_t H)
        : w(W), h(H), sx(static_cast<float>(W) / kDesignWidth), sy(static_cast<float>(H) / kDesignHeight),
          px(static_cast<std::size_t>(W) * H * 3) {}

    void blend(std::uint32_t x, std::uint32_t y, Rgb lin, float a) {
        if (a <= 0.0f || x >= w || y >= h) return;
        float* p = &px[(static_cast<std::size_t>(y) * w + x) * 3];
        p[0] += (lin.r - p[0]) * a;
        p[1] += (lin.g - p[1]) * a;
        p[2] += (lin.b - p[2]) * a;
    }
    // Rounded rect in design units with 1px AA.
    void roundRect(Rect r, float radius, Rgb lin, float alpha) {
        const float x0 = r.x * sx, y0 = r.y * sy, x1 = (r.x + r.w) * sx, y1 = (r.y + r.h) * sy, rad = radius * sy;
        const auto ix0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(x0)));
        const auto iy0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(y0)));
        const auto ix1 = std::min(w, static_cast<std::uint32_t>(std::ceil(x1)));
        const auto iy1 = std::min(h, static_cast<std::uint32_t>(std::ceil(y1)));
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f, mx = (x0 + x1) * 0.5f, my = (y0 + y1) * 0.5f;
        for (std::uint32_t y = iy0; y < iy1; ++y)
            for (std::uint32_t x = ix0; x < ix1; ++x) {
                const float qx = std::fabs(x + 0.5f - mx) - (hx - rad);
                const float qy = std::fabs(y + 0.5f - my) - (hy - rad);
                const float d  = std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - rad;
                blend(x, y, lin, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
    }
    void circle(float cxd, float cyd, float rd, Rgb lin, float alpha) {
        const float cx = cxd * sx, cy = cyd * sy, r = rd * sy;
        const auto ix0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(cx - r - 1)));
        const auto iy0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(cy - r - 1)));
        const auto ix1 = std::min(w, static_cast<std::uint32_t>(std::ceil(cx + r + 1)));
        const auto iy1 = std::min(h, static_cast<std::uint32_t>(std::ceil(cy + r + 1)));
        for (std::uint32_t y = iy0; y < iy1; ++y)
            for (std::uint32_t x = ix0; x < ix1; ++x) {
                const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r;
                blend(x, y, lin, alpha * std::clamp(0.5f - d, 0.0f, 1.0f));
            }
    }
    // Recessed panel: shadow on top/left, highlight on bottom/right, then fill.
    void recess(Rect r, float radius, Rgb fill, Rgb shadow, Rgb light) {
        roundRect({r.x - 1, r.y - 1, r.w + 2, r.h + 2}, radius + 1, light, 0.55f);
        roundRect({r.x - 1, r.y - 1, r.w + 1, r.h + 1}, radius + 1, shadow, 0.65f);
        roundRect(r, radius, fill, 1.0f);
    }
    void screw(float cxd, float cyd, float rd, Rgb body, Rgb dark) {
        circle(cxd + 0.6f, cyd + 0.8f, rd, dark, 0.5f);
        circle(cxd, cyd, rd, body, 1.0f);
        roundRect({cxd - rd * 0.75f, cyd - rd * 0.14f, rd * 1.5f, rd * 0.28f}, rd * 0.1f, dark, 0.85f);
    }
};

Rgb lin(Rgb s) { return {srgbToLinear(s.r), srgbToLinear(s.g), srgbToLinear(s.b)}; }
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
Rgb scale(Rgb a, float k) { return {a.r * k, a.g * k, a.b * k}; }
} // namespace

const Palette& palette(ThemeId id) noexcept { return kPalettes[static_cast<std::size_t>(id) % kThemeCount]; }

ThemeId nextTheme(ThemeId id) noexcept {
    return static_cast<ThemeId>((static_cast<std::uint32_t>(id) + 1) % kThemeCount);
}

ThemeId themeFromName(const std::string& name) noexcept {
    for (std::uint32_t i = 0; i < kThemeCount; ++i)
        if (name == kPalettes[i].fileStem) return static_cast<ThemeId>(i);
    return ThemeId::Pearl;
}

Image generateFaceplate(ThemeId theme, const Layout& layout, std::uint32_t width, std::uint32_t height) {
    const Palette& p = palette(theme);
    Canvas cv(width, height);
    const Rgb top = lin(p.plateTop), bot = lin(p.plateBottom);

    // 1) Base plate: vertical gradient + brushed streaks + gloss band + pearl sheen.
    for (std::uint32_t y = 0; y < height; ++y) {
        const float v       = static_cast<float>(y) / static_cast<float>(height);
        const float rowNoise = (rnd(y * 2654435761u) - 0.5f) * 2.0f;
        for (std::uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width);
            Rgb col = mix(top, bot, v);
            // brushed metal: long horizontal streaks with slow variation along x
            const float streak = rowNoise * 0.6f + (rnd(y * 7919u + x / std::max(1u, width / 64)) - 0.5f) * 0.4f;
            col = scale(col, 1.0f + p.brushed * 0.18f * streak);
            // gloss: soft diagonal band
            const float d = (u * 0.8f + v * 0.6f) - 0.55f;
            col = mix(col, {1, 1, 1}, p.gloss * 0.12f * std::exp(-d * d * 18.0f));
            // pearl: subtle iridescence
            if (p.sheen > 0.0f) {
                const float ph = u * 6.0f + v * 3.5f;
                col.r *= 1.0f + 0.020f * p.sheen * std::sin(ph);
                col.g *= 1.0f + 0.016f * p.sheen * std::sin(ph + 2.1f);
                col.b *= 1.0f + 0.026f * p.sheen * std::sin(ph + 4.2f);
            }
            // fine grain
            col = scale(col, 1.0f + (rnd(x * 73856093u ^ y * 19349663u) - 0.5f) * 0.02f);
            float* d0 = &cv.px[(static_cast<std::size_t>(y) * width + x) * 3];
            d0[0] = col.r; d0[1] = col.g; d0[2] = col.b;
        }
    }

    const Rgb shadow = lin(scale(p.plateBottom, 0.45f));
    const Rgb light  = lin(mix(p.plateTop, {1, 1, 1}, 0.6f));
    const Rgb recess = lin(p.recess);
    const Rgb groove = lin(p.groove);

    // 2) Header bar edge.
    const Rect hb = layout.headerBar();
    cv.roundRect({hb.x, hb.h - 2, hb.w, 1}, 0, shadow, 0.6f);
    cv.roundRect({hb.x, hb.h - 1, hb.w, 1}, 0, light, 0.5f);

    // 3) Strip panels, fader slots, scale ticks, knob wells.
    for (const auto& st : layout.strips()) {
        cv.recess(st.panel, 7.0f, recess, shadow, light);
        const Rect t = st.faderTrack;
        cv.recess({t.x, t.y, t.w, t.h}, t.w * 0.5f, groove, shadow, light);
        // Ticks at fader dB marks (taper: unity at 0.75 of travel, -60 at bottom).
        const float marks[] = {6, 0, -6, -12, -24, -40, -60};
        for (float db : marks) {
            const float tt = (db + 60.0f) / 66.0f;
            const float n  = std::pow(tt, 1.0f / (std::log((0.0f + 60.0f) / 66.0f) / std::log(0.75f)));
            const float y  = t.y + t.h * (1.0f - n);
            cv.roundRect({t.x - 12, y - 0.6f, 9, 1.2f}, 0.4f, groove, 0.7f);
        }
    }
    for (const auto& w : layout.widgets()) {
        if (w.kind == WidgetKind::Knob || w.kind == WidgetKind::SmallKnob)
            cv.circle(w.rect.cx(), w.rect.cy() + 1.5f, w.rect.w * 0.5f + 3.0f, shadow, 0.35f);
        if (w.kind == WidgetKind::StemMeter || w.kind == WidgetKind::BusMeterL || w.kind == WidgetKind::BusMeterR)
            cv.recess(w.rect, 2.0f, groove, shadow, light);
        if (w.kind == WidgetKind::HeaderButton) cv.recess(w.rect, 6.0f, recess, shadow, light);
    }

    // 4) Corner screws.
    const Rgb screwBody = lin(mix(p.knobBody, p.plateBottom, 0.3f));
    const Rgb screwDark = lin(scale(p.groove, 0.8f));
    const float sx[] = {12, kDesignWidth - 12}, sy[] = {12, kDesignHeight - 12};
    for (float x : sx) for (float y : sy) cv.screw(x, y, 5.0f, screwBody, screwDark);

    // 5) Convert to sRGB8 with tiny dither.
    Image img;
    img.width  = width;
    img.height = height;
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0, n = static_cast<std::size_t>(width) * height; i < n; ++i) {
        const float dither = (rnd(static_cast<std::uint32_t>(i) * 2246822519u) - 0.5f) / 255.0f;
        for (int k = 0; k < 3; ++k)
            img.rgba[i * 4 + k] = static_cast<std::uint8_t>(std::clamp(linearToSrgb(cv.px[i * 3 + k]) + dither, 0.0f, 1.0f) * 255.0f + 0.5f);
        img.rgba[i * 4 + 3] = 255;
    }
    return img;
}

} // namespace hc::ui

namespace hc::ui {

CompressedTexture loadFaceplate(ThemeId theme, Tier tier, const Layout& layout,
                                const std::filesystem::path& dataDir, std::string* info) {
    const Palette& p = palette(theme);
    const std::filesystem::path cacheDir = dataDir / "texcache";
    std::error_code ec;

    for (const char* ext : {".png", ".jpg", ".jpeg"}) {
        const std::filesystem::path skin = dataDir / "skins" / (std::string(p.fileStem) + ext);
        if (!std::filesystem::exists(skin, ec)) continue;
        const std::filesystem::path cached = cacheDir / ("skin-" + textureCacheKey(skin, tier) + ".hctx");
        if (auto hit = loadTextureCache(cached)) {
            if (info) *info = "skin " + skin.filename().string() + " (cached)";
            return std::move(*hit);
        }
        std::string err;
        if (auto img = loadImageFile(skin, &err)) {
            CompressedTexture tex = bakeForTier(*img, tier);
            saveTextureCache(cached, tex);
            if (info) *info = "skin " + skin.filename().string() + " " + std::to_string(img->width) + "x" +
                              std::to_string(img->height) + " baked to " + std::to_string(tex.width) + "x" + std::to_string(tex.height);
            return tex;
        }
        if (info) *info = "skin " + skin.filename().string() + " unreadable (" + err + "), using built-in plate";
        break;
    }

    const std::filesystem::path cached = cacheDir / ("proc-" + std::string(p.fileStem) + "-" + std::to_string(tier.width) + "x" +
                                                     std::to_string(tier.height) + "-v1.hctx");
    if (auto hit = loadTextureCache(cached)) {
        if (info && info->empty()) *info = std::string("built-in ") + p.name + " plate (cached)";
        return std::move(*hit);
    }
    CompressedTexture tex = compressDxt5(buildMipChain(generateFaceplate(theme, layout, tier.width, tier.height)));
    saveTextureCache(cached, tex);
    if (info && info->empty()) *info = std::string("built-in ") + p.name + " plate";
    return tex;
}

} // namespace hc::ui

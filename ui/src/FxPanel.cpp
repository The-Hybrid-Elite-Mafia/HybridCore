#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/Parts.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace hc::ui {

// =============================================================================================
//  Layout
// =============================================================================================
namespace {

constexpr float kMargin = 44.0f, kHeaderH = 190.0f, kFooterH = 112.0f, kBayPad = 22.0f, kGap = 26.0f;
constexpr float kBayDepth = 5.0f, kWellDepth = 14.0f, kWindowDepth = 7.0f, kButtonDepth = 8.0f, kMeterDepth = 10.0f;
constexpr float kSectionCaptionH = 46.0f;

int balancedCols(int cells, int maxCols) {
    if (cells <= 0) return 1;
    const int rows = (cells + maxCols - 1) / maxCols;
    return (cells + rows - 1) / rows;
}

float meterColW(const PanelMeter& m) {
    const float textW = 14.5f * static_cast<float>(m.name.size()) + 18.0f;
    return std::max(82.0f, textW);
}

void addRectPocket(PanelLayout& L, Rect r, float radius, float depth, bool rings = false) {
    L.pockets.push_back({r, radius, depth, false, rings});
}

// One control cell at (x, y).
void layoutCell(PanelLayout& L, const PanelSpec& spec, int param, float x, float y, float baseDepth) {
    const PanelParam& p = spec.params[static_cast<std::size_t>(param)];
    const float cx = x + kPanelCellW * 0.5f;
    const Rect caption{x + 6, y + 6, kPanelCellW - 12, 40};
    L.captions.push_back({param, caption});
    switch (p.kind) {
    case PanelParam::Kind::Knob: {
        PanelKnob k;
        k.param = param;
        k.well = {cx - 84, y + 46, 168, 168};
        k.caption = caption;
        k.value = {cx - 88, y + 220, 176, 42};
        L.pockets.push_back({k.well, 84, baseDepth + kWellDepth, true, true});
        addRectPocket(L, k.value, 10, baseDepth + kWindowDepth);
        L.knobs.push_back(k);
        break;
    }
    case PanelParam::Kind::Toggle: {
        PanelButton b{param, -1, {cx - 82, y + 96, 164, 76}, p.name};
        addRectPocket(L, b.rect.inset(-6), 16, baseDepth + kButtonDepth);
        L.buttons.push_back(b);
        break;
    }
    case PanelParam::Kind::Choice: {
        const int n = static_cast<int>(p.choices.size());
        if (n <= 4) {
            const float top = y + 50, avail = 212.0f, gap = 8.0f;
            const float h = std::min(52.0f, (avail - gap * static_cast<float>(n - 1)) / static_cast<float>(std::max(1, n)));
            for (int c = 0; c < n; ++c) {
                PanelButton b{param, c, {cx - 96, top + static_cast<float>(c) * (h + gap), 192, h}, p.choices[static_cast<std::size_t>(c)]};
                addRectPocket(L, b.rect.inset(-5), 12, baseDepth + kButtonDepth);
                L.buttons.push_back(b);
            }
        } else {
            PanelSelector s;
            s.param = param;
            s.caption = caption;
            s.box  = {cx - 98, y + 92, 196, 66};
            s.prev = {cx - 98, y + 176, 92, 52};
            s.next = {cx + 6, y + 176, 92, 52};
            addRectPocket(L, s.box, 12, baseDepth + kWindowDepth);
            addRectPocket(L, s.prev.inset(-5), 12, baseDepth + kButtonDepth);
            addRectPocket(L, s.next.inset(-5), 12, baseDepth + kButtonDepth);
            L.selectors.push_back(s);
        }
        break;
    }
    }
}

} // namespace

PanelLayout layoutFxPanel(const PanelSpec& spec) {
    PanelLayout L;
    L.name = spec.name;
    L.blurb = spec.blurb;
    std::vector<PanelGroup> groups = spec.groups;
    if (groups.empty()) groups.push_back({"", 0, static_cast<int>(spec.params.size())});

    // ---- sections: flow left to right, wrap at a comfortable width -----------------------
    const bool single = groups.size() == 1;
    const int  maxCols = single ? kPanelMaxCols : 4;
    struct Placed { Rect bay; int g; int cols; };
    std::vector<Placed> placed;
    const float rowMaxW = single ? 1e9f : 2000.0f;
    float x = kMargin, y = kMargin + kHeaderH, rowH = 0, widest = 0;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const int cells = groups[g].count;
        const int cols = balancedCols(cells, maxCols), rows = (cells + cols - 1) / std::max(1, cols);
        const float capH = groups[g].name.empty() ? 0.0f : kSectionCaptionH;
        const float w = static_cast<float>(cols) * kPanelCellW + 2 * kBayPad;
        const float h = static_cast<float>(rows) * kPanelCellH + 2 * kBayPad + capH;
        if (x > kMargin && x + w > kMargin + rowMaxW) { x = kMargin; y += rowH + kGap; rowH = 0; }
        placed.push_back({{x, y, w, h}, static_cast<int>(g), cols});
        x += w + kGap;
        rowH = std::max(rowH, h);
        widest = std::max(widest, x - kGap);
    }
    float bodyBottom = y + rowH;

    // ---- meter bay ------------------------------------------------------------------------
    float metersW = 0;
    for (const PanelMeter& m : spec.meters) metersW += meterColW(m);
    float right = widest;
    if (!spec.meters.empty()) {
        const float bayW = metersW + 2 * kBayPad;
        const float bayH = std::max(bodyBottom - (kMargin + kHeaderH), kPanelCellH + 2 * kBayPad);
        L.meterBay = {widest + kGap, kMargin + kHeaderH, bayW, bayH};
        right = L.meterBay.right();
        bodyBottom = std::max(bodyBottom, L.meterBay.bottom());
    }
    L.width  = std::max(right + kMargin, 1040.0f);
    L.height = bodyBottom + kGap + kFooterH + kMargin * 0.5f;
    // Centre the body if the title made the panel wider than the controls.
    const float shift = (L.width - kMargin - right) * 0.5f;
    if (shift > 1.0f) {
        for (auto& p : placed) p.bay = p.bay.offset(shift, 0);
        if (!spec.meters.empty()) L.meterBay = L.meterBay.offset(shift, 0);
    }

    // ---- header, footer -----------------------------------------------------------------
    L.header    = {kMargin, kMargin, L.width - 2 * kMargin, kHeaderH - 30};
    L.logo      = {kMargin + 18, kMargin + 14, 132, 132};
    L.titleArea = {L.logo.right() + 26, kMargin + 10, L.width - 2 * kMargin - 190, kHeaderH - 50};
    L.footer    = {kMargin, L.height - kMargin * 0.5f - kFooterH, L.width - 2 * kMargin, kFooterH - 16};
    L.bypass    = {L.footer.right() - 230, L.footer.y + 18, 210, L.footer.h - 36};
    addRectPocket(L, L.header, 22, kBayDepth);
    L.pockets.push_back({L.logo, 66, kBayDepth + 10, true, true});
    addRectPocket(L, L.bypass.inset(-6), 16, kButtonDepth);
    // neon channels: under the header, above the footer, both full width
    L.grooves.push_back({kMargin + 30, L.header.bottom() + 15, L.width - kMargin - 30, L.header.bottom() + 15, false});
    L.grooves.push_back({kMargin + 30, L.footer.y - 13, L.width - kMargin - 30, L.footer.y - 13, false});

    // ---- sections and their cells ---------------------------------------------------------
    for (const Placed& pl : placed) {
        const PanelGroup& g = groups[static_cast<std::size_t>(pl.g)];
        PanelSection s;
        s.bay = pl.bay;
        s.caption = g.name;
        const float capH = g.name.empty() ? 0.0f : kSectionCaptionH;
        s.captionArea = {pl.bay.x + kBayPad, pl.bay.y + 10, pl.bay.w - 2 * kBayPad, capH};
        addRectPocket(L, pl.bay, 26, kBayDepth);
        if (capH > 0) L.grooves.push_back({pl.bay.x + 24, pl.bay.y + capH + 6, pl.bay.right() - 24, pl.bay.y + capH + 6, true});
        L.sections.push_back(s);
        for (int i = 0; i < g.count; ++i) {
            const int col = i % pl.cols, row = i / pl.cols;
            layoutCell(L, spec, g.first + i, pl.bay.x + kBayPad + static_cast<float>(col) * kPanelCellW,
                       pl.bay.y + kBayPad + capH + static_cast<float>(row) * kPanelCellH, kBayDepth);
        }
    }

    // ---- meters -----------------------------------------------------------------------------
    if (!spec.meters.empty()) {
        addRectPocket(L, L.meterBay, 26, kBayDepth);
        float mx = L.meterBay.x + kBayPad;
        const float top = L.meterBay.y + kBayPad + 46, bottom = L.meterBay.bottom() - kBayPad - 58;
        for (std::size_t m = 0; m < spec.meters.size(); ++m) {
            const float w = meterColW(spec.meters[m]);
            PanelMeterSlot s;
            s.meter = static_cast<int>(m);
            s.value = {mx + 4, L.meterBay.y + kBayPad + 2, w - 8, 38};
            s.bar = {mx + w * 0.5f - 17, top, 34, bottom - top};
            s.caption = {mx, bottom + 8, w, 40};
            addRectPocket(L, s.bar.inset(-7), 12, kBayDepth + kMeterDepth);
            L.meters.push_back(s);
            mx += w;
        }
    }

    // ---- screws -----------------------------------------------------------------------------
    const float si = 24.0f;
    L.screws = {{si, si}, {L.width - si, si}, {si, L.height - si}, {L.width - si, L.height - si}};
    return L;
}

// =============================================================================================
//  The obsidian slab
// =============================================================================================
namespace {

struct V3 { float x, y, z; };
inline V3 norm3(V3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return {v.x / l, v.y / l, v.z / l}; }
inline float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float smooth01(float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }

inline std::uint32_t hash2(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8DA6B343u ^ static_cast<std::uint32_t>(y) * 0xD8163841u;
    h ^= h >> 13; h *= 0x5BD1E995u; h ^= h >> 15;
    return h;
}
inline float vnoise(float x, float y) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
    const float u = fx * fx * (3 - 2 * fx), v = fy * fy * (3 - 2 * fy);
    auto h = [](int a, int b) { return static_cast<float>(hash2(a, b) & 0xFFFF) / 65535.0f; };
    const float a = h(xi, yi), b = h(xi + 1, yi), c = h(xi, yi + 1), d = h(xi + 1, yi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
inline float fbm(float x, float y, int oct) {
    float s = 0, a = 0.5f, f = 1;
    for (int i = 0; i < oct; ++i) { s += a * vnoise(x * f, y * f); f *= 2.03f; a *= 0.5f; }
    return s;
}

// Signed distance to a rounded rectangle (negative inside).
inline float sdRound(float px, float py, const Rect& r, float radius) {
    const float hx = r.w * 0.5f, hy = r.h * 0.5f;
    const float qx = std::fabs(px - r.cx()) - hx + radius, qy = std::fabs(py - r.cy()) - hy + radius;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}
inline float pocketInside(const PanelPocket& p, float x, float y) {   // distance inside (> 0 inside)
    if (p.circle) {
        const float dx = x - p.r.cx(), dy = y - p.r.cy();
        return p.r.w * 0.5f - std::sqrt(dx * dx + dy * dy);
    }
    return -sdRound(x, y, p.r, p.radius);
}

constexpr float kChamfer = 4.5f;   // 45° chamfer width at the top of every pocket wall

struct Surface {
    float h = 0;          // height (design units, 0 = slab top)
    int   pocket = -1;    // pocket that defines the surface here (-1 = slab top)
    float inside = 0;     // distance inside that pocket
    std::uint8_t kind = 0;   // 0 slab, 1 chamfer, 2 wall, 3 floor, 4 groove, 5 bevel, 6 screw
};

} // namespace

Image renderFxPlate(const PanelLayout& L, const FxPalette& pal, float ppu) {
    const auto W = static_cast<std::uint32_t>(std::lround(L.width * ppu)), H = static_cast<std::uint32_t>(std::lround(L.height * ppu));
    Image img;
    img.width = W; img.height = H;
    img.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
    if (W == 0 || H == 0) return img;

    // Pockets per 64-unit tile, so each pixel looks at a handful, not all of them.
    constexpr float kTile = 64.0f;
    const int tw = static_cast<int>(std::ceil(L.width / kTile)) + 1, th = static_cast<int>(std::ceil(L.height / kTile)) + 1;
    std::vector<std::vector<int>> tiles(static_cast<std::size_t>(tw * th));
    for (std::size_t i = 0; i < L.pockets.size(); ++i) {
        const Rect r = L.pockets[i].r.inset(-3);
        for (int ty = std::max(0, static_cast<int>(r.y / kTile)); ty <= std::min(th - 1, static_cast<int>(r.bottom() / kTile)); ++ty)
            for (int tx = std::max(0, static_cast<int>(r.x / kTile)); tx <= std::min(tw - 1, static_cast<int>(r.right() / kTile)); ++tx)
                tiles[static_cast<std::size_t>(ty * tw + tx)].push_back(static_cast<int>(i));
    }
    const float outerR = 38.0f, bevel = 16.0f;
    const Rect slab{0, 0, L.width, L.height};

    auto surfaceAt = [&](float x, float y) {
        Surface s;
        // slab edge: rounded bevel down to the frame
        const float inEdge = -sdRound(x, y, slab.inset(2), outerR);
        if (inEdge < bevel) {
            const float t = 1.0f - std::max(inEdge, 0.0f) / bevel;
            s.h = -bevel * t * t;
            s.kind = 5;
        }
        const int tx = std::clamp(static_cast<int>(x / kTile), 0, tw - 1), ty = std::clamp(static_cast<int>(y / kTile), 0, th - 1);
        for (int i : tiles[static_cast<std::size_t>(ty * tw + tx)]) {
            const PanelPocket& p = L.pockets[static_cast<std::size_t>(i)];
            const float d = pocketInside(p, x, y);
            if (d <= 0) continue;
            float h; std::uint8_t k;
            if (d < kChamfer) { h = -d; k = 1; }
            else { h = -kChamfer - (d - kChamfer) * 6.0f; k = 2; }
            if (h <= -p.depth) { h = -p.depth; k = 3; }
            if (h < s.h) { s.h = h; s.pocket = i; s.inside = d; s.kind = k; }
        }
        for (const PanelGroove& g : L.grooves) {
            const float vx = g.x1 - g.x0, vy = g.y1 - g.y0, len2 = vx * vx + vy * vy;
            const float t = std::clamp(((x - g.x0) * vx + (y - g.y0) * vy) / std::max(len2, 1e-6f), 0.0f, 1.0f);
            const float dx = x - (g.x0 + t * vx), dy = y - (g.y0 + t * vy);
            const float d = std::sqrt(dx * dx + dy * dy);
            constexpr float hw = 6.0f;
            if (d < hw) {
                const float h = -5.0f * std::sqrt(1.0f - (d / hw) * (d / hw));
                if (h < s.h) { s.h = h; s.kind = 4; s.pocket = -1; }
            }
        }
        for (const auto& sc : L.screws) {
            const float dx = x - sc[0], dy = y - sc[1], r = std::sqrt(dx * dx + dy * dy);
            if (r < 15.0f) {
                // chrome dome with a hex socket
                const float ang = std::atan2(dy, dx);
                const float hexR = 5.5f / std::cos(std::fmod(std::fabs(ang) + std::numbers::pi_v<float> / 6, std::numbers::pi_v<float> / 3) - std::numbers::pi_v<float> / 6);
                s.h = r < hexR ? 1.0f : 3.5f * std::sqrt(std::max(0.0f, 1.0f - (r / 15.0f) * (r / 15.0f)));
                s.kind = 6; s.pocket = -1;
            }
        }
        return s;
    };

    // Height + surface kind per pixel.
    std::vector<Surface> surf(static_cast<std::size_t>(W) * H);
    for (std::uint32_t py = 0; py < H; ++py)
        for (std::uint32_t px = 0; px < W; ++px)
            surf[static_cast<std::size_t>(py) * W + px] = surfaceAt((static_cast<float>(px) + 0.5f) / ppu, (static_cast<float>(py) + 0.5f) / ppu);
    auto hAt = [&](int px, int py) {
        px = std::clamp(px, 0, static_cast<int>(W) - 1); py = std::clamp(py, 0, static_cast<int>(H) - 1);
        return surf[static_cast<std::size_t>(py) * W + static_cast<std::size_t>(px)].h;
    };

    const V3 light = norm3({kLight[0], kLight[1], kLight[2]});
    const V3 view{0, 0, 1};
    const V3 half = norm3({light.x + view.x, light.y + view.y, light.z + view.z});
    const float lxy = std::sqrt(light.x * light.x + light.y * light.y);
    const float rise = light.z / lxy;   // height gained per unit travelled toward the light

    // Linear-light colours.
    auto lin = [](Rgba c) { return V3{srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b)}; };
    const V3 violet = lin(pal.violet), lime = lin(pal.lime);

    for (std::uint32_t py = 0; py < H; ++py) {
        for (std::uint32_t px = 0; px < W; ++px) {
            const Surface& s = surf[static_cast<std::size_t>(py) * W + px];
            const float x = (static_cast<float>(px) + 0.5f) / ppu, y = (static_cast<float>(py) + 0.5f) / ppu;
            // normal from the height field (heights in design units, step = one pixel)
            const float step = 1.0f / ppu;
            const float dhx = (hAt(static_cast<int>(px) + 1, static_cast<int>(py)) - hAt(static_cast<int>(px) - 1, static_cast<int>(py))) / (2 * step);
            const float dhy = (hAt(static_cast<int>(px), static_cast<int>(py) + 1) - hAt(static_cast<int>(px), static_cast<int>(py) - 1)) / (2 * step);
            V3 n = norm3({-dhx, -dhy, 1.0f});

            // shadow: march toward the light; any surface higher than the ray blocks it
            float shadow = 1.0f;
            if (s.h < -0.5f) {
                const float dirx = light.x / lxy, diry = light.y / lxy;
                for (float t = 1.5f; t < 40.0f; t += 1.5f) {
                    const float rayH = s.h + t * rise;
                    if (rayH >= 0.0f) break;
                    const float hh = hAt(static_cast<int>((x + dirx * t) * ppu), static_cast<int>((y + diry * t) * ppu));
                    if (hh > rayH + 0.3f) { shadow = 0.25f; break; }
                }
            }

            V3 albedo{0.010f, 0.010f, 0.012f};
            float specPow = 160.0f, specAmt = 0.9f, envAmt = 1.0f, ao = 1.0f;
            bool  metal = false;
            if (s.kind == 0 || s.kind == 5) {
                // polished obsidian: flow banding and a faint violet / green sheen inside the glass
                const float warp = fbm(x * 0.004f, y * 0.004f, 3) * 6.0f;
                const float band = 0.5f + 0.5f * std::sin(x * 0.006f + y * 0.017f + warp);
                const float grain = fbm(x * 0.05f, y * 0.05f, 2);
                const float sheen = std::pow(band, 6.0f) * 0.010f;
                albedo = {0.008f + 0.006f * grain + sheen * violet.x * 1.4f, 0.008f + 0.006f * grain + sheen * lime.y * 0.25f,
                          0.010f + 0.007f * grain + sheen * violet.z * 1.4f};
            } else if (s.kind == 3) {
                // milled floor: matte black stone, tool marks, darker toward the walls
                const PanelPocket& p = L.pockets[static_cast<std::size_t>(s.pocket)];
                float marks;
                if (p.rings || p.circle) {
                    const float dx = x - p.r.cx(), dy = y - p.r.cy();
                    marks = 0.5f + 0.5f * std::sin(std::sqrt(dx * dx + dy * dy) * 1.6f);
                } else {
                    marks = 0.5f + 0.5f * std::sin(y * 1.3f + fbm(x * 0.02f, y * 0.02f, 2) * 3.0f);
                }
                const float grain = fbm(x * 0.4f, y * 0.4f, 2);
                const float a = 0.0075f + 0.0045f * marks + 0.003f * grain;
                albedo = {a, a, a * 1.08f};
                specPow = 24.0f; specAmt = 0.12f; envAmt = 0.35f;
                ao = 0.35f + 0.65f * smooth01(kChamfer, kChamfer + p.depth * 2.2f, s.inside);
            } else if (s.kind == 1) {
                // polished chamfer: catches the light
                albedo = {0.030f, 0.030f, 0.034f};
                specPow = 70.0f; specAmt = 2.4f; envAmt = 2.4f;
            } else if (s.kind == 2) {
                albedo = {0.012f, 0.012f, 0.014f};
                specPow = 40.0f; specAmt = 0.5f; envAmt = 0.6f;
            } else if (s.kind == 4) {
                albedo = {0.006f, 0.006f, 0.008f};   // neon channel (the tube is drawn live)
                specPow = 30.0f; specAmt = 0.3f;
            } else if (s.kind == 6) {
                metal = true;
                albedo = {0.55f, 0.56f, 0.58f};
                specPow = 60.0f; specAmt = 1.2f;
            }

            const float diff = std::max(0.0f, dot3(n, light));
            const float spec = std::pow(std::max(0.0f, dot3(n, half)), specPow) * specAmt;
            // environment: a soft box up-left and a long diagonal reflection across the glass
            const V3 r{2 * n.z * n.x, 2 * n.z * n.y, 2 * n.z * n.z - 1};
            const float box = std::exp(-((r.x + 0.45f) * (r.x + 0.45f) + (r.y + 0.55f) * (r.y + 0.55f)) / 0.10f);
            const float u = (x / L.width) * 0.85f + (y / L.height) * 0.5f;
            const float streak = std::exp(-(u - 0.40f) * (u - 0.40f) / 0.0060f) * 0.75f + std::exp(-(u - 0.63f) * (u - 0.63f) / 0.0012f) * 0.30f
                               + std::exp(-(u - 0.12f) * (u - 0.12f) / 0.0100f) * 0.20f;
            const float fres = metal ? 0.6f : 0.045f + 0.6f * std::pow(1.0f - std::max(0.0f, n.z), 5.0f);
            const float env = (box * 0.9f + (s.kind == 0 || s.kind == 5 ? streak : 0.0f) + 0.012f) * fres * envAmt;

            V3 c{albedo.x * (0.06f + 0.94f * diff * shadow) * ao, albedo.y * (0.06f + 0.94f * diff * shadow) * ao,
                 albedo.z * (0.06f + 0.94f * diff * shadow) * ao};
            const float sp = spec * shadow * (metal ? 1.0f : 0.25f) + env;
            if (metal) { c.x += sp * albedo.x; c.y += sp * albedo.y; c.z += sp * albedo.z; }
            else       { c.x += sp; c.y += sp * 1.0f; c.z += sp * 1.04f; }
            // a little violet light spills out of the neon channels onto the slab
            std::uint8_t* o = &img.rgba[(static_cast<std::size_t>(py) * W + px) * 4];
            o[0] = linearToSrgb8(c.x); o[1] = linearToSrgb8(c.y); o[2] = linearToSrgb8(c.z); o[3] = 255;
        }
    }
    return img;
}

// =============================================================================================
//  Interaction
// =============================================================================================
PanelHit hitTestPanel(const PanelLayout& L, float x, float y) noexcept {
    for (std::size_t i = 0; i < L.knobs.size(); ++i) {
        const Rect& w = L.knobs[i].well;
        const float dx = x - w.cx(), dy = y - w.cy();
        if (dx * dx + dy * dy <= (w.w * 0.5f) * (w.w * 0.5f) || L.knobs[i].value.contains(x, y)) return {PanelHit::Kind::Knob, static_cast<int>(i)};
    }
    for (std::size_t i = 0; i < L.buttons.size(); ++i)
        if (L.buttons[i].rect.contains(x, y)) return {PanelHit::Kind::Button, static_cast<int>(i)};
    for (std::size_t i = 0; i < L.selectors.size(); ++i) {
        if (L.selectors[i].prev.contains(x, y)) return {PanelHit::Kind::SelectorPrev, static_cast<int>(i)};
        if (L.selectors[i].next.contains(x, y)) return {PanelHit::Kind::SelectorNext, static_cast<int>(i)};
        if (L.selectors[i].box.contains(x, y)) return {PanelHit::Kind::SelectorBox, static_cast<int>(i)};
    }
    if (L.bypass.contains(x, y)) return {PanelHit::Kind::Bypass, 0};
    return {};
}

void FxPanelController::stepChoice(int param, int delta) {
    const PanelParam& p = spec_.params[static_cast<std::size_t>(param)];
    const int n = static_cast<int>(p.choices.size());
    if (n < 2) return;
    const int cur = static_cast<int>(std::lround(host_.normalized(param) * static_cast<float>(n - 1)));
    const int next = ((cur + delta) % n + n) % n;
    host_.beginEdit(param);
    host_.setNormalized(param, static_cast<float>(next) / static_cast<float>(n - 1));
    host_.endEdit(param);
}

void FxPanelController::mouseDown(float x, float y, bool doubleClick, bool) {
    const PanelHit h = hitTestPanel(layout_, x, y);
    hover_ = h;
    switch (h.kind) {
    case PanelHit::Kind::Knob: {
        const int param = layout_.knobs[static_cast<std::size_t>(h.index)].param;
        if (doubleClick) {
            host_.beginEdit(param);
            host_.setNormalized(param, spec_.params[static_cast<std::size_t>(param)].defNormalized);
            host_.endEdit(param);
            return;
        }
        active_ = h;
        lastY_ = y;
        dragValue_ = host_.normalized(param);
        host_.beginEdit(param);
        break;
    }
    case PanelHit::Kind::Button: {
        const PanelButton& b = layout_.buttons[static_cast<std::size_t>(h.index)];
        const PanelParam& p = spec_.params[static_cast<std::size_t>(b.param)];
        host_.beginEdit(b.param);
        if (b.choice < 0) host_.setNormalized(b.param, host_.normalized(b.param) >= 0.5f ? 0.0f : 1.0f);
        else host_.setNormalized(b.param, p.choices.size() > 1 ? static_cast<float>(b.choice) / static_cast<float>(p.choices.size() - 1) : 0.0f);
        host_.endEdit(b.param);
        break;
    }
    case PanelHit::Kind::SelectorPrev: stepChoice(layout_.selectors[static_cast<std::size_t>(h.index)].param, -1); break;
    case PanelHit::Kind::SelectorNext:
    case PanelHit::Kind::SelectorBox:  stepChoice(layout_.selectors[static_cast<std::size_t>(h.index)].param, +1); break;
    case PanelHit::Kind::Bypass:       if (host_.hasBypass()) host_.setBypassed(!host_.bypassed()); break;
    case PanelHit::Kind::None:         break;
    }
}

void FxPanelController::mouseMove(float x, float y, bool fine) {
    if (active_.kind == PanelHit::Kind::Knob) {
        const int param = layout_.knobs[static_cast<std::size_t>(active_.index)].param;
        dragValue_ = std::clamp(dragValue_ + (lastY_ - y) / kDragUnitsForFullRange * (fine ? 0.15f : 1.0f), 0.0f, 1.0f);
        lastY_ = y;
        host_.setNormalized(param, dragValue_);
        return;
    }
    hover_ = hitTestPanel(layout_, x, y);
}

void FxPanelController::mouseUp(float, float) {
    if (active_.kind == PanelHit::Kind::Knob) host_.endEdit(layout_.knobs[static_cast<std::size_t>(active_.index)].param);
    active_ = {};
}

void FxPanelController::wheel(float x, float y, float notches, bool fine) {
    const PanelHit h = hitTestPanel(layout_, x, y);
    if (h.kind == PanelHit::Kind::Knob) {
        const int param = layout_.knobs[static_cast<std::size_t>(h.index)].param;
        host_.beginEdit(param);
        host_.setNormalized(param, std::clamp(host_.normalized(param) + notches * (fine ? 0.005f : 0.03f), 0.0f, 1.0f));
        host_.endEdit(param);
    } else if (h.kind == PanelHit::Kind::SelectorBox || h.kind == PanelHit::Kind::SelectorPrev || h.kind == PanelHit::Kind::SelectorNext) {
        stepChoice(layout_.selectors[static_cast<std::size_t>(h.index)].param, notches > 0 ? -1 : 1);
    }
}

} // namespace hc::ui

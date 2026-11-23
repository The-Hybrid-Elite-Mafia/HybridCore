#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/Parts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace hc::ui {

// =============================================================================================
//  Layout
// =============================================================================================
namespace {

constexpr float kMargin = 44.0f, kHeaderH = 210.0f, kFooterH = 112.0f, kBayPad = 22.0f, kGap = 26.0f;
constexpr float kBayDepth = 5.0f, kWellDepth = 14.0f, kWindowDepth = 7.0f, kButtonDepth = 8.0f, kMeterDepth = 10.0f;
constexpr float kSectionCaptionH = 60.0f;

int balancedCols(int cells, int maxCols) {
    if (cells <= 0) return 1;
    const int rows = (cells + maxCols - 1) / maxCols;
    return (cells + rows - 1) / rows;
}

float meterColW(const PanelMeter& m) {
    const float textW = 14.5f * static_cast<float>(m.name.size()) + 18.0f;
    return std::max(112.0f, textW);   // room for "-28.2" in the value window
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
                PanelButton b{param, c, {cx - 104, top + static_cast<float>(c) * (h + gap), 208, h}, p.choices[static_cast<std::size_t>(c)]};
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

namespace {
PanelLayout layoutAtOne(const PanelSpec& spec, int maxColsOverride, float rowMaxWOverride = 0.0f);
Rect zr(Rect r, float z) { return {r.x * z, r.y * z, r.w * z, r.h * z}; }
} // namespace

PanelLayout layoutFxPanel(const PanelSpec& spec, float zoom, int maxCols) {
    PanelLayout L = layoutAtOne(spec, maxCols);
    const float z = std::clamp(zoom, 0.25f, 4.0f);
    if (std::fabs(z - 1.0f) < 1e-6f) return L;
    L.zoom = z;
    L.width *= z; L.height *= z;
    for (Rect* r : {&L.header, &L.titleArea, &L.footer, &L.bypass, &L.logo, &L.meterBay}) *r = zr(*r, z);
    for (auto& p : L.pockets) { p.r = zr(p.r, z); p.radius *= z; }
    for (auto& g : L.grooves) { g.x0 *= z; g.y0 *= z; g.x1 *= z; g.y1 *= z; }
    for (auto& s : L.sections) { s.bay = zr(s.bay, z); s.captionArea = zr(s.captionArea, z); }
    for (auto& k : L.knobs) { k.well = zr(k.well, z); k.caption = zr(k.caption, z); k.value = zr(k.value, z); }
    for (auto& b : L.buttons) b.rect = zr(b.rect, z);
    for (auto& s : L.selectors) { s.caption = zr(s.caption, z); s.box = zr(s.box, z); s.prev = zr(s.prev, z); s.next = zr(s.next, z); }
    for (auto& m : L.meters) { m.bar = zr(m.bar, z); m.caption = zr(m.caption, z); m.value = zr(m.value, z); }
    for (auto& c : L.captions) c.r = zr(c.r, z);
    for (auto& s : L.screws) { s[0] *= z; s[1] *= z; }
    return L;
}

namespace {
PanelLayout layoutAtOne(const PanelSpec& spec, int maxColsOverride, float rowMaxWOverride) {
    PanelLayout L;
    L.name = spec.name;
    L.blurb = spec.blurb;
    std::vector<PanelGroup> groups = spec.groups;
    if (groups.empty()) groups.push_back({"", 0, static_cast<int>(spec.params.size())});

    // ---- sections: flow left to right, wrap at a comfortable width -----------------------
    const bool single = groups.size() == 1;
    const int  maxCols = maxColsOverride > 0 ? std::clamp(maxColsOverride, 1, 12) : single ? kPanelMaxCols : 4;
    struct Placed { Rect bay; int g; int cols; };
    std::vector<Placed> placed;
    const float rowMaxW = rowMaxWOverride > 0.0f ? rowMaxWOverride : single ? 1e9f : 2000.0f;
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
    L.hasBypass = spec.hasBypass;
    if (spec.hasBypass) addRectPocket(L, L.bypass.inset(-6), 16, kButtonDepth);
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
        if (capH > 0) L.grooves.push_back({pl.bay.x + 24, pl.bay.y + capH + 2, pl.bay.right() - 24, pl.bay.y + capH + 2, true});
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
} // namespace

// =============================================================================================
//  The HEM frame look: the same layout, scaled into the glass bed of Steven's frame
// =============================================================================================
namespace {
Rect bodyOf(const PanelLayout& L) {   // the section bays and the meter bay together
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    auto add = [&](const Rect& r) {
        if (r.w <= 0 || r.h <= 0) return;
        x0 = std::min(x0, r.x); y0 = std::min(y0, r.y); x1 = std::max(x1, r.right()); y1 = std::max(y1, r.bottom());
    };
    for (const PanelSection& sec : L.sections) add(sec.bay);
    add(L.meterBay);
    return x1 > x0 ? Rect{x0, y0, x1 - x0, y1 - y0} : Rect{};
}
} // namespace

PanelLayout layoutFxPanelFramed(const PanelSpec& spec) {
    using namespace fxframe;
    const Rect C = kContent;
    const float wantAspect = C.w / C.h;
    // Try every column count and row width; keep the one that makes the controls biggest
    // (a tie goes to the shape closest to the bed's own shape).
    PanelLayout best;
    Rect bestBody;
    float bestS = -1.0f, bestAspectErr = 1e9f;
    for (const float rowW : {0.0f, 1400.0f, 2000.0f, 2600.0f, 3200.0f, 4200.0f, 1e9f}) {
        for (int cols = 1; cols <= 12; ++cols) {
            PanelLayout L = layoutAtOne(spec, cols, rowW);
            const Rect b = bodyOf(L);
            if (b.w <= 0 || b.h <= 0) continue;
            const float s = std::min({C.w / b.w, C.h / b.h, kMaxScale});
            const float aspectErr = std::fabs(std::log((b.w / b.h) / wantAspect));
            if (s > bestS + 1e-3f || (std::fabs(s - bestS) <= 1e-3f && aspectErr < bestAspectErr - 1e-4f)) {
                bestS = s; bestAspectErr = aspectErr; bestBody = b; best = std::move(L);
            }
        }
    }
    PanelLayout L = std::move(best);
    if (bestS <= 0) bestS = 1.0f;
    const float s = bestS;
    const float ox = C.cx() - bestBody.cx() * s, oy = C.cy() - bestBody.cy() * s;
    auto T = [&](Rect r) { return Rect{ox + r.x * s, oy + r.y * s, r.w * s, r.h * s}; };

    // The stand-alone plate's header, crown and bypass pockets and its two full-width neon
    // channels are the first ones laid out; the frame has its own top strip instead.
    const std::size_t chrome = std::min<std::size_t>(L.hasBypass ? 3 : 2, L.pockets.size());
    L.pockets.erase(L.pockets.begin(), L.pockets.begin() + static_cast<std::ptrdiff_t>(chrome));
    L.grooves.erase(L.grooves.begin(), L.grooves.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, L.grooves.size())));

    for (auto& p : L.pockets) { p.r = T(p.r); p.radius *= s; }
    for (auto& g : L.grooves) { g.x0 = ox + g.x0 * s; g.y0 = oy + g.y0 * s; g.x1 = ox + g.x1 * s; g.y1 = oy + g.y1 * s; }
    for (auto& sec : L.sections) { sec.bay = T(sec.bay); sec.captionArea = T(sec.captionArea); }
    for (auto& k : L.knobs) { k.well = T(k.well); k.caption = T(k.caption); k.value = T(k.value); }
    for (auto& b : L.buttons) b.rect = T(b.rect);
    for (auto& sel : L.selectors) { sel.caption = T(sel.caption); sel.box = T(sel.box); sel.prev = T(sel.prev); sel.next = T(sel.next); }
    for (auto& m : L.meters) { m.bar = T(m.bar); m.caption = T(m.caption); m.value = T(m.value); }
    for (auto& c : L.captions) c.r = T(c.r);
    if (L.meterBay.w > 0) L.meterBay = T(L.meterBay);

    L.zoom = s;
    L.width = kW;
    L.height = kH;
    L.header = {0, 0, kW, kBed.y};
    L.titleArea = {kTitleX, kTitleBase - 110, kTitleMaxW, 200};
    L.footer = {};
    L.logo = kEmblem;
    L.bypass = L.hasBypass ? kBypass : Rect{};
    if (L.hasBypass) addRectPocket(L, kBypass.inset(-6), 16, kButtonDepth);
    L.screws.clear();
    L.framed = true;
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
    // A zoomed plate is the same plate, bigger: every depth and detail scales with it.
    const float z = L.zoom, kCh = kChamfer * z;
    const float outerR = 38.0f * z, bevel = 16.0f * z;
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
            if (d < kCh) { h = -d; k = 1; }
            else { h = -kCh - (d - kCh) * 6.0f; k = 2; }
            if (h <= -p.depth * z) { h = -p.depth * z; k = 3; }
            if (h < s.h) { s.h = h; s.pocket = i; s.inside = d; s.kind = k; }
        }
        for (const PanelGroove& g : L.grooves) {
            const float vx = g.x1 - g.x0, vy = g.y1 - g.y0, len2 = vx * vx + vy * vy;
            const float t = std::clamp(((x - g.x0) * vx + (y - g.y0) * vy) / std::max(len2, 1e-6f), 0.0f, 1.0f);
            const float dx = x - (g.x0 + t * vx), dy = y - (g.y0 + t * vy);
            const float d = std::sqrt(dx * dx + dy * dy);
            const float hw = 6.0f * z;
            if (d < hw) {
                const float h = -5.0f * z * std::sqrt(1.0f - (d / hw) * (d / hw));
                if (h < s.h) { s.h = h; s.kind = 4; s.pocket = -1; }
            }
        }
        for (const auto& sc : L.screws) {
            const float dx = x - sc[0], dy = y - sc[1], r = std::sqrt(dx * dx + dy * dy) / z;
            if (r < 15.0f) {
                // chrome dome with a hex socket
                const float ang = std::atan2(dy, dx);
                const float hexR = 5.5f / std::cos(std::fmod(std::fabs(ang) + std::numbers::pi_v<float> / 6, std::numbers::pi_v<float> / 3) - std::numbers::pi_v<float> / 6);
                s.h = z * (r < hexR ? 1.0f : 3.5f * std::sqrt(std::max(0.0f, 1.0f - (r / 15.0f) * (r / 15.0f))));
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
            const float tx = x / z, ty = y / z;   // texture patterns are laid out at zoom 1, so they scale with the plate
            // normal from the height field (heights in design units, step = one pixel)
            const float step = 1.0f / ppu;
            const float dhx = (hAt(static_cast<int>(px) + 1, static_cast<int>(py)) - hAt(static_cast<int>(px) - 1, static_cast<int>(py))) / (2 * step);
            const float dhy = (hAt(static_cast<int>(px), static_cast<int>(py) + 1) - hAt(static_cast<int>(px), static_cast<int>(py) - 1)) / (2 * step);
            V3 n = norm3({-dhx, -dhy, 1.0f});

            // shadow: march toward the light; any surface higher than the ray blocks it
            float shadow = 1.0f;
            if (s.h < -0.5f) {
                const float dirx = light.x / lxy, diry = light.y / lxy;
                for (float t = 1.5f * z; t < 40.0f * z; t += 1.5f * z) {
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
                const float warp = fbm(tx * 0.004f, ty * 0.004f, 3) * 6.0f;
                const float band = 0.5f + 0.5f * std::sin(tx * 0.006f + ty * 0.017f + warp);
                const float grain = fbm(tx * 0.05f, ty * 0.05f, 2);
                const float sheen = std::pow(band, 6.0f) * 0.010f;
                albedo = {0.008f + 0.006f * grain + sheen * violet.x * 1.4f, 0.008f + 0.006f * grain + sheen * lime.y * 0.25f,
                          0.010f + 0.007f * grain + sheen * violet.z * 1.4f};
            } else if (s.kind == 3) {
                // milled floor: matte black stone, tool marks, darker toward the walls
                const PanelPocket& p = L.pockets[static_cast<std::size_t>(s.pocket)];
                float marks;
                if (p.rings || p.circle) {
                    const float dx = x - p.r.cx(), dy = y - p.r.cy();
                    marks = 0.5f + 0.5f * std::sin(std::sqrt(dx * dx + dy * dy) / z * 1.6f);
                } else {
                    marks = 0.5f + 0.5f * std::sin(ty * 1.3f + fbm(tx * 0.02f, ty * 0.02f, 2) * 3.0f);
                }
                const float grain = fbm(tx * 0.4f, ty * 0.4f, 2);
                const float a = 0.0075f + 0.0045f * marks + 0.003f * grain;
                albedo = {a, a, a * 1.08f};
                specPow = 24.0f; specAmt = 0.12f; envAmt = 0.35f;
                ao = 0.35f + 0.65f * smooth01(kCh, kCh + p.depth * z * 2.2f, s.inside);
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

Image renderFxPlateFramed(const PanelLayout& L, const FxPalette& pal, float ppu, const Image& frameArt) {
    const auto W = static_cast<std::uint32_t>(std::lround(L.width * ppu)), H = static_cast<std::uint32_t>(std::lround(L.height * ppu));
    // 1) the frame art at the output size
    Image img = frameArt.empty() || W == 0 || H == 0 ? Image{} : resizeSrgb(frameArt, W, H);
    if (img.empty() || img.width != W || img.height != H) {   // no art on disk: plain dark chassis
        img = Image{};
        img.width = W; img.height = H;
        img.rgba.assign(static_cast<std::size_t>(W) * H * 4, 255);
        for (std::size_t i = 0; i < img.rgba.size(); i += 4) { img.rgba[i] = img.rgba[i + 1] = 14; img.rgba[i + 2] = 17; }
    }
    if (W == 0 || H == 0) return img;

    // 2) the big panels (sections, meter bay): smoked glass cut into the bed, the board faintly showing
    auto same = [](const Rect& a, const Rect& b) {
        return std::fabs(a.x - b.x) < 0.01f && std::fabs(a.y - b.y) < 0.01f && std::fabs(a.w - b.w) < 0.01f && std::fabs(a.h - b.h) < 0.01f;
    };
    auto isBay = [&](const PanelPocket& p) {
        if (p.circle) return false;
        for (const PanelSection& sec : L.sections) if (same(p.r, sec.bay)) return true;
        return L.meterBay.w > 0 && same(p.r, L.meterBay);
    };
    auto lit = [](float nx, float ny) { return std::max(0.0f, -(nx * 0.6f + ny * 0.8f)); };   // light from the top-left
    for (const PanelPocket& p : L.pockets) {
        if (!isBay(p)) continue;
        const float margin = 26.0f;   // soft contact shadow round each panel
        const int x0 = std::max(0, static_cast<int>((p.r.x - margin) * ppu)), x1 = std::min(static_cast<int>(W), static_cast<int>((p.r.right() + margin) * ppu) + 1);
        const int y0 = std::max(0, static_cast<int>((p.r.y - margin) * ppu)), y1 = std::min(static_cast<int>(H), static_cast<int>((p.r.bottom() + margin) * ppu) + 1);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float ux = (static_cast<float>(px) + 0.5f) / ppu, uy = (static_cast<float>(py) + 0.5f) / ppu;
                const float d = sdRound(ux, uy, p.r, p.radius);
                std::uint8_t* o = &img.rgba[(static_cast<std::size_t>(py) * W + static_cast<std::size_t>(px)) * 4];
                float R = srgb8ToLinear(o[0]), G = srgb8ToLinear(o[1]), B = srgb8ToLinear(o[2]);
                if (d >= 0.0f) {
                    const float k = 0.42f + 0.58f * std::clamp(d / margin, 0.0f, 1.0f);
                    R *= k; G *= k; B *= k;
                } else {
                    const float e = 0.75f;
                    float nx = sdRound(ux + e, uy, p.r, p.radius) - sdRound(ux - e, uy, p.r, p.radius);
                    float ny = sdRound(ux, uy + e, p.r, p.radius) - sdRound(ux, uy - e, p.r, p.radius);
                    const float l = std::sqrt(nx * nx + ny * ny) + 1e-6f;
                    nx /= l; ny /= l;
                    const float t = std::clamp((uy - p.r.y) / p.r.h, 0.0f, 1.0f);
                    const float rim = std::clamp(1.0f + d / 4.0f, 0.0f, 1.0f);   // 4 px bevel at the cut edge
                    const float li = lit(-nx, -ny);
                    const float k = 0.30f;
                    R = R * k + 0.0045f; G = G * k + 0.0055f; B = B * k + 0.0075f;
                    const float sheen = 0.010f * (1.0f - t) * (1.0f - t);
                    R += sheen; G += sheen; B += sheen * 1.15f;
                    R += rim * (0.10f * li + 0.012f); G += rim * (0.11f * li + 0.014f); B += rim * (0.12f * li + 0.018f);
                }
                o[0] = linearToSrgb8(std::min(1.0f, R)); o[1] = linearToSrgb8(std::min(1.0f, G)); o[2] = linearToSrgb8(std::min(1.0f, B)); o[3] = 255;
            }
        }
    }

    // 3) every knob well, value window, button and meter pocket: the machined obsidian insert,
    //    exactly as the stand-alone plate renders it (same chamfers, milled floors, shadows)
    const Image obs = renderFxPlate(L, pal, ppu);
    for (const PanelPocket& p : L.pockets) {
        if (isBay(p)) continue;
        const Rect bb = p.r.inset(-2);
        const int x0 = std::max(0, static_cast<int>(bb.x * ppu)), x1 = std::min(static_cast<int>(W), static_cast<int>(bb.right() * ppu) + 1);
        const int y0 = std::max(0, static_cast<int>(bb.y * ppu)), y1 = std::min(static_cast<int>(H), static_cast<int>(bb.bottom() * ppu) + 1);
        for (int py = y0; py < y1; ++py) {
            for (int px = x0; px < x1; ++px) {
                const float ux = (static_cast<float>(px) + 0.5f) / ppu, uy = (static_cast<float>(py) + 0.5f) / ppu;
                const float a = std::clamp(pocketInside(p, ux, uy) * ppu + 0.5f, 0.0f, 1.0f);   // 1-pixel soft edge
                if (a <= 0.0f) continue;
                const std::size_t i = (static_cast<std::size_t>(py) * W + static_cast<std::size_t>(px)) * 4;
                for (int ch = 0; ch < 3; ++ch) {
                    const float v = srgb8ToLinear(img.rgba[i + static_cast<std::size_t>(ch)]) * (1.0f - a) + srgb8ToLinear(obs.rgba[i + static_cast<std::size_t>(ch)]) * a;
                    img.rgba[i + static_cast<std::size_t>(ch)] = linearToSrgb8(std::min(1.0f, v));
                }
            }
        }
    }
    return img;
}

// =============================================================================================
//  Meter text
// =============================================================================================
namespace {
std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    // a value that rounds to zero is shown without a sign ("0.0", not "-0.0" or "+0.0")
    bool zero = true;
    for (const char* c = b; *c != '\0'; ++c)
        if (*c >= '1' && *c <= '9') { zero = false; break; }
    if (zero && (b[0] == '-' || b[0] == '+')) return std::string(b + 1);
    return b;
}
} // namespace

std::string formatPanelMeter(const PanelMeter& m, float v) {
    const std::string& u = m.unit;
    if (u == "note") {
        static const char* kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
        const int n = static_cast<int>(std::lround(v));
        if (n <= 0) return "--";
        return std::string(kNames[n % 12]) + std::to_string(n / 12 - 1);
    }
    if (u == "Hz") {
        if (v < 1.0f) return "--";
        return v >= 1000.0f ? fmt("%.1fk", v / 1000.0) : fmt("%.0f", v);
    }
    if (u == "corr") return fmt("%+.2f", v);
    if (u == "cents") return fmt("%+.0f", v);
    if (u == "%" || u == "ms") return fmt("%.0f", v);
    if (u == "dBFS") return v <= -95.0f ? "--" : fmt("%.1f", v);
    if (u == "LUFS" || u == "dBTP") return v <= -70.0f ? "--" : fmt("%.1f", v);   // nothing measured yet (silence)
    if (m.centred) return fmt("%+.1f", v);
    return fmt("%.1f", v);
}

float panelMeterFraction(const PanelMeter& m, float v) noexcept {
    if (m.centred) {
        const float half = std::max(std::fabs(m.min), std::fabs(m.max));
        return half > 0 ? std::clamp(v / half, -1.0f, 1.0f) : 0.0f;
    }
    if (m.unit == "note" && v <= 0.0f) return 0.0f;
    if (m.unit == "Hz" && v < 1.0f) return 0.0f;
    if (m.max <= m.min) return 0.0f;
    if (m.max <= 0.0f && m.unit == "dB") return std::clamp(v / m.min, 0.0f, 1.0f);   // reduction: 0 dB empty, min full
    return std::clamp((v - m.min) / (m.max - m.min), 0.0f, 1.0f);
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
        dragValue_ = std::clamp(dragValue_ + (lastY_ - y) / (kDragUnitsForFullRange * layout_.zoom) * (fine ? 0.15f : 1.0f), 0.0f, 1.0f);
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

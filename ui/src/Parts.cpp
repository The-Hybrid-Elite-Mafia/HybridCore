#include "hybrid/ui/Parts.hpp"
#include "hybrid/ui/Faceplate.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace hc::ui {

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- small vector maths ------------------------------------------------------------
struct V3 {
    float x, y, z;
};
V3    norm(V3 v) { const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); return {v.x / l, v.y / l, v.z / l}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
float smooth(float e0, float e1, float x) { const float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3 - 2 * t); }
float mix(float a, float b, float t) { return a + (b - a) * t; }

const V3 kL    = norm({kLight[0], kLight[1], kLight[2]});
const V3 kBox  = norm({-0.40f, -0.82f, 0.46f});   // studio softbox, up and to the left
const V3 kFill = norm({0.80f, -0.25f, 0.55f});    // weak fill from the right
const float kLightAngle = std::atan2(kL.y, kL.x);

// Reflection of the view ray (0,0,-1 towards the screen) about n.
V3 reflectView(V3 n) { return {2 * n.z * n.x, 2 * n.z * n.y, 2 * n.z * n.z - 1}; }

// Studio environment seen in chrome: bright ceiling, dark floor, dark horizon band,
// a hot softbox up-left and a weak fill on the right. Linear radiance.
float env(V3 r) {
    const float up = -r.y;
    float e = 0.05f + 0.60f * smooth(-0.35f, 0.95f, up);
    const float hb = (up - 0.08f) / 0.22f;
    e *= 1.0f - 0.65f * std::exp(-hb * hb);
    e += 1.70f * std::pow(std::max(dot(r, kBox), 0.0f), 28.0f);
    e += 0.25f * std::pow(std::max(dot(r, kFill), 0.0f), 6.0f);
    return e;
}

float lambert(V3 n) { return std::max(dot(n, kL), 0.0f) / kL.z; }   // 1.0 for a face looking at the viewer

// ---- noise -------------------------------------------------------------------------
std::uint32_t hash(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float hashf(int i, int j, int seed) {
    const std::uint32_t h = hash(static_cast<std::uint32_t>(i) * 73856093U ^ static_cast<std::uint32_t>(j) * 19349663U ^ static_cast<std::uint32_t>(seed) * 83492791U);
    return static_cast<float>(h & 0xFFFFFF) / 8388607.5f - 1.0f;   // -1..1
}
float vnoise1(float x, int seed) {
    const float fl = std::floor(x);
    const int   i  = static_cast<int>(fl);
    const float f  = x - fl, s = f * f * (3 - 2 * f);
    return mix(hashf(i, 0, seed), hashf(i + 1, 0, seed), s);
}
float vnoise2(float x, float y, int seed) {
    const float fx = std::floor(x), fy = std::floor(y);
    const int   i = static_cast<int>(fx), j = static_cast<int>(fy);
    const float u = x - fx, v = y - fy, su = u * u * (3 - 2 * u), sv = v * v * (3 - 2 * v);
    return mix(mix(hashf(i, j, seed), hashf(i + 1, j, seed), su), mix(hashf(i, j + 1, seed), hashf(i + 1, j + 1, seed), su), sv);
}
// Horizontal brushed-metal streaks, -1..1.
float brushed(float x, float y, int seed) {
    return 0.55f * vnoise2(x * 0.035f, y * 1.25f, seed) + 0.30f * vnoise2(x * 0.012f, y * 0.45f, seed + 7) +
           0.15f * vnoise2(x * 0.25f, y * 2.0f, seed + 13);
}

// ---- shapes -------------------------------------------------------------------------
float cover(float d) { return clamp01(0.5f - d); }   // d: signed distance in pixels (< 0 inside)
float sdRoundRect(float px, float py, float hw, float hh, float r) {
    const float qx = std::fabs(px) - hw + r, qy = std::fabs(py) - hh + r;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}
float sdSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
    const float h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay));
    const float dx = pax - bax * h, dy = pay - bay * h;
    return std::sqrt(dx * dx + dy * dy);
}
float sdTriangle(float px, float py, const float (&v)[6]) {
    float d = 1e9f;
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        d = std::min(d, sdSegment(px, py, v[i * 2], v[i * 2 + 1], v[j * 2], v[j * 2 + 1]));
    }
    // inside test (same sign of all edge cross products)
    bool pos = false, neg = false;
    for (int i = 0; i < 3; ++i) {
        const int j = (i + 1) % 3;
        const float c = (v[j * 2] - v[i * 2]) * (py - v[i * 2 + 1]) - (v[j * 2 + 1] - v[i * 2 + 1]) * (px - v[i * 2]);
        (c > 0 ? pos : neg) = true;
    }
    return (pos && neg) ? d : -d;
}

// Outward unit gradient of the rounded-rect SDF (finite difference).
void rrGradient(float px, float py, float hw, float hh, float r, float& gx, float& gy) {
    const float e = 0.5f;
    gx = sdRoundRect(px + e, py, hw, hh, r) - sdRoundRect(px - e, py, hw, hh, r);
    gy = sdRoundRect(px, py + e, hw, hh, r) - sdRoundRect(px, py - e, hw, hh, r);
    const float l = std::sqrt(gx * gx + gy * gy);
    if (l > 1e-6f) { gx /= l; gy /= l; } else { gx = gy = 0; }
}

// ---- canvas of linear premultiplied RGBA ----------------------------------------------
struct Pix {
    float r = 0, g = 0, b = 0, a = 0;
};
struct Raster {
    int w = 0, h = 0;
    std::vector<Pix> p;
    Raster(int W, int H) : w(W), h(H), p(static_cast<std::size_t>(W) * H) {}
    Pix& at(int x, int y) { return p[static_cast<std::size_t>(y) * w + x]; }
    // Calls f(px, py) with pixel-centre coordinates relative to the canvas centre.
    template <typename F> void each(F&& f) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) at(x, y) = f(static_cast<float>(x) + 0.5f - w * 0.5f, static_cast<float>(y) + 0.5f - h * 0.5f);
    }
};
Pix grey(float v, float a) { return {v * a, v * a, v * a, a}; }                 // opaque-ish colour, premultiplied
Pix addOnly(float v) { return {v, v, v, 0.0f}; }                                  // pure light (additive)

// ---- faceplate cut-outs ------------------------------------------------------------
Raster cutSocket(const Image& fp) {
    const Circle s = plate::kSockets[2];
    const int half = 100;
    Raster c(half * 2, half * 2);
    const int sx0 = static_cast<int>(s.cx) - half, sy0 = static_cast<int>(s.cy) - half;
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const float dx = static_cast<float>(x) + 0.5f - half, dy = static_cast<float>(y) + 0.5f - half;
            const float a  = clamp01((plate::kSocketOuterRadius - std::sqrt(dx * dx + dy * dy)) / 1.5f + 0.5f);
            const std::uint8_t* px = fp.at(static_cast<std::uint32_t>(sx0 + x), static_cast<std::uint32_t>(sy0 + y));
            c.at(x, y) = {srgb8ToLinear(px[0]) * a, srgb8ToLinear(px[1]) * a, srgb8ToLinear(px[2]) * a, a};
        }
    return c;
}

// Glass window: the display bezel, compacted to a 9-slice tile (60 px corners, 16 px middle).
constexpr int kGlassCorner = 60, kGlassMid = 16;
Raster cutGlass(const Image& fp) {
    const Rect b = plate::kDisplayBezel;
    const int n = kGlassCorner * 2 + kGlassMid;
    Raster c(n, n);
    auto mapX = [&](int t) { return t < kGlassCorner ? static_cast<int>(b.x) + t
                                  : t < kGlassCorner + kGlassMid ? 3350 + (t - kGlassCorner)
                                  : static_cast<int>(b.right()) - kGlassCorner + (t - kGlassCorner - kGlassMid); };
    auto mapY = [&](int t) { return t < kGlassCorner ? static_cast<int>(b.y) + t
                                  : t < kGlassCorner + kGlassMid ? 340 + (t - kGlassCorner)
                                  : static_cast<int>(b.bottom()) - kGlassCorner + (t - kGlassCorner - kGlassMid); };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int sx = mapX(x), sy = mapY(y);
            const float d = sdRoundRect(static_cast<float>(sx) + 0.5f - b.cx(), static_cast<float>(sy) + 0.5f - b.cy(), b.w * 0.5f, b.h * 0.5f,
                                        plate::kDisplayBezelRadius);
            const float a = clamp01(0.5f - d / 1.25f);
            const std::uint8_t* px = fp.at(static_cast<std::uint32_t>(sx), static_cast<std::uint32_t>(sy));
            c.at(x, y) = {srgb8ToLinear(px[0]) * a, srgb8ToLinear(px[1]) * a, srgb8ToLinear(px[2]) * a, a};
        }
    return c;
}

// ---- knob ------------------------------------------------------------------------
Raster knobShadow() {
    Raster c(128, 128);
    c.each([](float x, float y) {
        const float rn = std::sqrt(x * x + y * y) / 64.0f;
        return grey(0.0f, 0.70f * std::pow(1.0f - smooth(0.40f, 1.0f, rn), 1.4f));
    });
    return c;
}

// Spans the socket outer diameter (256 px = 2 × socket outer radius).
Raster knobTrack() {
    Raster c(256, 256);
    c.each([](float x, float y) {
        const float r = std::sqrt(x * x + y * y), rs = r / 128.0f;
        const float a = cover((knob::kTrackInner - rs) * 128.0f) * cover((rs - (knob::kRimInner + 0.012f)) * 128.0f);
        if (a <= 0) return Pix{};
        const float nx = x / std::max(r, 1e-3f), ny = y / std::max(r, 1e-3f);
        // The rim shades the outer part of the ring on the lit side; a faint lip catches light opposite.
        const float outer = smooth(0.70f, knob::kRimInner, rs);
        const float lit   = -(nx * kL.x + ny * kL.y) / std::sqrt(kL.x * kL.x + kL.y * kL.y);   // 1 facing the light
        float v = 0.020f * (1.0f - 0.55f * outer * clamp01(lit)) + 0.030f * outer * clamp01(-lit);
        v += 0.010f * vnoise2(x * 0.3f, y * 0.3f, 41);
        return grey(std::max(v, 0.004f), a);
    });
    return c;
}

// Body spans its own diameter (256 px = 2 × body radius).
constexpr float kCapEdge = 0.715f, kGroove = 0.775f;
Raster knobBody() {
    Raster c(256, 256);
    c.each([](float x, float y) {
        const float r = std::sqrt(x * x + y * y), rb = r / 127.0f;
        const float a = cover(r - 127.0f);
        if (a <= 0) return Pix{};
        const float th = std::atan2(y, x);
        const float g  = 0.55f * vnoise1(r * 1.9f, 1) + 0.45f * vnoise1(r * 0.55f, 2);          // spun rings
        const float sw = vnoise2(th * 4.0f + 20.0f, r * 0.05f, 3);                              // machining swirl (rotates)
        const float cap     = 0.050f * (1.0f + 0.40f * g + 0.25f * sw);
        const float chamfer = 0.42f * (1.0f + 0.12f * g);
        const float ridge   = 0.5f + 0.5f * std::cos(th * 64.0f);
        const float t       = clamp01((rb - kGroove) / (1.0f - kGroove));
        const float skirt   = 0.38f * mix(0.30f, 1.15f, std::pow(ridge, 0.8f)) * (1.0f - 0.25f * t);
        float v = mix(cap, chamfer, smooth(kCapEdge - 0.008f, kCapEdge + 0.008f, rb));
        v = mix(v, skirt, smooth(kGroove - 0.004f, kGroove + 0.010f, rb));
        const float gd = (rb - kGroove) * 127.0f / 1.3f;
        v *= 1.0f - 0.75f * std::exp(-gd * gd);                                                  // dark groove
        return Pix{v * 0.98f * a, v * a, v * 1.04f * a, a};
    });
    return c;
}

float knobTilt(float rb) {   // surface tilt (radians) of the rotationally symmetric knob profile
    static const float pts[][2] = {{0.0f, 0.0f}, {0.70f, 8.0f}, {kCapEdge, 42.0f}, {0.76f, 42.0f}, {kGroove, 10.0f},
                                   {0.80f, 22.0f}, {0.93f, 48.0f}, {1.0f, 80.0f}};
    for (std::size_t i = 1; i < std::size(pts); ++i)
        if (rb <= pts[i][0]) {
            const float t = (rb - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
            return mix(pts[i - 1][1], pts[i][1], t) * kPi / 180.0f;
        }
    return 80.0f * kPi / 180.0f;
}

Raster knobLight() {
    Raster c(256, 256);
    c.each([](float x, float y) {
        const float r = std::sqrt(x * x + y * y), rb = r / 127.0f;
        const float a = cover(r - 127.0f);
        if (a <= 0) return Pix{};
        const float dx = x / std::max(r, 1e-3f), dy = y / std::max(r, 1e-3f);
        const float tilt = knobTilt(rb);
        const V3 n = {dx * std::sin(tilt), dy * std::sin(tilt), std::cos(tilt)};
        const V3 R = reflectView(n);
        const float f    = 0.40f + 0.60f * lambert(n);
        const float dark = 1.0f - std::min(f, 1.0f);
        float spec = 0.10f * std::max(f - 1.0f, 0.0f);
        const float th = std::atan2(dy, dx), cd = std::cos(th - kLightAngle);
        const float capW = 1.0f - smooth(kCapEdge - 0.01f, kCapEdge + 0.01f, rb);
        // Spun metal: anisotropic streak along the radius towards (and away from) the light.
        spec += capW * std::pow(std::fabs(cd), 22.0f) * (cd > 0 ? 0.30f : 0.13f) * (0.25f + 0.75f * smooth(0.08f, 0.65f, rb));
        spec += capW * 0.20f * std::pow(std::max(dot(R, kBox), 0.0f), 50.0f);
        const float chW = smooth(kCapEdge - 0.01f, kCapEdge + 0.01f, rb) * (1.0f - smooth(kGroove - 0.01f, kGroove + 0.005f, rb));
        spec += chW * (1.1f * std::pow(std::max(dot(R, kBox), 0.0f), 6.0f) + 0.22f * env(R));
        const float skW = smooth(kGroove, kGroove + 0.015f, rb);
        spec += skW * 0.75f * env(R);
        const float rim = smooth(0.95f, 0.99f, rb) * std::pow(std::max(-(dx * 0.62f + dy * 0.78f), 0.0f), 2.0f);
        spec += 0.35f * rim;
        return Pix{spec * a, spec * a, spec * 1.02f * a, dark * a};
    });
    return c;
}

Raster knobIndicator() {
    Raster c(24, 96);
    for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            const float d    = sdSegment(px, py, 12.0f, 12.0f, 12.0f, 84.0f) - 3.6f;
            const float fill = cover(d), edge = cover(d - 2.2f);
            const float v = 0.95f * fill;
            c.at(x, y) = {v, v, v, fill + 0.55f * (edge - fill)};
        }
    return c;
}

// ---- transport cap -------------------------------------------------------------------
Raster capDark() {
    Raster c(192, 192);
    c.each([](float x, float y) {
        const float r = std::sqrt(x * x + y * y), rn = r / 91.0f;
        const float a = cover(r - 91.0f);
        if (a <= 0) return Pix{};
        const float dx = x / std::max(r, 1e-3f), dy = y / std::max(r, 1e-3f);
        const float tilt = std::atan(0.55f * std::pow(rn, 1.5f)) + smooth(0.88f, 1.0f, rn) * 0.9f;
        const V3 n = {dx * std::sin(tilt), dy * std::sin(tilt), std::cos(tilt)};
        const V3 R = reflectView(n);
        float v = 0.012f * (0.4f + 0.6f * lambert(n));
        v += 0.08f * env(R) * (0.6f + 0.4f * smooth(0.6f, 1.0f, rn));
        v += 1.40f * std::pow(std::max(dot(R, kBox), 0.0f), 90.0f);
        v += smooth(0.90f, 0.97f, rn) * 0.45f * env(R);
        return Pix{v * a, v * a, v * 1.05f * a, a};
    });
    return c;
}

Raster ringGlow() {
    Raster c(192, 192);
    c.each([](float x, float y) {
        const float rn = std::sqrt(x * x + y * y) / 96.0f;
        const float t1 = (rn - 0.86f) / 0.06f, t2 = (rn - 0.86f) / 0.17f;
        return addOnly(std::exp(-t1 * t1) + 0.12f * std::exp(-t2 * t2));
    });
    return c;
}

// ---- chrome caps -------------------------------------------------------------------
struct CapShape {
    float hw, hh, radius, bevel, dome;
    bool  grooves;
};
Raster chromeCap(int W, int H, CapShape s, int seed) {
    Raster c(W, H);
    c.each([&](float x, float y) {
        const float d = sdRoundRect(x, y, s.hw, s.hh, s.radius);
        const float a = cover(d);
        if (a <= 0) return Pix{};
        float gx, gy;
        rrGradient(x, y, s.hw, s.hh, s.radius, gx, gy);
        const float t     = clamp01(-d / s.bevel);
        const float slope = 1.7f * std::pow(1.0f - t, 1.6f);
        V3 n = {gx * slope + s.dome * x / s.hw, gy * slope + s.dome * y / s.hh, 1.0f};
        float albedo = 0.50f * (1.0f + 0.11f * brushed(x, y, seed));
        bool  channel = false;
        if (s.grooves) {
            channel = std::fabs(y) < 6.5f && std::fabs(x) < s.hw - 16.0f;
            if (std::fabs(x) < s.hw - 24.0f)
                for (float g0 : {-46.0f, -34.0f, -22.0f, 22.0f, 34.0f, 46.0f}) {
                    const float g = (y - g0) / 2.0f;
                    n.y += -1.2f * g * std::exp(-g * g);
                }
        }
        n = norm(n);
        const V3 R = reflectView(n);
        const float vgrad = 1.10f - 0.20f * (y + s.hh) / (2.0f * s.hh);
        float v = albedo * (0.35f + 0.65f * lambert(n)) * vgrad * 0.80f + 0.42f * env(R);
        if (channel) {
            const float edge = clamp01((std::fabs(y) - 4.5f) / 2.0f);
            const float inner = 0.015f + 0.05f * clamp01((y + 6.5f) / 13.0f);   // shadowed from the top
            v = mix(inner, v, edge);
        }
        return Pix{v * a, v * a, v * 1.03f * a, a};
    });
    return c;
}

Raster softRoundRect(int W, int H, float hw, float hh, float r, float blur, float alpha) {
    Raster c(W, H);
    c.each([&](float x, float y) {
        const float d = sdRoundRect(x, y, hw, hh, r);
        return grey(0.0f, alpha * (1.0f - smooth(-blur, blur, d)));
    });
    return c;
}

Raster faderLine() {
    Raster c(128, 20);
    c.each([](float x, float y) {
        const float d = sdSegment(x, y, -54.0f, 0.0f, 54.0f, 0.0f) - 3.0f;
        const float core = cover(d), halo = std::exp(-std::pow(std::max(d, 0.0f) / 3.0f, 2.0f));
        return Pix{core + 0.25f * halo, core + 0.25f * halo, core + 0.25f * halo, core * 0.95f};
    });
    return c;
}

Raster buttonLens() {
    Raster c(128, 96);
    c.each([](float x, float y) {
        const float d = sdRoundRect(x, y, 47.0f, 31.0f, 11.0f);
        const float v = clamp01(0.5f - d / 3.0f);
        const float bright = 0.70f + 0.30f * (1.0f - std::pow(std::fabs(x) / 47.0f, 2.0f)) * (1.0f - std::pow(std::fabs(y) / 31.0f, 2.0f));
        return Pix{v * bright, v * bright, v * bright, v * 0.88f};
    });
    return c;
}

Raster buttonHalo() {
    Raster c(160, 128);
    c.each([](float x, float y) {
        const float d = std::max(sdRoundRect(x, y, 50.0f, 34.0f, 14.0f), 0.0f);
        return addOnly(std::exp(-std::pow(d / 13.0f, 2.0f)));
    });
    return c;
}

// ---- glass ---------------------------------------------------------------------------
Raster glassSheen() {
    Raster c(128, 128);
    c.each([](float x, float y) {
        const float u = (x + 64.0f) / 128.0f, v = (y + 64.0f) / 128.0f;
        const float band = std::exp(-std::pow((v - 0.02f - 0.10f * u) / 0.30f, 2.0f));
        return addOnly(0.012f + 0.085f * band * (1.0f - 0.45f * u));
    });
    return c;
}

Raster glassRim() {
    Raster c(64, 64);
    c.each([](float x, float y) {
        const float d = std::fabs(sdRoundRect(x, y, 28.5f, 28.5f, 12.0f)) - 0.7f;
        const float line = cover(d);
        const float len  = std::sqrt(x * x + y * y);
        const float lit  = len > 0 ? clamp01(-(x * 0.6f + y * 0.8f) / len) : 0.0f;
        return addOnly(line * (0.12f + 0.55f * lit));
    });
    return c;
}

Raster innerShadow() {
    Raster c(64, 64);
    c.each([](float x, float y) {
        const float din = std::max(-sdRoundRect(x, y, 32.0f, 32.0f, 10.0f), 0.0f);
        const float top = y + 32.0f, left = x + 32.0f;   // distance from the lit-side edges
        const float tl  = std::min(top, left);
        return grey(0.0f, std::min(1.0f, 0.70f * std::exp(-tl / 6.0f) + 0.30f * std::exp(-din / 3.0f)));
    });
    return c;
}

Raster dropShadow() {
    Raster c(96, 96);
    c.each([](float x, float y) {
        const float d = sdRoundRect(x, y, 20.0f, 20.0f, 12.0f);
        return grey(0.0f, 0.60f * std::exp(-std::pow(std::max(d, 0.0f) / 11.0f, 2.0f)) * (d < 0 ? 1.0f : 1.0f));
    });
    return c;
}

// ---- lights -------------------------------------------------------------------------
Raster ledSegment() {
    Raster c(48, 20);
    c.each([](float x, float y) {
        const float a = cover(sdRoundRect(x, y, 21.0f, 8.0f, 3.0f));
        const float b = 0.78f + 0.22f * (1.0f - (y / 8.0f) * (y / 8.0f)) + 0.15f * std::exp(-std::pow((y + 4.0f) / 2.0f, 2.0f));
        return Pix{a * b, a * b, a * b, a};
    });
    return c;
}

Raster glowDot() {
    Raster c(64, 64);
    c.each([](float x, float y) {
        const float rn = std::sqrt(x * x + y * y) / 32.0f;
        return addOnly(std::exp(-rn * rn * 4.5f) * (1.0f - smooth(0.85f, 1.0f, rn)));
    });
    return c;
}

Raster lamp() {
    Raster c(128, 128);
    c.each([](float x, float y) {
        const float r = std::sqrt(x * x + y * y), rn = r / 60.0f;
        const float a = cover(r - 60.0f);
        if (a <= 0) return Pix{};
        const float v = 0.50f + 0.50f * std::sqrt(std::max(0.0f, 1.0f - rn * rn));
        return Pix{v * a, v * a, v * a, a};
    });
    return c;
}

// ---- icons (white masks) ---------------------------------------------------------------
template <typename Sdf> Raster icon(Sdf sdf) {
    Raster c(96, 96);
    for (int y = 0; y < 96; ++y)
        for (int x = 0; x < 96; ++x) {
            const float a = cover(sdf(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f));
            c.at(x, y) = {a, a, a, a};
        }
    return c;
}
Raster iconTriangle(bool left) {
    return icon([left](float x, float y) {
        if (!left) x = 96.0f - x;
        const float tri[6] = {26.0f, 48.0f, 66.0f, 24.0f, 66.0f, 72.0f};
        return std::min(sdTriangle(x, y, tri) - 1.5f, std::max(std::fabs(x - 20.0f) - 3.0f, std::fabs(y - 48.0f) - 22.0f));
    });
}
Raster iconSine() {
    return icon([](float x, float y) {
        float d = 1e9f;
        float px = 12.0f, py = 48.0f;
        for (int i = 1; i <= 96; ++i) {
            const float nx = 12.0f + 72.0f * static_cast<float>(i) / 96.0f;
            const float ny = 48.0f - 20.0f * std::sin(2.0f * kPi * (nx - 12.0f) / 72.0f);
            d = std::min(d, sdSegment(x, y, px, py, nx, ny));
            px = nx; py = ny;
        }
        return d - 3.6f;
    });
}
Raster iconInput() {
    return icon([](float x, float y) {
        const float shaft = sdSegment(x, y, 12.0f, 48.0f, 50.0f, 48.0f) - 3.6f;
        const float head[6] = {46.0f, 30.0f, 66.0f, 48.0f, 46.0f, 66.0f};
        const float tri = sdTriangle(x, y, head) - 1.0f;
        const float bar = std::min(sdSegment(x, y, 78.0f, 22.0f, 78.0f, 74.0f), std::min(sdSegment(x, y, 64.0f, 22.0f, 78.0f, 22.0f),
                                                                                         sdSegment(x, y, 64.0f, 74.0f, 78.0f, 74.0f))) - 3.6f;
        return std::min(std::min(shaft, tri), bar);
    });
}

Raster iconFolder() {
    return icon([](float x, float y) {
        const float body = sdRoundRect(x - 48.0f, y - 54.0f, 34.0f, 22.0f, 5.0f);
        const float tab  = sdRoundRect(x - 30.0f, y - 30.0f, 16.0f, 7.0f, 4.0f);
        const float outline = std::fabs(std::min(body, tab)) - 3.4f;
        return outline;
    });
}
Raster iconWave() {   // stem waveform: bars of varying height
    return icon([](float x, float y) {
        static const float h[] = {10, 22, 34, 18, 28, 40, 24, 14, 30, 20, 8};
        float d = 1e9f;
        for (int i = 0; i < 11; ++i) {
            const float cx = 18.0f + 6.0f * static_cast<float>(i);
            d = std::min(d, sdSegment(x, y, cx, 48.0f - h[i] * 0.5f, cx, 48.0f + h[i] * 0.5f) - 2.2f);
        }
        return d;
    });
}

Raster iconPlay() {
    return icon([](float x, float y) {
        const float tri[6] = {34.0f, 22.0f, 74.0f, 48.0f, 34.0f, 74.0f};
        return sdTriangle(x, y, tri) - 2.0f;
    });
}
Raster iconPause() {
    return icon([](float x, float y) {
        return std::min(sdRoundRect(x - 36.0f, y - 48.0f, 7.0f, 25.0f, 2.5f), sdRoundRect(x - 60.0f, y - 48.0f, 7.0f, 25.0f, 2.5f));
    });
}
Raster iconStop() {
    return icon([](float x, float y) { return sdRoundRect(x - 48.0f, y - 48.0f, 22.0f, 22.0f, 4.0f); });
}
Raster iconLoop() {   // two arrows chasing round a rounded rectangle
    return icon([](float x, float y) {
        const float ring = std::fabs(sdRoundRect(x - 48.0f, y - 48.0f, 30.0f, 18.0f, 14.0f)) - 3.6f;
        const float gapA = sdRoundRect(x - 34.0f, y - 30.0f, 7.0f, 7.0f, 0.0f);    // break in the top run
        const float gapB = sdRoundRect(x - 62.0f, y - 66.0f, 7.0f, 7.0f, 0.0f);    // break in the bottom run
        const float body = std::max(ring, -std::min(gapA, gapB));
        const float headA[6] = {42.0f, 19.0f, 54.0f, 30.0f, 42.0f, 41.0f};
        const float headB[6] = {54.0f, 55.0f, 42.0f, 66.0f, 54.0f, 77.0f};
        return std::min(body, std::min(sdTriangle(x, y, headA), sdTriangle(x, y, headB)) - 0.5f);
    });
}

// ---- packing ----------------------------------------------------------------------------
struct Item {
    Sprite      id;
    Raster      canvas;
    SpriteInfo  info;
    int         x = 0, y = 0;
};

} // namespace

PartsAtlas buildPartsAtlas(const Image& fp) {
    std::vector<Item> items;
    auto add = [&](Sprite id, Raster c, float slice = 0.0f) {
        Item it{id, std::move(c), {}};
        for (float& s : it.info.slice) s = slice;
        items.push_back(std::move(it));
    };
    const CapShape fader{124.0f, 62.0f, 22.0f, 16.0f, 0.0f, true};
    const CapShape button{58.0f, 42.0f, 17.0f, 13.0f, 0.06f, false};

    add(Sprite::Socket, cutSocket(fp));
    add(Sprite::KnobShadow, knobShadow());
    add(Sprite::KnobTrack, knobTrack());
    add(Sprite::KnobBody, knobBody());
    add(Sprite::KnobIndicator, knobIndicator());
    add(Sprite::KnobLight, knobLight());
    add(Sprite::CapDark, capDark());
    add(Sprite::RingGlow, ringGlow());
    add(Sprite::FaderCap, chromeCap(256, 136, fader, 5));
    add(Sprite::FaderShadow, softRoundRect(128, 80, 50.0f, 25.0f, 12.0f, 10.0f, 0.75f));
    add(Sprite::FaderLine, faderLine());
    add(Sprite::ButtonCap, chromeCap(128, 96, button, 9));
    add(Sprite::ButtonShadow, softRoundRect(96, 72, 34.0f, 22.0f, 10.0f, 8.0f, 0.70f));
    add(Sprite::ButtonLens, buttonLens());
    add(Sprite::ButtonHalo, buttonHalo());
    add(Sprite::Glass, cutGlass(fp), static_cast<float>(kGlassCorner));
    add(Sprite::GlassSheen, glassSheen());
    add(Sprite::GlassRim, glassRim(), 16.0f);
    add(Sprite::InnerShadow, innerShadow(), 20.0f);
    add(Sprite::DropShadow, dropShadow(), 40.0f);
    add(Sprite::LedSegment, ledSegment());
    add(Sprite::GlowDot, glowDot());
    add(Sprite::Lamp, lamp());
    add(Sprite::IconPrev, iconTriangle(true));
    add(Sprite::IconNext, iconTriangle(false));
    add(Sprite::IconSine, iconSine());
    add(Sprite::IconInput, iconInput());
    add(Sprite::IconFolder, iconFolder());
    add(Sprite::IconWave, iconWave());
    add(Sprite::IconPlay, iconPlay());
    add(Sprite::IconPause, iconPause());
    add(Sprite::IconStop, iconStop());
    add(Sprite::IconLoop, iconLoop());

    // Shelf packing, tallest first, 4 px transparent gutters.
    constexpr int kAtlasW = 1024, kPad = 4;
    std::vector<Item*> order;
    for (auto& it : items) order.push_back(&it);
    std::stable_sort(order.begin(), order.end(), [](const Item* a, const Item* b) { return a->canvas.h > b->canvas.h; });
    int x = kPad, y = kPad, shelf = 0;
    for (Item* it : order) {
        if (x + it->canvas.w + kPad > kAtlasW) { x = kPad; y += shelf + kPad; shelf = 0; }
        it->x = x;
        it->y = y;
        x += it->canvas.w + kPad;
        shelf = std::max(shelf, it->canvas.h);
    }
    int atlasH = 64;
    while (atlasH < y + shelf + kPad) atlasH *= 2;

    Image img;
    img.width  = kAtlasW;
    img.height = static_cast<std::uint32_t>(atlasH);
    img.rgba.assign(static_cast<std::size_t>(kAtlasW) * atlasH * 4, 0);
    PartsAtlas atlas;
    for (Item& it : items) {
        for (int yy = 0; yy < it.canvas.h; ++yy)
            for (int xx = 0; xx < it.canvas.w; ++xx) {
                const Pix& p = it.canvas.at(xx, yy);
                std::uint8_t* o = &img.rgba[(static_cast<std::size_t>(it.y + yy) * kAtlasW + (it.x + xx)) * 4];
                o[0] = linearToSrgb8(p.r);
                o[1] = linearToSrgb8(p.g);
                o[2] = linearToSrgb8(p.b);
                o[3] = static_cast<std::uint8_t>(clamp01(p.a) * 255.0f + 0.5f);
            }
        SpriteInfo info = it.info;
        info.width  = static_cast<float>(it.canvas.w);
        info.height = static_cast<float>(it.canvas.h);
        info.u0 = static_cast<float>(it.x) / kAtlasW;
        info.v0 = static_cast<float>(it.y) / static_cast<float>(atlasH);
        info.u1 = static_cast<float>(it.x + it.canvas.w) / kAtlasW;
        info.v1 = static_cast<float>(it.y + it.canvas.h) / static_cast<float>(atlasH);
        atlas.sprites[static_cast<std::size_t>(it.id)] = info;
    }
    atlas.texture = buildMips(std::move(img));
    return atlas;
}

} // namespace hc::ui

#include "hybrid/ui/ScopeView.hpp"

#include "hybrid/analysis/Analyzer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hc::ui {

namespace {
using analysis::Analyzer;

constexpr int   kRows   = Analyzer::kRows;
constexpr int   kPts    = Analyzer::kPoints;
constexpr float kXW     = 1.6f;    // half width of the landscape (frequency)
constexpr float kZD     = 2.4f;    // depth (time)
constexpr float kH      = 0.85f;   // height at 0 dB
constexpr float kDbLow  = -84.0f;  // floor of the landscape
constexpr float kCamD   = 6.0f;    // camera distance (perspective strength)

constexpr Rgba kBgTop    = rgb8(0x0B1222);
constexpr Rgba kBgBottom = rgb8(0x03050A);
constexpr Rgba kGrid     = rgb8(0x3FD0FF);
constexpr Rgba kText     = rgb8(0xE8EEF6);
constexpr Rgba kDim      = rgb8(0x8C9AAE);
constexpr Rgba kCyan     = rgb8(0x56C8FF);
constexpr Rgba kGood     = rgb8(0x3FE07A);
constexpr Rgba kNote     = rgb8(0x56B6FF);
constexpr Rgba kWarn     = rgb8(0xFFC21A);
constexpr Rgba kBad      = rgb8(0xFF4545);

// Magma-like heat: deep violet -> magenta -> orange -> gold -> white.
Rgba heat(float t) {
    static constexpr std::array<std::uint32_t, 7> stops{0x0D0520, 0x3B0F70, 0x8C2981, 0xDE4968, 0xFE9F6D, 0xFDE68A, 0xFFFFFF};
    t = std::clamp(t, 0.0f, 1.0f) * static_cast<float>(stops.size() - 1);
    const int i = std::min(static_cast<int>(t), static_cast<int>(stops.size()) - 2);
    const float f = t - static_cast<float>(i);
    const Rgba a = rgb8(stops[static_cast<std::size_t>(i)]), b = rgb8(stops[static_cast<std::size_t>(i + 1)]);
    return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, 1.0f};
}
Rgba mix(Rgba a, Rgba b, float f) { return {a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f, a.a + (b.a - a.a) * f}; }
Rgba scale(Rgba c, float k) { return {std::min(1.0f, c.r * k), std::min(1.0f, c.g * k), std::min(1.0f, c.b * k), c.a}; }

std::string fmtNum(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string fitText(const Renderer& r, TextStyle st, std::string s, float maxW) {
    if (r.textWidth(st, s) <= maxW) return s;
    while (!s.empty() && r.textWidth(st, s + "...") > maxW) s.pop_back();
    return s + "...";
}

std::vector<std::string> wrap(const Renderer& r, TextStyle st, const std::string& s, float maxW) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && r.textWidth(st, trial) > maxW) { lines.push_back(line); line = word; }
        else line = trial;
        word.clear();
    };
    for (char ch : s) {
        if (ch == ' ') flush();
        else word += ch;
    }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// Projection of the landscape into a view rectangle (design units), fitted to the box.
struct Projector {
    float cy_, sy_, cp_, sp_;
    float scale = 1.0f, ox = 0.0f, oy = 0.0f;

    Projector(const ScopeCamera& cam, Rect view) {
        const float yaw = std::clamp(cam.yaw, -0.9f, 0.9f), pitch = std::clamp(cam.pitch, 0.15f, 1.1f);
        cy_ = std::cos(yaw); sy_ = std::sin(yaw); cp_ = std::cos(pitch); sp_ = std::sin(pitch);
        // Fit the whole box (plus room for the labels) into the view.
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (float x : {-kXW - 0.25f, kXW + 0.25f})
            for (float y : {0.0f, kH})
                for (float z : {-0.6f, kZD}) {
                    float u, v, d;
                    raw(x, y, z, u, v, d);
                    x0 = std::min(x0, u); x1 = std::max(x1, u); y0 = std::min(y0, v); y1 = std::max(y1, v);
                }
        scale = std::min(view.w / (x1 - x0), view.h / (y1 - y0)) * 0.94f;
        ox = view.cx() - scale * 0.5f * (x0 + x1);
        oy = view.cy() - scale * 0.5f * (y0 + y1);
    }
    void raw(float x, float y, float z, float& u, float& v, float& depth) const {
        const float rx0 = x, ry = y - kH * 0.3f, rz0 = z - kZD * 0.5f;
        const float rx = rx0 * cy_ + rz0 * sy_;
        const float rz = -rx0 * sy_ + rz0 * cy_;
        const float up = ry * cp_ + rz * sp_;
        depth = rz * cp_ - ry * sp_ + kCamD;
        u = rx / depth * kCamD;
        v = -up / depth * kCamD;
    }
    void operator()(float x, float y, float z, float& sx, float& sy) const {
        float u, v, d;
        raw(x, y, z, u, v, d);
        sx = ox + u * scale;
        sy = oy + v * scale;
    }
    [[nodiscard]] float depth(float x, float y, float z) const {
        float u, v, d;
        raw(x, y, z, u, v, d);
        return d;
    }
};

float pointX(int i) { return -kXW + 2.0f * kXW * static_cast<float>(i) / static_cast<float>(kPts - 1); }
float freqX(float hz) { return -kXW + 2.0f * kXW * std::log(hz / 20.0f) / std::log(1000.0f); }
float rowZ(int age) { return kZD * static_cast<float>(age) / static_cast<float>(kRows - 1); }
float dbT(float db) { return std::clamp((db - kDbLow) / -kDbLow, 0.0f, 1.0f); }
float tHeight(float t) { return kH * std::pow(t, 1.15f); }

void line3(Renderer& r, const Projector& P, float x0, float y0, float z0, float x1, float y1, float z1, float w, Rgba c, bool emissive = false) {
    float xs[2], ys[2];
    P(x0, y0, z0, xs[0], ys[0]);
    P(x1, y1, z1, xs[1], ys[1]);
    r.polyline(xs, ys, 2, w, c, Blend::Over, emissive);
}

void card(Renderer& r, Rect c) {
    r.fillRound(c, 18, rgb8(0xFFFFFF, 0.035f), Blend::Over);
    r.fillRound({c.x, c.y, c.w, 2}, 1, rgb8(0x56C8FF, 0.25f), Blend::Over);   // thin lit edge on top
}

// Horizontal meter: value v in [lo, hi], coloured zones, a marker.
void hbar(Renderer& r, Rect b, float lo, float hi, float v, float marker, const std::array<std::pair<float, Rgba>, 5>& zones) {
    r.fillRound(b, b.h * 0.5f, rgb8(0x000000, 0.55f), Blend::Over);
    const auto xOf = [&](float x) { return b.x + b.w * std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f); };
    float from = lo;
    for (const auto& [to, col] : zones) {
        const float a = xOf(from), e = xOf(std::min(to, v));
        if (e > a) r.fillRound({a, b.y + 3, e - a, b.h - 6}, 3, col, Blend::Over, true);
        from = to;
        if (to >= v) break;
    }
    if (marker > lo) {
        const float mx = xOf(marker);
        r.fillRound({mx - 2, b.y - 8, 4, b.h + 16}, 2, rgb8(0xFFFFFF, 0.9f), Blend::Over);
    }
}
} // namespace

Rect scopeViewArea(Rect a) noexcept {
    const float split = a.x + a.w * 0.655f;
    return {a.x + 20, a.y + 100, split - a.x - 30, a.h - 120};
}

void drawScope(Renderer& r, Rect a, const ViewState& v) {
    // ---- the panel: deep blue-black glass ----
    {
        const float xs[4] = {a.x, a.right(), a.right(), a.x}, ys[4] = {a.y, a.y, a.bottom(), a.bottom()};
        const Rgba cs[4] = {kBgTop, kBgTop, kBgBottom, kBgBottom};
        r.fillRound(a.inset(-4), 24, rgb8(0x000000), Blend::Over);
        r.colorQuad(xs, ys, cs);
        r.fillRound({a.x + 24, a.y + 2, a.w - 48, 2}, 1, kCyan.withAlpha(0.35f), Blend::Over);
    }
    const Analyzer* an = v.analyzer;

    // ---- header ----
    r.text(TextStyle::Heading, "ANALYZER", a.x + 34, a.y + 74, kCyan, Align::Left, TextFx::Neon);
    const float hx = a.x + 34 + r.textWidth(TextStyle::Heading, "ANALYZER") + 30;
    r.text(TextStyle::Sub, "3D spectrum  \xC2\xB7  loudness  \xC2\xB7  plain-words checks", hx, a.y + 70, kDim, Align::Left, TextFx::Shadowed);
    const std::string src = v.scopeSource.empty() ? std::string("Listening to: MAIN OUT") : "Playing: " + v.scopeSource;
    r.text(TextStyle::Label, fitText(r, TextStyle::Label, src, a.w * 0.34f), a.right() - 34, a.y + 66, kText, Align::Right, TextFx::Shadowed);

    const Rect view = scopeViewArea(a);
    const Projector P(v.camera, {view.x, view.y + 10, view.w, view.h - 60});

    // ---- floor, grid, back wall ----
    {
        float fx[4], fy[4];
        P(-kXW, 0, 0, fx[0], fy[0]);
        P(kXW, 0, 0, fx[1], fy[1]);
        P(kXW, 0, kZD, fx[2], fy[2]);
        P(-kXW, 0, kZD, fx[3], fy[3]);
        const Rgba fc[4] = {rgb8(0x0E1A2E, 0.95f), rgb8(0x0E1A2E, 0.95f), rgb8(0x060A12, 0.95f), rgb8(0x060A12, 0.95f)};
        r.colorQuad(fx, fy, fc);
        float wx[4], wy[4];
        P(-kXW, 0, kZD, wx[0], wy[0]);
        P(kXW, 0, kZD, wx[1], wy[1]);
        P(kXW, kH, kZD, wx[2], wy[2]);
        P(-kXW, kH, kZD, wx[3], wy[3]);
        const Rgba wc[4] = {rgb8(0x08101E, 0.9f), rgb8(0x08101E, 0.9f), rgb8(0x050812, 0.6f), rgb8(0x050812, 0.6f)};
        r.colorQuad(wx, wy, wc);
        for (float hz : {50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f}) {
            const float x = freqX(hz);
            line3(r, P, x, 0, 0, x, 0, kZD, 1.6f, kGrid.withAlpha(0.16f));
            line3(r, P, x, 0, kZD, x, kH, kZD, 1.4f, kGrid.withAlpha(0.10f));
        }
        for (int k = 0; k <= 4; ++k) {   // time lines every ~0.68 s
            const float z = kZD * static_cast<float>(k) / 4.0f;
            line3(r, P, -kXW, 0, z, kXW, 0, z, 1.6f, kGrid.withAlpha(k == 0 ? 0.35f : 0.14f));
        }
        for (float db : {-72.0f, -48.0f, -24.0f, 0.0f}) {
            const float y = tHeight(dbT(db));
            line3(r, P, -kXW, y, kZD, kXW, y, kZD, 1.4f, kGrid.withAlpha(db == 0.0f ? 0.3f : 0.12f));
            float lx, ly;
            P(-kXW - 0.04f, y, kZD, lx, ly);
            r.text(TextStyle::Caption, fmtNum("%.0f dB", db), lx - 8, ly + 9, kDim, Align::Right, TextFx::Shadowed);
        }
    }

    // ---- the landscape ----
    static thread_local std::vector<float> H;      // heights
    static thread_local std::vector<float> T;      // 0..1 levels
    H.assign(static_cast<std::size_t>(kRows) * kPts, 0.0f);
    T.assign(static_cast<std::size_t>(kRows) * kPts, 0.0f);
    if (an) {
        for (int age = 0; age < kRows; ++age) {
            const float* row = an->row(age);
            for (int i = 0; i < kPts; ++i) {
                const float t = dbT(row[i]);
                T[static_cast<std::size_t>(age * kPts + i)] = t;
                H[static_cast<std::size_t>(age * kPts + i)] = tHeight(t);
            }
        }
    }
    const auto h = [&](int age, int i) {
        age = std::clamp(age, 0, kRows - 1);
        i = std::clamp(i, 0, kPts - 1);
        return H[static_cast<std::size_t>(age * kPts + i)];
    };
    const float dx = 2.0f * kXW / static_cast<float>(kPts - 1), dz = kZD / static_cast<float>(kRows - 1);
    // Light from the upper left, toward the viewer.
    const float lx = -0.45f, ly = 0.80f, lz = -0.40f, ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    const Rgba fog = rgb8(0x060A14);
    const auto vertexColour = [&](int age, int i) {
        const float t = T[static_cast<std::size_t>(age * kPts + i)];
        const float gx = (h(age, i + 1) - h(age, i - 1)) / (2.0f * dx), gz = (h(age + 1, i) - h(age - 1, i)) / (2.0f * dz);
        const float nl = std::sqrt(gx * gx + 1.0f + gz * gz);
        const float diff = std::max(0.0f, (-gx * lx + ly - gz * lz) / (nl * ll));
        Rgba c = scale(heat(0.08f + 0.92f * t), 0.45f + 0.75f * diff);
        c = mix(c, fog, 0.70f * static_cast<float>(age) / static_cast<float>(kRows - 1));
        c.a = 0.97f;
        return c;
    };
    const bool leftFar = P.depth(-kXW, 0, kZD * 0.5f) > P.depth(kXW, 0, kZD * 0.5f);
    for (int age = kRows - 2; age >= 0; --age) {   // back to front
        const float z0 = rowZ(age + 1), z1 = rowZ(age);
        for (int k = 0; k < kPts - 1; ++k) {
            const int i = leftFar ? k : kPts - 2 - k;
            if (T[static_cast<std::size_t>((age + 1) * kPts + i)] + T[static_cast<std::size_t>((age + 1) * kPts + i + 1)] +
                    T[static_cast<std::size_t>(age * kPts + i)] + T[static_cast<std::size_t>(age * kPts + i + 1)] <= 0.0f)
                continue;   // flat on the floor: the floor shows through
            float xs[4], ys[4];
            P(pointX(i), h(age + 1, i), z0, xs[0], ys[0]);
            P(pointX(i + 1), h(age + 1, i + 1), z0, xs[1], ys[1]);
            P(pointX(i + 1), h(age, i + 1), z1, xs[2], ys[2]);
            P(pointX(i), h(age, i), z1, xs[3], ys[3]);
            const Rgba cs[4] = {vertexColour(age + 1, i), vertexColour(age + 1, i + 1), vertexColour(age, i + 1), vertexColour(age, i)};
            r.colorQuad(xs, ys, cs);
        }
        // A fine contour every 6th row (tied to the audio, so it scrolls with it).
        if (an && ((an->rowsMade() + static_cast<std::uint64_t>(kRows) * 1000 - static_cast<std::uint64_t>(age)) % 6) == 0) {
            float xs[kPts], ys[kPts];
            for (int i = 0; i < kPts; ++i) P(pointX(i), h(age, i) + 0.004f, z1, xs[i], ys[i]);
            r.polyline(xs, ys, kPts, 1.5f, heat(0.85f).withAlpha(0.22f * (1.0f - static_cast<float>(age) / kRows)));
        }
    }
    // Front face: the newest slice, solid down to the floor, with a glowing ridge.
    {
        for (int i = 0; i < kPts - 1; ++i) {
            float xs[4], ys[4];
            P(pointX(i), h(0, i), 0, xs[0], ys[0]);
            P(pointX(i + 1), h(0, i + 1), 0, xs[1], ys[1]);
            P(pointX(i + 1), 0, 0, xs[2], ys[2]);
            P(pointX(i), 0, 0, xs[3], ys[3]);
            const Rgba top0 = scale(heat(0.1f + 0.9f * T[static_cast<std::size_t>(i)]), 0.55f).withAlpha(0.95f);
            const Rgba top1 = scale(heat(0.1f + 0.9f * T[static_cast<std::size_t>(i + 1)]), 0.55f).withAlpha(0.95f);
            const Rgba cs[4] = {top0, top1, rgb8(0x05070D, 0.95f), rgb8(0x05070D, 0.95f)};
            r.colorQuad(xs, ys, cs);
        }
        float xs[kPts], ys[kPts];
        for (int i = 0; i < kPts; ++i) P(pointX(i), h(0, i), 0, xs[i], ys[i]);
        r.polyline(xs, ys, kPts, 5.0f, rgb8(0xFFB86B, 0.35f), Blend::Add, true);
        r.polyline(xs, ys, kPts, 2.4f, rgb8(0xFFF1C8, 0.95f), Blend::Over, true);
    }

    // ---- axis labels ----
    {
        const std::array<std::pair<float, const char*>, 8> marks{{{50, "50"}, {100, "100"}, {200, "200"}, {500, "500"}, {1000, "1k"},
                                                                  {2000, "2k"}, {5000, "5k"}, {10000, "10k"}}};
        for (const auto& [hz, name] : marks) {
            float sx, sy;
            P(freqX(hz), 0, -0.12f, sx, sy);
            r.text(TextStyle::Caption, name, sx, sy + 22, kDim, Align::Center, TextFx::Shadowed);
        }
        float sx, sy;
        P(-kXW, 0, -0.55f, sx, sy);
        r.text(TextStyle::Caption, "< BASS", sx, sy + 26, kCyan.withAlpha(0.8f), Align::Left, TextFx::Shadowed);
        P(kXW, 0, -0.55f, sx, sy);
        r.text(TextStyle::Caption, "HIGHS >   (Hz)", sx, sy + 26, kCyan.withAlpha(0.8f), Align::Right, TextFx::Shadowed);
        P(kXW + 0.08f, 0, 0, sx, sy);
        r.text(TextStyle::Caption, "NOW", sx, sy + 8, rgb8(0xFFE2A8), Align::Left, TextFx::Shadowed);
        P(kXW + 0.08f, 0, kZD, sx, sy);
        r.text(TextStyle::Caption, fmtNum("%.1f s ago", static_cast<double>(kRows) * 2.0 * Analyzer::kHop / 48000.0), sx, sy + 8, kDim, Align::Left,
               TextFx::Shadowed);
        r.text(TextStyle::Caption, "DRAG TO TURN  /  DOUBLE-CLICK TO RESET", view.x + 10, view.bottom() - 8, kDim.withAlpha(0.7f), Align::Left,
               TextFx::Shadowed);
    }

    // ---- right column: numbers and checks ----
    const float cx0 = a.x + a.w * 0.665f, cw = a.right() - 28 - cx0;
    float y = a.y + 104;
    const auto lufsText = [](double l) { return l <= Analyzer::kSilentLufs ? std::string("--.-") : fmtNum("%.1f", l); };

    {   // LOUDNESS
        const Rect c{cx0, y, cw, 250};
        card(r, c);
        r.text(TextStyle::DisplayCap, "LOUDNESS - LAST 3 S", c.x + 26, c.y + 44, kCyan, Align::Left, TextFx::Neon);
        const double st = an ? an->shortTermLufs() : Analyzer::kSilentLufs;
        const std::string big = lufsText(st);
        r.text(TextStyle::Display, big, c.x + 26, c.y + 140, kText, Align::Left, TextFx::Neon);
        r.text(TextStyle::Title, "LUFS", c.x + 40 + r.textWidth(TextStyle::Display, big), c.y + 138, kDim, Align::Left, TextFx::Shadowed);
        const double whole = an ? an->integratedLufs() : Analyzer::kSilentLufs;
        r.text(TextStyle::Sub, "Whole play: " + lufsText(whole) + " LUFS   \xC2\xB7   finished songs: about -14", c.x + 26, c.y + 184, kDim, Align::Left,
               TextFx::Shadowed);
        const std::array<std::pair<float, Rgba>, 5> zones{{{-20, rgb8(0x2F7DFF)}, {-16, rgb8(0x3FD0FF)}, {-12, kGood}, {-9, kWarn}, {0, rgb8(0xFF7A1A)}}};
        hbar(r, {c.x + 26, c.y + 206, c.w - 52, 22}, -36, -6, static_cast<float>(st), static_cast<float>(whole), zones);
        float tx = c.x + 26 + (c.w - 52) * (-14.0f + 36.0f) / 30.0f;
        r.text(TextStyle::Caption, "-14", tx, c.y + 248, kGood.withAlpha(0.9f), Align::Center, TextFx::Shadowed);
        y = c.bottom() + 16;
    }
    {   // PEAK
        const Rect c{cx0, y, cw, 196};
        card(r, c);
        r.text(TextStyle::DisplayCap, "PEAK", c.x + 26, c.y + 44, kCyan, Align::Left, TextFx::Neon);
        const float hold = an ? an->peakHoldDb() : -120.0f, most = an ? an->maxPeakDb() : -120.0f;
        const bool hot = most > -1.0f || (an && an->clippedSamples() > 0);
        const std::string big = hold > -100.0f ? fmtNum("%.1f", hold) : "--.-";
        r.text(TextStyle::Display, big, c.x + 26, c.y + 132, hot ? kBad : kText, Align::Left, TextFx::Neon);
        r.text(TextStyle::Title, "dB", c.x + 40 + r.textWidth(TextStyle::Display, big), c.y + 130, kDim, Align::Left, TextFx::Shadowed);
        const std::string sub = "Highest: " + (most > -100.0f ? fmtNum("%.1f dB", most) : std::string("--")) +
                                "   \xC2\xB7   clipped: " + std::to_string(an ? an->clippedSamples() : 0);
        r.text(TextStyle::Sub, sub, c.x + c.w - 26, c.y + 44, hot ? kBad : kDim, Align::Right, TextFx::Shadowed);
        const std::array<std::pair<float, Rgba>, 5> zones{{{-18, kGood}, {-6, rgb8(0x9BE33C)}, {-3, kWarn}, {-1, rgb8(0xFF7A1A)}, {0.5f, kBad}}};
        hbar(r, {c.x + 26, c.y + 156, c.w - 52, 20}, -48, 0, an ? an->peakDb() : -120.0f, hold, zones);
        y = c.bottom() + 16;
    }
    {   // TONE + STEREO
        const Rect c{cx0, y, cw, 214};
        card(r, c);
        r.text(TextStyle::DisplayCap, "TONE", c.x + 26, c.y + 44, kCyan, Align::Left, TextFx::Neon);
        const float b = an ? an->bassDb() : -140.0f, m = an ? an->midsDb() : -140.0f, hi = an ? an->highsDb() : -140.0f;
        const float top = std::max({b, m, hi});
        const std::array<std::pair<const char*, float>, 3> bands{{{"BASS", b}, {"MIDS", m}, {"HIGHS", hi}}};
        const float bw = (c.w * 0.56f - 26) / 3.0f;
        for (int k = 0; k < 3; ++k) {
            const float bx = c.x + 26 + bw * static_cast<float>(k);
            const Rect slot{bx + 10, c.y + 62, bw - 36, 100};
            r.fillRound(slot, 8, rgb8(0x000000, 0.5f), Blend::Over);
            const float lvl = top > -130.0f ? std::clamp((bands[static_cast<std::size_t>(k)].second - top + 30.0f) / 30.0f, 0.0f, 1.0f) : 0.0f;
            if (lvl > 0.01f)
                r.fillRound({slot.x + 4, slot.bottom() - 4 - (slot.h - 8) * lvl, slot.w - 8, (slot.h - 8) * lvl}, 5, heat(0.35f + 0.6f * lvl),
                            Blend::Over, true);
            r.text(TextStyle::Caption, bands[static_cast<std::size_t>(k)].first, slot.cx(), slot.bottom() + 34, kDim, Align::Center, TextFx::Shadowed);
        }
        // Stereo: correlation -1 .. +1.
        const float sx0 = c.x + c.w * 0.6f, sw = c.right() - 26 - sx0;
        r.text(TextStyle::DisplayCap, "STEREO", sx0, c.y + 44, kCyan, Align::Left, TextFx::Neon);
        const float corr = an ? an->correlation() : 0.0f;
        const Rect sb{sx0, c.y + 96, sw, 22};
        r.fillRound(sb, 11, rgb8(0x000000, 0.55f), Blend::Over);
        r.fillRound({sb.x + 3, sb.y + 3, sw * 0.5f - 3, sb.h - 6}, 3, kBad.withAlpha(0.25f), Blend::Over);
        r.fillRound({sb.cx(), sb.y + 3, sw * 0.5f - 3, sb.h - 6}, 3, kGood.withAlpha(0.25f), Blend::Over);
        const float mx = sb.x + sw * 0.5f * (corr + 1.0f);
        r.fillRound({mx - 5, sb.y - 8, 10, sb.h + 16}, 4, corr < 0.0f ? kBad : kGood, Blend::Over, true);
        r.text(TextStyle::Caption, "-1", sb.x, sb.bottom() + 30, kDim, Align::Left, TextFx::Shadowed);
        r.text(TextStyle::Caption, "+1", sb.right(), sb.bottom() + 30, kDim, Align::Right, TextFx::Shadowed);
        r.text(TextStyle::Label, fmtNum("%+.2f", corr), sb.cx(), sb.bottom() + 74, kText, Align::Center, TextFx::Shadowed);
        y = c.bottom() + 16;
    }
    {   // CHECKS
        const Rect c{cx0, y, cw, a.bottom() - 24 - y};
        card(r, c);
        r.text(TextStyle::DisplayCap, "CHECKS - WHOLE PLAY SO FAR", c.x + 26, c.y + 44, kCyan, Align::Left, TextFx::Neon);
        std::vector<Analyzer::Check> checks;
        if (an) checks = an->checks();
        else checks.push_back({Analyzer::Mark::Note, "No audio stream: pick a device with the DEVICE panel."});
        // Every check must fit: normal text when there is room, smaller text when the window is short.
        TextStyle st = TextStyle::Label;
        float lineH = 40.0f, gap = 8.0f, dotY = 22.0f;
        const auto heightFor = [&](TextStyle s, float lh, float g) {
            float h = 0.0f;
            for (const auto& ck : checks) h += lh * static_cast<float>(wrap(r, s, ck.text, c.w - 100).size()) + g;
            return h;
        };
        const float room = c.bottom() - 8 - (c.y + 92) + lineH;
        if (heightFor(st, lineH, gap) > room) { st = TextStyle::Caption; lineH = 32.0f; gap = 5.0f; dotY = 19.0f; }
        float ly2 = c.y + 92;
        for (const auto& ck : checks) {
            const Rgba dot = ck.mark == Analyzer::Mark::Good ? kGood : ck.mark == Analyzer::Mark::Warn ? kWarn : ck.mark == Analyzer::Mark::Bad ? kBad : kNote;
            const auto lines = wrap(r, st, ck.text, c.w - 100);
            if (ly2 + lineH * static_cast<float>(lines.size() - 1) > c.bottom() - 8) break;
            r.fillRound({c.x + 30, ly2 - dotY, 18, 18}, 9, dot, Blend::Over, true);
            for (const auto& l : lines) {
                r.text(st, l, c.x + 66, ly2, kText, Align::Left, TextFx::Shadowed);
                ly2 += lineH;
            }
            ly2 += gap;
        }
    }
}

} // namespace hc::ui

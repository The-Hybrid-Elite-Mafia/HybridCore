#include "HemUi.hpp"

#include <cstdint>
#include <cstdio>
#include <map>

#pragma comment(lib, "gdiplus.lib")

namespace hem::ui {
using namespace Gdiplus;

namespace {
ULONG_PTR g_token = 0;

void roundedPath(GraphicsPath& p, RectF r, float rad) {
    const float d = rad * 2.0f;
    p.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}
void fillRounded(Graphics& g, Brush& b, RectF r, float rad) { GraphicsPath p; roundedPath(p, r, rad); g.FillPath(&b, &p); }
void strokeRounded(Graphics& g, Pen& pen, RectF r, float rad) { GraphicsPath p; roundedPath(p, r, rad); g.DrawPath(&pen, &p); }
RectF inset(RectF r, float d) { return RectF(r.X + d, r.Y + d, r.Width - 2 * d, r.Height - 2 * d); }

// Fonts are looked up once. Segoe UI and Consolas ship with Windows; if one is missing the generic face is used.
std::map<std::uint64_t, std::unique_ptr<Font>>& fontCache() { static std::map<std::uint64_t, std::unique_ptr<Font>> c; return c; }
Font* fontFor(Face f, float px) {
    auto& cache = fontCache();
    const std::uint64_t key = (static_cast<std::uint64_t>(f) << 32) | static_cast<std::uint32_t>(px * 10.0f);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.get();
    const bool mono = f == Face::Mono || f == Face::MonoBold;
    const int style = (f == Face::MonoBold || f == Face::Legend || f == Face::Title) ? FontStyleBold : FontStyleRegular;
    std::unique_ptr<Font> font;
    FontFamily fam(mono ? L"Consolas" : L"Segoe UI");
    if (fam.GetLastStatus() == Ok) font = std::make_unique<Font>(&fam, px, style, UnitPixel);
    else font = std::make_unique<Font>(mono ? FontFamily::GenericMonospace() : FontFamily::GenericSansSerif(), px, style, UnitPixel);
    Font* raw = font.get();
    cache[key] = std::move(font);
    return raw;
}
} // namespace

bool startup() { GdiplusStartupInput in; return GdiplusStartup(&g_token, &in, nullptr) == Ok; }
void shutdown() { fontCache().clear(); if (g_token) GdiplusShutdown(g_token); g_token = 0; }   // GDI+ objects must be gone before GDI+ is shut down

std::unique_ptr<Bitmap> makePlate(int w, int h, unsigned seed) {
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppARGB);
    BitmapData bd{};
    Rect all(0, 0, w, h);
    if (bmp->LockBits(&all, ImageLockModeWrite, PixelFormat32bppARGB, &bd) != Ok) return bmp;
    unsigned s = seed * 2654435761u + 12345u;
    auto rnd = [&]() { s = s * 1664525u + 1013904223u; return static_cast<float>((s >> 8) & 0xFFFF) / 65535.0f - 0.5f; };
    std::vector<float> n(static_cast<std::size_t>(w)), sm(static_cast<std::size_t>(w));
    const int R = 26;   // brush streak length (pixels, each side)
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) n[static_cast<std::size_t>(x)] = rnd();
        // two box blurs along the row make long fine streaks
        for (int pass = 0; pass < 2; ++pass) {
            double acc = 0; int cnt = 0;
            for (int x = 0; x < w + R; ++x) {
                if (x < w) { acc += n[static_cast<std::size_t>(x)]; ++cnt; }
                const int out = x - 2 * R - 1;
                if (out >= 0) { acc -= n[static_cast<std::size_t>(out)]; --cnt; }
                const int c = x - R;
                if (c >= 0 && c < w) sm[static_cast<std::size_t>(c)] = static_cast<float>(acc / std::max(cnt, 1));
            }
            n.swap(sm);
        }
        const float t = static_cast<float>(y) / static_cast<float>(h - 1);
        // vertical tone: lit from above, darker in the middle, a little lift at the bottom edge
        const float base = t < 0.5f ? 62.0f - 28.0f * (t / 0.5f) : 34.0f + 12.0f * ((t - 0.5f) / 0.5f);
        const float rowBias = rnd() * 5.0f;
        auto* row = reinterpret_cast<std::uint8_t*>(static_cast<std::uint8_t*>(bd.Scan0) + static_cast<std::ptrdiff_t>(y) * bd.Stride);
        for (int x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(w - 1);
            const float sheen = 9.0f * std::exp(-((fx - 0.28f) * (fx - 0.28f)) / 0.05f) + 5.0f * std::exp(-((fx - 0.82f) * (fx - 0.82f)) / 0.03f);
            float v = base + sheen + rowBias + n[static_cast<std::size_t>(x)] * 150.0f + rnd() * 3.0f;
            v = std::clamp(v, 0.0f, 255.0f);
            row[4 * x + 0] = static_cast<std::uint8_t>(v * 1.04f > 255.0f ? 255.0f : v * 1.04f);   // blue a touch up: cool titanium
            row[4 * x + 1] = static_cast<std::uint8_t>(v * 1.01f > 255.0f ? 255.0f : v * 1.01f);
            row[4 * x + 2] = static_cast<std::uint8_t>(v);
            row[4 * x + 3] = 255;
        }
    }
    bmp->UnlockBits(&bd);
    return bmp;
}

static void screw(Graphics& g, float cx, float cy, float r, float angleDeg) {
    // sunk shadow
    SolidBrush sh(rgba(0, 0, 0, 120));
    g.FillEllipse(&sh, cx - r - 1.0f, cy - r + 1.5f, 2 * r + 2.0f, 2 * r + 2.0f);
    LinearGradientBrush rim(RectF(cx - r, cy - r, 2 * r, 2 * r), rgba(150, 156, 164), rgba(24, 26, 29), LinearGradientModeForwardDiagonal);
    g.FillEllipse(&rim, cx - r, cy - r, 2 * r, 2 * r);
    LinearGradientBrush face(RectF(cx - r * 0.82f, cy - r * 0.82f, r * 1.64f, r * 1.64f), rgba(40, 43, 47), rgba(122, 128, 136), LinearGradientModeForwardDiagonal);
    g.FillEllipse(&face, cx - r * 0.82f, cy - r * 0.82f, r * 1.64f, r * 1.64f);
    GraphicsState st = g.Save();
    g.TranslateTransform(cx, cy);
    g.RotateTransform(angleDeg);
    SolidBrush slot(rgba(8, 9, 10));
    g.FillRectangle(&slot, -r * 0.62f, -r * 0.12f, r * 1.24f, r * 0.24f);
    SolidBrush hi(rgba(190, 196, 204, 90));
    g.FillRectangle(&hi, -r * 0.62f, r * 0.12f, r * 1.24f, 1.0f);
    g.Restore(st);
}

void drawPlateDressing(Graphics& g, int w, int h) {
    const float ear = 94.0f;
    LinearGradientBrush earL(RectF(0, 0, ear, static_cast<float>(h)), rgba(74, 79, 86, 170), rgba(43, 46, 51, 170), LinearGradientModeHorizontal);
    g.FillRectangle(&earL, 0.0f, 0.0f, ear, static_cast<float>(h));
    LinearGradientBrush earR(RectF(w - ear, 0, ear, static_cast<float>(h)), rgba(43, 46, 51, 170), rgba(74, 79, 86, 170), LinearGradientModeHorizontal);
    g.FillRectangle(&earR, static_cast<float>(w) - ear, 0.0f, ear, static_cast<float>(h));
    Pen dark(rgba(8, 9, 10), 2.0f), light(rgba(120, 126, 134, 150), 1.0f);
    for (float x : {ear, static_cast<float>(w) - ear}) { g.DrawLine(&dark, x, 0.0f, x, static_cast<float>(h)); g.DrawLine(&light, x + 2.0f, 0.0f, x + 2.0f, static_cast<float>(h)); }
    // top and bottom edge lines
    SolidBrush top(rgba(110, 116, 124)), bot(rgba(8, 9, 10));
    g.FillRectangle(&top, 0.0f, 0.0f, static_cast<float>(w), 3.0f);
    g.FillRectangle(&bot, 0.0f, static_cast<float>(h) - 3.0f, static_cast<float>(w), 3.0f);
    // rack slots
    for (float cx : {52.0f, static_cast<float>(w) - 52.0f}) for (float cy : {122.0f, static_cast<float>(h) - 138.0f}) {
        RectF r(cx - 12.0f, cy - 30.0f, 24.0f, 60.0f);
        LinearGradientBrush rim(r, rgba(138, 144, 152), rgba(15, 17, 19), LinearGradientModeForwardDiagonal);
        fillRounded(g, rim, r, 12.0f);
        SolidBrush hole(rgba(3, 4, 4));
        fillRounded(g, hole, inset(r, 4.0f), 8.0f);
    }
}

RectF drawWindowFrame(Graphics& g, RectF o, bool withScrews) {
    // drop shadow of the frame onto the plate
    for (int i = 6; i >= 1; --i) { SolidBrush s(rgba(0, 0, 0, 16)); fillRounded(g, s, RectF(o.X - i + 2.0f, o.Y - i + 4.0f, o.Width + 2.0f * i, o.Height + 2.0f * i), 14.0f + i); }
    LinearGradientBrush bevel(o, rgba(150, 156, 164), rgba(15, 17, 19), LinearGradientModeForwardDiagonal);
    fillRounded(g, bevel, o, 12.0f);
    RectF a = inset(o, 7.0f);
    SolidBrush well(rgba(9, 11, 13));
    fillRounded(g, well, a, 8.0f);
    RectF glass = inset(o, 15.0f);
    LinearGradientBrush gl(glass, rgba(13, 20, 25), rgba(4, 6, 8), LinearGradientModeVertical);
    fillRounded(g, gl, glass, 4.0f);
    if (withScrews) {
        screw(g, o.X - 14.0f, o.Y - 14.0f, 10.0f, 35.0f); screw(g, o.GetRight() + 14.0f, o.Y - 14.0f, 10.0f, 80.0f);
        screw(g, o.X - 14.0f, o.GetBottom() + 14.0f, 10.0f, 120.0f); screw(g, o.GetRight() + 14.0f, o.GetBottom() + 14.0f, 10.0f, 10.0f);
    }
    return glass;
}

void drawGlassSheen(Graphics& g, RectF glass) {
    GraphicsState st = g.Save();
    GraphicsPath clip; roundedPath(clip, glass, 4.0f);
    g.SetClip(&clip);
    // inner edge shadow
    for (int i = 0; i < 9; ++i) { Pen p(rgba(0, 0, 0, 120 - i * 12), 2.0f); g.DrawRectangle(&p, glass.X + i, glass.Y + i, glass.Width - 2.0f * i, glass.Height - 2.0f * i); }
    // reflection: a soft diagonal wedge across the upper left
    PointF wedge[4] = {PointF(glass.X, glass.Y), PointF(glass.X + glass.Width * 0.55f, glass.Y), PointF(glass.X + glass.Width * 0.25f, glass.GetBottom()), PointF(glass.X, glass.GetBottom())};
    LinearGradientBrush shine(PointF(glass.X, glass.Y), PointF(glass.X + glass.Width * 0.5f, glass.Y + glass.Height * 0.6f), rgba(255, 255, 255, 22), rgba(255, 255, 255, 0));
    g.FillPolygon(&shine, wedge, 4);
    g.Restore(st);
}

void text(Graphics& g, const std::wstring& s, RectF box, Face face, float px, Color c, StringAlignment h, StringAlignment v) {
    StringFormat sf;
    sf.SetAlignment(h); sf.SetLineAlignment(v);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);
    SolidBrush br(c);
    g.DrawString(s.c_str(), static_cast<INT>(s.size()), fontFor(face, px), box, &sf, &br);
}

void engraved(Graphics& g, const std::wstring& s, float x, float y, float px, StringAlignment h, bool title) {
    const Face f = title ? Face::Title : Face::Legend;
    const float W = 900.0f;
    const float bx = h == StringAlignmentNear ? x : (h == StringAlignmentCenter ? x - W / 2 : x - W);
    text(g, s, RectF(bx + 1.0f, y + 1.5f, W, px * 1.6f), f, px, rgba(0, 0, 0, 200), h);
    text(g, s, RectF(bx, y, W, px * 1.6f), f, px, rgba(200, 204, 211), h);
}

void drawKnob(Graphics& g, const Knob& k, const std::wstring& readout) {
    const float cx = k.cx, cy = k.cy, r = k.r;
    const double a0 = -135.0, a1 = 135.0;
    auto pt = [&](double deg, float rad) { const double t = deg * 3.14159265358979 / 180.0; return PointF(cx + static_cast<float>(std::sin(t)) * rad, cy - static_cast<float>(std::cos(t)) * rad); };
    // scale: 11 ticks around the knob; ticks up to the current value glow
    const double tNow = k.t01();
    for (int i = 0; i <= 10; ++i) {
        const double t = i / 10.0, a = a0 + (a1 - a0) * t;
        const bool on = t <= tNow + 1e-9;
        Pen p(on ? rgba(255, 176, 64) : rgba(150, 156, 164), (i % 5 == 0) ? 3.0f : 2.0f);
        p.SetStartCap(LineCapRound); p.SetEndCap(LineCapRound);
        g.DrawLine(&p, pt(a, r * 1.18f), pt(a, r * ((i % 5 == 0) ? 1.42f : 1.34f)));
    }
    wchar_t lo[24], hi[24];
    std::swprintf(lo, 24, L"%g", k.lo); std::swprintf(hi, 24, L"%g", k.hi);
    const PointF pl = pt(a0, r * 1.62f), ph = pt(a1, r * 1.62f);
    text(g, lo, RectF(pl.X - 40.0f, pl.Y - 8.0f, 80.0f, 18.0f), Face::Mono, 14.0f, rgba(170, 176, 184), StringAlignmentCenter);
    text(g, hi, RectF(ph.X - 40.0f, ph.Y - 8.0f, 80.0f, 18.0f), Face::Mono, 14.0f, rgba(170, 176, 184), StringAlignmentCenter);

    // shadow under the knob
    for (int i = 10; i >= 1; --i) { SolidBrush s(rgba(0, 0, 0, 14)); g.FillEllipse(&s, cx - r - i + 4.0f, cy - r - i + 8.0f, 2 * r + 2.0f * i, 2 * r + 2.0f * i); }
    // outer ring
    LinearGradientBrush ring(RectF(cx - r, cy - r, 2 * r, 2 * r), rgba(168, 174, 182), rgba(14, 16, 18), LinearGradientModeForwardDiagonal);
    g.FillEllipse(&ring, cx - r, cy - r, 2 * r, 2 * r);
    // knurled grip: 72 fine ridges
    const float rk0 = r * 0.84f, rk1 = r * 0.97f;
    for (int i = 0; i < 72; ++i) {
        const double a = i * 5.0;
        const double lit = std::cos((a - 40.0) * 3.14159265358979 / 180.0);   // light from the upper left
        const int v = static_cast<int>(70 + 60 * lit);
        Pen p(rgba(std::clamp(v, 0, 255), std::clamp(v + 2, 0, 255), std::clamp(v + 6, 0, 255), 230), 2.0f);
        g.DrawLine(&p, pt(a, rk0), pt(a, rk1));
    }
    // cap
    const float rc = r * 0.80f;
    {
        GraphicsPath gp; gp.AddEllipse(cx - rc, cy - rc, 2 * rc, 2 * rc);
        PathGradientBrush pb(&gp);
        pb.SetCenterPoint(PointF(cx - rc * 0.35f, cy - rc * 0.40f));
        pb.SetCenterColor(rgba(138, 144, 152));
        Color sur = rgba(26, 28, 31);
        INT cnt = 1; pb.SetSurroundColors(&sur, &cnt);
        g.FillPath(&pb, &gp);
    }
    // machined rings on the cap
    for (int i = 1; i <= 7; ++i) {
        const float rr = rc * (0.18f + 0.115f * i);
        Pen p((i % 2) ? rgba(255, 255, 255, 18) : rgba(0, 0, 0, 40), 1.0f);
        g.DrawEllipse(&p, cx - rr, cy - rr, 2 * rr, 2 * rr);
    }
    // soft angular highlight across the cap (brushed-aluminium look)
    {
        GraphicsState st = g.Save();
        GraphicsPath clip; clip.AddEllipse(cx - rc, cy - rc, 2 * rc, 2 * rc);
        g.SetClip(&clip);
        PointF band[4] = {PointF(cx - rc, cy - rc * 0.55f), PointF(cx + rc, cy - rc * 0.05f), PointF(cx + rc, cy + rc * 0.15f), PointF(cx - rc, cy - rc * 0.30f)};
        SolidBrush hi2(rgba(255, 255, 255, 20));
        g.FillPolygon(&hi2, band, 4);
        g.Restore(st);
    }
    Pen edge(rgba(0, 0, 0, 200), 1.5f);
    g.DrawEllipse(&edge, cx - rc, cy - rc, 2 * rc, 2 * rc);
    // pointer
    const double ang = a0 + (a1 - a0) * tNow;
    Pen groove(rgba(0, 0, 0, 220), 6.0f); groove.SetStartCap(LineCapRound); groove.SetEndCap(LineCapRound);
    g.DrawLine(&groove, pt(ang, rc * 0.30f), pt(ang, rc * 0.86f));
    Pen mark(k.dragging ? rgba(255, 200, 110) : rgba(240, 242, 246), 3.0f); mark.SetStartCap(LineCapRound); mark.SetEndCap(LineCapRound);
    g.DrawLine(&mark, pt(ang, rc * 0.34f), pt(ang, rc * 0.84f));

    // legend and the numeric readout window under it
    engraved(g, k.legend, cx, cy + r * 1.72f + 22.0f, 20.0f, StringAlignmentCenter);
    RectF lcd(cx - 66.0f, cy + r * 1.72f + 54.0f, 132.0f, 38.0f);
    RectF in = drawWindowFrame(g, lcd, false);
    text(g, readout, in, Face::MonoBold, 24.0f, rgba(255, 176, 64), StringAlignmentCenter, StringAlignmentCenter);
    drawGlassSheen(g, in);
}

void drawButton(Graphics& g, const Button& b) {
    RectF o(b.x, b.y, b.w, b.h);
    for (int i = 5; i >= 1; --i) { SolidBrush s(rgba(0, 0, 0, 16)); fillRounded(g, s, RectF(o.X - i + 1.0f, o.Y - i + 3.0f, o.Width + 2.0f * i, o.Height + 2.0f * i), 12.0f + i); }
    LinearGradientBrush bevel(o, rgba(150, 156, 164), rgba(15, 17, 19), LinearGradientModeForwardDiagonal);
    fillRounded(g, bevel, o, 12.0f);
    SolidBrush well(rgba(8, 9, 10));
    fillRounded(g, well, inset(o, 6.0f), 8.0f);
    const float drop = b.down ? 3.0f : 0.0f;
    RectF cap(o.X + 10.0f, o.Y + 10.0f + drop, o.Width - 20.0f, o.Height - 20.0f - (b.down ? 0.0f : 3.0f));
    const int top = b.down ? 44 : 92, bot = b.down ? 26 : 38;
    LinearGradientBrush capB(cap, rgba(top, top + 2, top + 6), rgba(bot, bot + 2, bot + 4), LinearGradientModeVertical);
    fillRounded(g, capB, cap, 6.0f);
    Pen rim(rgba(255, 255, 255, b.down ? 20 : 60), 1.0f);
    strokeRounded(g, rim, inset(cap, 0.5f), 6.0f);
    // lamp strip
    RectF lamp(cap.X + 12.0f, cap.Y + 5.0f, cap.Width - 24.0f, 4.0f);
    SolidBrush lampB(b.lit ? rgba(255, 176, 64) : rgba(24, 24, 24));
    fillRounded(g, lampB, lamp, 2.0f);
    if (b.lit) { SolidBrush glow(rgba(255, 176, 64, 40)); fillRounded(g, glow, RectF(lamp.X - 4.0f, lamp.Y - 3.0f, lamp.Width + 8.0f, lamp.Height + 6.0f), 4.0f); }
    text(g, b.label, RectF(cap.X, cap.Y + 6.0f, cap.Width, cap.Height - 6.0f), Face::Legend, 21.0f, b.enabled ? rgba(226, 230, 236) : rgba(110, 114, 120), StringAlignmentCenter, StringAlignmentCenter);
}

bool savePng(Bitmap& bmp, const std::wstring& path) {
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<std::uint8_t> buf(size);
    auto* enc = reinterpret_cast<ImageCodecInfo*>(buf.data());
    GetImageEncoders(num, size, enc);
    for (UINT i = 0; i < num; ++i) if (std::wstring(enc[i].MimeType) == L"image/png") return bmp.Save(path.c_str(), &enc[i].Clsid, nullptr) == Ok;
    return false;
}

} // namespace hem::ui

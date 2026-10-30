// HEM Comb and Click Finder: a Windows program. Pure C++ (Win32 + GDI+), no JUCE, no web page.
//   hem_comb_click.exe                      opens the window
//   hem_comb_click.exe --snapshot out.png [--made-up <clickMs>] [--period <ms>] [--idle]
//        draws the window to a picture. With --made-up a made-up noise clip with a click train is scanned first
//        (the picture says MADE-UP). Used by the build machine so the look can be checked without a person.
#include "CombClickModel.hpp"
#include "HemUi.hpp"

#include <commdlg.h>
#include <shellapi.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace hem;
using namespace hem::ui;
namespace LY = hem::ui::layout;
namespace wav = hc::hiss::wav;
using Gdiplus::Graphics;
using Gdiplus::RectF;
using Gdiplus::PointF;

namespace {

enum class Phase { Idle, Scanning, Done, Failed };

struct View {
    Phase phase = Phase::Idle;
    Knob knob;
    Button bOpen, bScan, bSave;
    std::wstring path;                         // file on disk (empty for the made-up clip)
    std::string name = "(no file)";
    std::string message = "Press OPEN, or drop a WAV file on the window.";
    std::shared_ptr<const wav::File> file;     // the audio (shared with the scan thread)
    bool madeUp = false;
    combclick::ScanResult res;
    bool haveRes = false;
    std::string fileInfo;
    int blink = 0;
    View() {
        knob.cx = LY::kKnobX; knob.cy = LY::kKnobY; knob.r = LY::kKnobR;
        knob.lo = 10; knob.hi = 100; knob.def = 40; knob.step = 1; knob.value = 40;
        knob.legend = L"CLICK PERIOD"; knob.unit = L" ms";
        bOpen = {LY::kBtn0.x, LY::kBtn0.y, LY::kBtn0.w, LY::kBtn0.h, L"OPEN WAV"};
        bScan = {LY::kBtn1.x, LY::kBtn1.y, LY::kBtn1.w, LY::kBtn1.h, L"SCAN"};
        bSave = {LY::kBtn2.x, LY::kBtn2.y, LY::kBtn2.w, LY::kBtn2.h, L"SAVE REPORT"};
        refreshButtons();
    }
    void refreshButtons() {
        const bool busy = phase == Phase::Scanning;
        bOpen.enabled = !busy;
        bScan.enabled = file && !busy;
        bSave.enabled = file && phase == Phase::Done && !busy;
        bScan.lit = busy;
        bSave.lit = false;
        bOpen.lit = false;
    }
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string narrowAnsi(const std::wstring& w) {     // wav::read opens files with the narrow C library, which uses the ANSI code page
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring fmt(const wchar_t* f, double a) { wchar_t b[96]; std::swprintf(b, 96, f, a); return b; }
std::wstring fmt2(const wchar_t* f, double a, double b2) { wchar_t b[128]; std::swprintf(b, 128, f, a, b2); return b; }

// ---------------------------------------------------------------- drawing of the three kinds of window contents
const Gdiplus::Color kAmber = rgba(255, 176, 64), kGreen = rgba(120, 235, 160), kRed = rgba(255, 96, 80), kDim = rgba(120, 138, 150), kText = rgba(205, 220, 230);

void drawFold(Graphics& g, RectF gl, const View& v) {
    const bool stale = v.haveRes && v.res.periodMs != v.knob.value;
    text(g, v.haveRes ? fmt(L"CLICK BAND ADDED UP INSIDE ONE %.4g ms FRAME, OVER THE WHOLE FILE", v.res.periodMs) : L"CLICK BAND ADDED UP INSIDE ONE FRAME", RectF(gl.X + 22, gl.Y + 14, gl.Width - 44, 24), Face::Legend, 16.0f, kDim);
    const RectF plot(gl.X + 80, gl.Y + 62, gl.Width - 130, gl.Height - 138);
    Gdiplus::Pen grid(rgba(40, 60, 72), 1.0f);
    for (int i = 0; i <= 4; ++i) { const float y = plot.Y + plot.Height * i / 4.0f; g.DrawLine(&grid, plot.X, y, plot.GetRight(), y); }
    if (!v.haveRes || !v.res.ok || v.res.seam.foldProfile.empty()) {
        std::wstring msg = v.phase == Phase::Scanning ? L"SCANNING..." : (v.haveRes && v.res.ok ? L"THIS FILE IS TOO SHORT FOR THIS PERIOD. NOTHING IS SHOWN." : L"NO RESULT YET");
        if (v.phase == Phase::Scanning && (v.blink / 4) % 2) msg = L"SCANNING";
        text(g, msg, plot, Face::MonoBold, 28.0f, v.phase == Phase::Scanning ? kAmber : kDim, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
        return;
    }
    const auto& s = v.res.seam;
    float top = s.foldBar;
    for (float p : s.foldProfile) top = std::max(top, p);
    top *= 1.18f;
    if (top <= 0.0f) top = 1.0f;
    const std::size_t nb = s.foldProfile.size();
    std::vector<PointF> pts;
    pts.reserve(nb + 2);
    pts.push_back(PointF(plot.X, plot.GetBottom()));
    for (std::size_t i = 0; i < nb; ++i) {
        const float x = plot.X + plot.Width * (static_cast<float>(i) + 0.5f) / static_cast<float>(nb);
        pts.push_back(PointF(x, plot.GetBottom() - plot.Height * s.foldProfile[i] / top));
    }
    pts.push_back(PointF(plot.GetRight(), plot.GetBottom()));
    Gdiplus::LinearGradientBrush fill(plot, rgba(80, 220, 150, stale ? 40 : 110), rgba(80, 220, 150, 6), Gdiplus::LinearGradientModeVertical);
    g.FillPolygon(&fill, pts.data(), static_cast<INT>(pts.size()));
    Gdiplus::Pen line(stale ? rgba(110, 150, 130) : kGreen, 1.6f);
    g.DrawLines(&line, pts.data() + 1, static_cast<INT>(nb));
    // the bar: a clean file only crosses it 1 time in 100 (by design; 0 of 140 clean made-up clips crossed it in the tests)
    const float by = plot.GetBottom() - plot.Height * s.foldBar / top;
    Gdiplus::Pen bar(kAmber, 2.0f); bar.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawLine(&bar, plot.X, by, plot.GetRight(), by);
    text(g, L"1-IN-100 BAR", RectF(plot.GetRight() - 220, by - 24, 220, 20), Face::Legend, 15.0f, kAmber, Gdiplus::StringAlignmentFar);
    // time axis
    const double P = s.foldProfileMs;
    const double stepMs = P > 80 ? 20 : (P > 40 ? 10 : (P > 20 ? 5 : 2));
    for (double t = 0; t <= P + 1e-6; t += stepMs) {
        const float x = plot.X + plot.Width * static_cast<float>(t / P);
        g.DrawLine(&grid, x, plot.GetBottom(), x, plot.GetBottom() + 8.0f);
        text(g, fmt(L"%g", t), RectF(x - 30, plot.GetBottom() + 10, 60, 20), Face::Mono, 14.0f, kDim, Gdiplus::StringAlignmentCenter);
    }
    text(g, L"ms INSIDE THE FRAME", RectF(plot.X, plot.GetBottom() + 32, plot.Width, 20), Face::Legend, 14.0f, kDim, Gdiplus::StringAlignmentCenter);
    if (s.pattern) {
        const float x = plot.X + plot.Width * static_cast<float>(s.foldPhaseMs / P);
        Gdiplus::Pen mk(kRed, 2.0f);
        g.DrawLine(&mk, x, plot.Y, x, plot.GetBottom());
        const bool right = x > plot.X + plot.Width * 0.6f;
        text(g, fmt(L"CLICK AT %.2f ms", s.foldPhaseMs), RectF(right ? x - 260 : x + 8, plot.Y + 2, 252, 22), Face::MonoBold, 17.0f, kRed, right ? Gdiplus::StringAlignmentFar : Gdiplus::StringAlignmentNear);
    }
    if (stale) text(g, fmt(L"SHOWING THE SCAN AT %.4g ms. THE KNOB HAS MOVED: PRESS SCAN.", v.res.periodMs), RectF(gl.X + 22, gl.GetBottom() - 30, gl.Width - 44, 22), Face::MonoBold, 15.0f, kAmber);
}

void drawVerdict(Graphics& g, RectF gl, const View& v) {
    text(g, L"VERDICT", RectF(gl.X + 14, gl.Y + 8, gl.Width - 28, 20), Face::Legend, 14.0f, kDim);
    if (!v.haveRes || !v.res.ok) { text(g, v.phase == Phase::Scanning ? L"SCANNING" : L"NOT RUN", RectF(gl.X, gl.Y + 30, gl.Width, 60), Face::MonoBold, 28.0f, v.phase == Phase::Scanning ? kAmber : kDim, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter); return; }
    const auto& s = v.res.seam;
    if (!s.valid || s.nullFolds == 0) { text(g, L"TOO SHORT", RectF(gl.X, gl.Y + 30, gl.Width, 60), Face::MonoBold, 28.0f, kAmber, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter); return; }
    text(g, s.pattern ? L"REPEATING CLICKS" : L"NONE SHOWN", RectF(gl.X, gl.Y + 26, gl.Width, 40), Face::MonoBold, s.pattern ? 25.0f : 27.0f, s.pattern ? kRed : kGreen, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
    text(g, fmt(L"chance on a clean file %.4f", s.foldP), RectF(gl.X + 14, gl.Y + 72, gl.Width - 28, 20), Face::Mono, 15.0f, kText);
    text(g, fmt2(L"period %.4g ms   %.0f null folds", v.res.periodMs, static_cast<double>(s.nullFolds)), RectF(gl.X + 14, gl.Y + 94, gl.Width - 28, 20), Face::Mono, 15.0f, kDim);
}

void drawLimits(Graphics& g, RectF gl, const View& v) {
    text(g, L"SMALLEST CLICK THIS FILE CAN SHOW", RectF(gl.X + 14, gl.Y + 8, gl.Width - 28, 20), Face::Legend, 14.0f, kDim);
    if (!v.haveRes || !v.res.ok || v.res.seam.nullFolds == 0) { text(g, L"--", gl, Face::MonoBold, 30.0f, kDim, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter); return; }
    const auto& s = v.res.seam;
    text(g, L"all one direction", RectF(gl.X + 14, gl.Y + 34, 160, 20), Face::Mono, 15.0f, kDim);
    text(g, fmt(L"%.1f dBFS", s.detectLimitDb), RectF(gl.X + 120, gl.Y + 30, gl.Width - 134, 28), Face::MonoBold, 22.0f, kAmber, Gdiplus::StringAlignmentFar);
    text(g, L"either direction", RectF(gl.X + 14, gl.Y + 66, 160, 20), Face::Mono, 15.0f, kDim);
    text(g, fmt(L"%.1f dBFS", s.detectLimitAnySignDb), RectF(gl.X + 120, gl.Y + 62, gl.Width - 134, 28), Face::MonoBold, 22.0f, kAmber, Gdiplus::StringAlignmentFar);
    if (s.pattern) text(g, fmt(L"click found: %.1f dBFS peak", s.foldLevelDb), RectF(gl.X + 14, gl.Y + 100, gl.Width - 28, 22), Face::MonoBold, 16.0f, kRed);
    else text(g, L"worked out from this file's own noise", RectF(gl.X + 14, gl.Y + 102, gl.Width - 28, 20), Face::Mono, 13.0f, kDim);
}

void drawComb(Graphics& g, RectF gl, const View& v) {
    text(g, L"25 Hz COMB LINES, 3 TO 20 kHz", RectF(gl.X + 14, gl.Y + 8, gl.Width - 28, 20), Face::Legend, 14.0f, kDim);
    const RectF plot(gl.X + 14, gl.Y + 34, gl.Width - 28, gl.Height - 84);
    Gdiplus::Pen grid(rgba(40, 60, 72), 1.0f);
    g.DrawRectangle(&grid, plot.X, plot.Y, plot.Width, plot.Height);
    if (!v.haveRes || !v.res.ok || !v.res.comb.valid || v.res.comb.lineDb.size() < 8) {
        text(g, v.haveRes && v.res.ok ? L"CLIP TOO SHORT, OR RATE NOT A MULTIPLE OF 25 Hz" : L"--", plot, Face::Mono, 14.0f, kDim, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentCenter);
        return;
    }
    const auto& d = v.res.comb.lineDb;
    // show the middle 98 % of the levels so one odd line does not flatten the picture
    std::vector<double> sorted(d);
    std::sort(sorted.begin(), sorted.end());
    const double lo = sorted[sorted.size() / 100], hi = sorted[sorted.size() - 1 - sorted.size() / 100];
    const double span = std::max(hi - lo, 1.0);
    const std::size_t cols = static_cast<std::size_t>(plot.Width);
    std::vector<PointF> pts;
    for (std::size_t c = 0; c < cols; ++c) {
        const std::size_t a = c * d.size() / cols, b = std::max(a + 1, (c + 1) * d.size() / cols);
        double mx = -1e9; for (std::size_t i = a; i < b && i < d.size(); ++i) mx = std::max(mx, d[i]);
        pts.push_back(PointF(plot.X + static_cast<float>(c), plot.GetBottom() - plot.Height * static_cast<float>(std::clamp((mx - lo) / span, 0.0, 1.0))));
    }
    Gdiplus::Pen line(kGreen, 1.2f);
    g.DrawLines(&line, pts.data(), static_cast<INT>(pts.size()));
    text(g, fmt2(L"%.1f %% of lines beat their neighbours   median %.1f dBFS", v.res.comb.onGreaterThanOffPct, v.res.comb.medianLineDb), RectF(gl.X + 14, gl.GetBottom() - 40, gl.Width - 28, 22), Face::Mono, 13.5f, kText);
    text(g, L"about 45-50 % = no comb. clearly higher = comb.", RectF(gl.X + 14, gl.GetBottom() - 22, gl.Width - 28, 18), Face::Mono, 12.0f, kDim);
}

void drawStatus(Graphics& g, RectF gl, const View& v) {
    std::wstring name = widen(v.name) + (v.madeUp ? L"   (MADE-UP CLIP, NOT A REAL SONG)" : L"");
    text(g, name, RectF(gl.X + 14, gl.Y + 6, gl.Width - 28, 24), Face::MonoBold, 17.0f, v.madeUp ? kAmber : kText);
    text(g, widen(v.fileInfo), RectF(gl.X + 14, gl.Y + 28, gl.Width - 28, 20), Face::Mono, 14.0f, kDim);
    text(g, widen(v.message), RectF(gl.X + 14, gl.Y + 46, gl.Width - 28, 20), Face::Mono, 14.5f, v.phase == Phase::Failed ? kRed : kGreen);
}

void render(Graphics& g, const View& v, Gdiplus::Bitmap& plate) {
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.DrawImage(&plate, 0, 0, LY::kW, LY::kH);
    drawPlateDressing(g, LY::kW, LY::kH);
    engraved(g, L"HEM  COMB AND CLICK FINDER", 180.0f, 22.0f, 30.0f, Gdiplus::StringAlignmentNear, true);
    engraved(g, L"SEAM CLICKS  ·  25 Hz COMB", 1540.0f, 34.0f, 17.0f, Gdiplus::StringAlignmentFar);

    auto rect = [](const LY::R& r) { return RectF(r.x, r.y, r.w, r.h); };
    RectF gBig = drawWindowFrame(g, rect(LY::kBig), true);
    drawFold(g, gBig, v); drawGlassSheen(g, gBig);
    RectF g0 = drawWindowFrame(g, rect(LY::kSmall0), false); drawVerdict(g, g0, v); drawGlassSheen(g, g0);
    RectF g1 = drawWindowFrame(g, rect(LY::kSmall1), false); drawLimits(g, g1, v); drawGlassSheen(g, g1);
    RectF g2 = drawWindowFrame(g, rect(LY::kSmall2), false); drawComb(g, g2, v); drawGlassSheen(g, g2);
    RectF gs = drawWindowFrame(g, rect(LY::kStatus), false); drawStatus(g, gs, v); drawGlassSheen(g, gs);

    drawKnob(g, v.knob, fmt(L"%.0f ms", v.knob.value));
    drawButton(g, v.bOpen); drawButton(g, v.bScan); drawButton(g, v.bSave);
    engraved(g, L"HYBRID ELITE MAFIA  ·  THE HYBRID CORE", 836.0f, 878.0f, 15.0f, Gdiplus::StringAlignmentCenter);
}

// ---------------------------------------------------------------- loading and scanning
bool loadWav(View& v, const std::wstring& path) {
    auto f = std::make_shared<wav::File>();
    std::string err;
    if (!wav::read(narrowAnsi(path), *f, err)) { v.phase = Phase::Failed; v.message = err; v.refreshButtons(); return false; }
    char b[160];
    const double secs = static_cast<double>(f->left.size()) / f->sampleRate;
    std::snprintf(b, sizeof b, "%d Hz   %d ch   %d bit%s   %d:%02d", f->sampleRate, f->channels, f->bits, f->isFloat ? " float" : "", static_cast<int>(secs) / 60, static_cast<int>(secs) % 60);
    v.fileInfo = b;
    v.path = path;
    v.name = std::filesystem::path(path).filename().string();
    v.madeUp = false;
    v.file = f;
    v.haveRes = false;
    v.phase = Phase::Idle;
    v.message = "Loaded. Set the click period with the knob, then press SCAN.";
    v.refreshButtons();
    return true;
}

HWND g_hwnd = nullptr;
constexpr UINT WM_SCAN_DONE = WM_APP + 1;
std::unique_ptr<View> g_view;
std::unique_ptr<Gdiplus::Bitmap> g_plate;
std::atomic<unsigned> g_job{0};

void startScan(View& v) {
    if (!v.file || v.phase == Phase::Scanning) return;
    v.phase = Phase::Scanning; v.message = "Scanning..."; v.refreshButtons();
    const unsigned job = ++g_job;
    auto file = v.file; const double period = v.knob.value; HWND hwnd = g_hwnd;
    std::thread([file, period, job, hwnd]() {
        auto* r = new combclick::ScanResult(combclick::scan(file->left, file->right, file->sampleRate, period));
        if (!PostMessageW(hwnd, WM_SCAN_DONE, job, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void finishScan(View& v, combclick::ScanResult* r, unsigned job) {
    std::unique_ptr<combclick::ScanResult> own(r);
    if (job != g_job.load()) return;
    v.res = *own; v.haveRes = true;
    v.phase = own->ok ? Phase::Done : Phase::Failed;
    char b[200];
    if (!own->ok) std::snprintf(b, sizeof b, "Could not scan: %s", own->error.c_str());
    else if (!own->seam.valid) std::snprintf(b, sizeof b, "Clip shorter than 0.5 s: no click scan. Scanned in %.1f s.", own->secondsTaken);
    else std::snprintf(b, sizeof b, "Scanned in %.1f s at %.4g ms. Checked on made-up clips only; real songs: UNKNOWN.", own->secondsTaken, own->periodMs);
    v.message = b;
    v.refreshButtons();
}

void saveReport(View& v) {
    if (!v.file || v.path.empty()) { v.message = "Nothing to save: this is the made-up clip."; return; }
    std::filesystem::path out = v.path;
    out.replace_extension(L"");
    out += L"_combclick.md";
    const std::string body = combclick::reportText(v.file->left, v.file->right, v.file->sampleRate, v.haveRes ? v.res.periodMs : v.knob.value, v.name);
    std::ofstream f(out, std::ios::binary);
    if (!f) { v.phase = Phase::Failed; v.message = "Could not write the report next to the WAV file."; return; }
    f << body;
    v.message = "Report saved: " + out.filename().string();
}

void openDialog(View& v) {
    wchar_t buf[MAX_PATH * 2] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o; o.hwndOwner = g_hwnd; o.lpstrFile = buf; o.nMaxFile = static_cast<DWORD>(sizeof buf / sizeof buf[0]);
    o.lpstrFilter = L"WAV files\0*.wav\0All files\0*.*\0\0";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&o)) loadWav(v, buf);
}

// ---------------------------------------------------------------- window
float g_scale = 1.0f, g_offX = 0.0f, g_offY = 0.0f;
void mapMouse(LPARAM lp, float& x, float& y) {
    x = (static_cast<float>(static_cast<short>(LOWORD(lp))) - g_offX) / g_scale;
    y = (static_cast<float>(static_cast<short>(HIWORD(lp))) - g_offY) / g_scale;
}
Button* buttonAt(View& v, float x, float y) { for (Button* b : {&v.bOpen, &v.bScan, &v.bSave}) if (b->hit(x, y)) return b; return nullptr; }
Button* g_held = nullptr;

LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    View& v = *g_view;
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        const int cw = std::max<int>(rc.right, 1), ch = std::max<int>(rc.bottom, 1);
        g_scale = std::min(static_cast<float>(cw) / LY::kW, static_cast<float>(ch) / LY::kH);
        g_offX = (cw - LY::kW * g_scale) * 0.5f; g_offY = (ch - LY::kH * g_scale) * 0.5f;
        Gdiplus::Bitmap back(cw, ch, PixelFormat32bppARGB);
        {
            Graphics g(&back);
            g.Clear(Gdiplus::Color(255, 10, 11, 12));
            g.TranslateTransform(g_offX, g_offY);
            g.ScaleTransform(g_scale, g_scale);
            render(g, v, *g_plate);
        }
        Graphics screen(dc);
        screen.DrawImage(&back, 0, 0);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_TIMER: ++v.blink; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        float x, y; mapMouse(lp, x, y);
        SetCapture(h);
        if (v.knob.hit(x, y)) v.knob.begin(y);
        else if (Button* b = buttonAt(v, x, y); b && b->enabled) { b->down = true; g_held = b; }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        float x, y; mapMouse(lp, x, y);
        if (v.knob.hit(x, y)) { v.knob.reset(); InvalidateRect(h, nullptr, FALSE); }
        else if (Button* b = buttonAt(v, x, y); b && b->enabled) { b->down = true; g_held = b; InvalidateRect(h, nullptr, FALSE); SetCapture(h); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (v.knob.dragging) { float x, y; mapMouse(lp, x, y); v.knob.drag(y, (wp & MK_SHIFT) != 0); InvalidateRect(h, nullptr, FALSE); }
        else if (g_held) { float x, y; mapMouse(lp, x, y); const bool in = g_held->hit(x, y); if (g_held->down != in) { g_held->down = in; InvalidateRect(h, nullptr, FALSE); } }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x, y; mapMouse(lp, x, y);
        ReleaseCapture();
        v.knob.end();
        if (g_held) {
            Button* b = g_held; g_held = nullptr;
            const bool fire = b->down && b->hit(x, y) && b->enabled;
            b->down = false;
            if (fire) {
                if (b == &v.bOpen) openDialog(v);
                else if (b == &v.bScan) startScan(v);
                else if (b == &v.bSave) saveReport(v);
            }
        }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT p{static_cast<LONG>(static_cast<short>(LOWORD(lp))), static_cast<LONG>(static_cast<short>(HIWORD(lp)))};
        ScreenToClient(h, &p);
        float x = (static_cast<float>(p.x) - g_offX) / g_scale, y = (static_cast<float>(p.y) - g_offY) / g_scale;
        if (v.knob.hit(x, y)) { v.knob.wheel(GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_DROPFILES: {
        wchar_t buf[MAX_PATH * 2];
        if (DragQueryFileW(reinterpret_cast<HDROP>(wp), 0, buf, static_cast<UINT>(sizeof buf / sizeof buf[0]))) { if (v.phase != Phase::Scanning) loadWav(v, buf); }
        DragFinish(reinterpret_cast<HDROP>(wp));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_SCAN_DONE: finishScan(v, reinterpret_cast<combclick::ScanResult*>(lp), static_cast<unsigned>(wp)); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------- picture mode (for the build machine)
int snapshot(const std::wstring& out, bool idle, double madeUpClickMs, double period) {
    View v;
    v.knob.set(period);
    if (!idle) {
        auto f = std::make_shared<wav::File>();
        f->sampleRate = 48000; f->channels = 2; f->bits = 24;
        f->left = combclick::madeUpClip(48000.0, 12.0, madeUpClickMs, -30.0, -40.0, 7);
        f->right = f->left;
        v.file = f; v.madeUp = true; v.name = "made-up noise with a click train";
        char b[160]; std::snprintf(b, sizeof b, "48000 Hz   2 ch   24 bit   0:12   noise -40 dBFS, click every %.4g ms at -30 dBFS", madeUpClickMs);
        v.fileInfo = b;
        auto r = combclick::scan(f->left, f->right, 48000.0, v.knob.value);
        v.res = r; v.haveRes = true; v.phase = r.ok ? Phase::Done : Phase::Failed;
        char m[200]; std::snprintf(m, sizeof m, "Scanned in %.1f s at %.4g ms.", r.secondsTaken, r.periodMs);
        v.message = m;
        v.refreshButtons();
    }
    auto plate = makePlate(LY::kW, LY::kH, 11);
    Gdiplus::Bitmap bmp(LY::kW, LY::kH, PixelFormat32bppARGB);
    { Graphics g(&bmp); render(g, v, *plate); }
    return savePng(bmp, out) ? 0 : 3;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    if (!startup()) return 2;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--snapshot" && i + 1 < argc) {
            bool idle = false; double clickMs = 0.0, period = 40.0;
            for (int j = i + 2; j < argc; ++j) {
                const std::wstring a = argv[j];
                if (a == L"--idle") idle = true;
                else if (a == L"--made-up" && j + 1 < argc) clickMs = _wtof(argv[++j]);
                else if (a == L"--period" && j + 1 < argc) period = _wtof(argv[++j]);
            }
            const int rc = snapshot(argv[i + 1], idle, clickMs, period);
            LocalFree(argv);
            shutdown();
            return rc;
        }
    }
    g_view = std::make_unique<View>();
    g_plate = makePlate(LY::kW, LY::kH, 11);
    if (argv && argc > 1 && argv[1][0] != L'-') loadWav(*g_view, argv[1]);
    if (argv) LocalFree(argv);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc; wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW; wc.lpfnWndProc = wndProc; wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.lpszClassName = L"HemCombClick"; wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);
    RECT r{0, 0, 1280, 720};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"HEM Comb and Click Finder", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) { shutdown(); return 1; }
    DragAcceptFiles(g_hwnd, TRUE);
    SetTimer(g_hwnd, 1, 120, nullptr);
    ShowWindow(g_hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_plate.reset();
    shutdown();
    return static_cast<int>(msg.wParam);
}

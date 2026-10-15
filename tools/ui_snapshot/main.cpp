// hc_ui_snapshot — renders the real console (ui::Console, the same code HybridCore.exe runs)
// off-screen with LIVE engine data and saves PNGs plus side-by-side comparisons with the
// faceplate art. Used by CI (software OpenGL) and works on the rig.
//
//   hc_ui_snapshot --out <folder> [--sizes 3840x2160,1389x868] [--seconds 2] [--device offline|default]
//
// Live data: the real Engine + 13-stem MixerProcessor (13 test tones) run on the offline clock
// (no sound card needed) or on the default WASAPI output; meters, DSP load and buffer numbers
// come from the running engine, exactly as in the app. A few controls are set first, the way a
// user would, so on/off states are visible (listed in the output).
//
// Checks (exit 3 on failure): GL errors, chassis pixels equal Main-Faceplate.png where nothing is
// drawn on top, meters lit by the engine, glass display populated.
#include "AudioHost.hpp"

#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Console.hpp"
#include "hybrid/ui/Gl.hpp"
#include "hybrid/ui/GlWindow.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace hc;

namespace {

struct Size { int w, h; };

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

// Mean absolute difference (0..255) between two images over a rectangle.
double meanAbsDiff(const ui::Image& a, const ui::Image& b, int x0, int y0, int x1, int y1) {
    double sum = 0;
    std::size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = a.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const std::uint8_t* q = b.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(p[c]) - static_cast<int>(q[c]));
            n += 3;
        }
    return n ? sum / static_cast<double>(n) : 0.0;
}

double regionStdDev(const ui::Image& img, int x0, int y0, int x1, int y1) {
    double sum = 0, sq = 0;
    std::size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const std::uint8_t* p = img.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const double l = 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
            sum += l; sq += l * l; ++n;
        }
    const double m = sum / static_cast<double>(std::max<std::size_t>(n, 1));
    return std::sqrt(std::max(0.0, sq / static_cast<double>(std::max<std::size_t>(n, 1)) - m * m));
}

// Pixels that look like lit meter LEDs (saturated green/yellow/red) inside a rectangle.
std::size_t litPixels(const ui::Image& img, int x0, int y0, int x1, int y1) {
    std::size_t n = 0;
    for (int y = std::max(0, y0); y < std::min<int>(y1, static_cast<int>(img.height)); ++y)
        for (int x = std::max(0, x0); x < std::min<int>(x1, static_cast<int>(img.width)); ++x) {
            const std::uint8_t* p = img.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
            const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
            if (mx > 150 && mx - mn > 90) ++n;
        }
    return n;
}

// Two images side by side on a dark strip, each scaled to `h` pixels tall.
ui::Image sideBySide(const ui::Image& left, const ui::Image& right, std::uint32_t h) {
    const auto lw = static_cast<std::uint32_t>(std::lround(static_cast<double>(left.width) * h / left.height));
    const auto rw = static_cast<std::uint32_t>(std::lround(static_cast<double>(right.width) * h / right.height));
    const ui::Image l = ui::resizeSrgb(left, lw, h), r = ui::resizeSrgb(right, rw, h);
    const std::uint32_t gap = 24;
    ui::Image out;
    out.width  = lw + gap + rw;
    out.height = h;
    out.rgba.assign(static_cast<std::size_t>(out.width) * h * 4, 0);
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
    for (std::uint32_t y = 0; y < h; ++y) {
        std::copy_n(l.at(0, y), static_cast<std::size_t>(lw) * 4, &out.rgba[(static_cast<std::size_t>(y) * out.width) * 4]);
        std::copy_n(r.at(0, y), static_cast<std::size_t>(rw) * 4, &out.rgba[(static_cast<std::size_t>(y) * out.width + lw + gap) * 4]);
    }
    return out;
}

void setUserControls(params::ParameterTree& t) {
    t.find("stem02.main")->setPlain(-6.0f);
    t.find("stem03.mute")->setPlain(1.0f);
    t.find("stem05.invert")->setPlain(1.0f);
    t.find("stem06.pan")->setPlain(-0.6f);
    t.find("stem07.pan")->setPlain(0.8f);
    t.find("stem08.trim")->setPlain(9.0f);
    t.find("stem09.aux1")->setPlain(-3.0f);
    t.find("stem10.aux2")->setPlain(-8.0f);
    t.find("stem11.main")->setPlain(-12.0f);
    t.find("stem12.trim")->setPlain(-6.0f);
    t.find("aux1.mute")->setPlain(1.0f);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    io::configureWideConsole();
    std::filesystem::path out = ".";
    std::vector<Size> sizes = {{3840, 2160}, {1389, 868}};
    double seconds = 2.0;
    bool   offline = true;
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--out" && i + 1 < argc) out = argv[++i];
        else if (k == L"--seconds" && i + 1 < argc) seconds = std::wcstod(argv[++i], nullptr);
        else if (k == L"--device" && i + 1 < argc) offline = std::wstring(argv[++i]) != L"default";
        else if (k == L"--sizes" && i + 1 < argc) {
            sizes.clear();
            std::wstring s = argv[++i];
            std::size_t pos = 0;
            while (pos < s.size()) {
                const std::size_t comma = s.find(L',', pos);
                const std::wstring item = s.substr(pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos);
                wchar_t* end = nullptr;
                const long w = std::wcstol(item.c_str(), &end, 10);
                const long h = (end && *end == L'x') ? std::wcstol(end + 1, nullptr, 10) : 0;
                if (w >= 320 && h >= 200 && w <= 4096 && h <= 4096) sizes.push_back({static_cast<int>(w), static_cast<int>(h)});
                if (comma == std::wstring::npos) break;
                pos = comma + 1;
            }
        } else {
            std::wprintf(L"usage: hc_ui_snapshot --out <folder> [--sizes 3840x2160,1389x868] [--seconds 2] [--device offline|default]\n");
            return 1;
        }
    }
    if (sizes.empty()) return 1;
    std::error_code ec;
    std::filesystem::create_directories(out, ec);

    // ---- live engine -----------------------------------------------------------------------
    params::ParameterTree tree;
    params::addMixerParameters(tree);
    app::AudioHost host;
    const params::MixerBindings bindings(tree);
    params::bindMixerParameters(tree, bindings, host.matrix());
    setUserControls(tree);
    tree.dispatchChanges();
    const auto devices = app::AudioHost::listDevices();
    host.open(offline ? devices.back() : devices.front());
    std::wprintf(L"Engine  : %hs\n          %hs\n", host.status().line1.c_str(), host.status().line2.c_str());
    std::wprintf(L"Controls set before the shots: stem 2 fader -6 dB, stem 3 mute, stem 5 phase, stem 6 pan L60, stem 7 pan R80,\n"
                 L"          stem 8 trim +9 dB, stem 9 aux1 -3 dB, stem 10 aux2 -8 dB, stem 11 fader -12 dB, stem 12 trim -6 dB, aux 1 mute.\n");

    ui::MeterState meters;
    auto pump = [&](double forSeconds) {
        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < forSeconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            tree.dispatchChanges();
            host.poll();
            const auto now = std::chrono::steady_clock::now();
            float sp[kStemCount], bl[kBusCount], br[kBusCount];
            for (std::uint32_t s = 0; s < kStemCount; ++s) sp[s] = host.matrix().stemPeak(s);
            for (std::uint32_t b = 0; b < kBusCount; ++b) { bl[b] = host.matrix().busPeak(b, 0); br[b] = host.matrix().busPeak(b, 1); }
            meters.update(sp, bl, br, std::chrono::duration<float>(now - last).count());
            last = now;
        }
    };
    pump(seconds);

    // ---- GPU ---------------------------------------------------------------------------------
    ui::GlWindow window;
    std::string err;
    if (!window.create(L"hc_ui_snapshot", 320, 200, false, &err)) { std::wprintf(L"GPU init failed: %hs\n", err.c_str()); return 2; }
    ui::Console console(tree);
    std::filesystem::path assets = exeDir() / "assets";   // build tree; installed: <app>\bin\..\assets
    if (!std::filesystem::is_directory(assets, ec)) assets = exeDir().parent_path() / "assets";
    if (!console.init(assets, {}, 8, &err)) { std::wprintf(L"console init failed: %hs\n", err.c_str()); return 2; }
    std::wprintf(L"OpenGL  : %hs\nRenderer: %hs\nMSAA    : %d samples (requested 8, GPU maximum %d)\nFaceplate: %hs\n",
                 console.renderer().glVersion().c_str(), console.renderer().glRenderer().c_str(), console.renderer().samples(),
                 console.renderer().maxSamples(), console.faceplateInfo().c_str());
    std::string plateErr;
    const auto art = ui::loadFaceplateArt(ui::resolveFaceplate(assets, {}), &plateErr);
    if (!art) { std::wprintf(L"faceplate reload failed: %hs\n", plateErr.c_str()); return 2; }

    int failures = 0;
    for (const Size sz : sizes) {
        if (!console.resize(sz.w, sz.h, &err)) { std::wprintf(L"resize failed: %hs\n", err.c_str()); return 2; }
        const app::HostStatus& st = host.status();
        ui::ViewState vs;
        vs.deviceName   = st.deviceName;
        vs.deviceFormat = st.driver + "  \xC2\xB7  " + std::to_string(st.blockFrames) + " SMP  \xC2\xB7  13 STEMS  \xC2\xB7  TEST TONES";
        vs.running      = st.running;
        vs.alert        = st.alert;
        vs.tonesOn      = true;

        constexpr int kFrames = 12;
        double cpuMs = 0, totalMs = 0, recordMs = 0;
        for (int f = 0; f < kFrames; ++f) {
            pump(0.05);   // keep the meters live between frames
            vs.status = st.line1 + "   \xC2\xB7   faceplate " + console.faceplateInfo();
            vs.engine = {st.running, st.driver, st.sampleRate, st.blockFrames, st.outLatencyMs, st.loadAvg * 100.0f, st.loadPeak * 100.0f, st.dropouts};
            const auto t0 = std::chrono::steady_clock::now();
            console.renderToCapture(vs, meters);
            const auto t1 = std::chrono::steady_clock::now();
            ::glFinish();
            const auto t2 = std::chrono::steady_clock::now();
            if (f > 0) {   // frame 0 also builds the static layer
                recordMs += console.timings().recordMs;
                cpuMs   += std::chrono::duration<double, std::milli>(t1 - t0).count();
                totalMs += std::chrono::duration<double, std::milli>(t2 - t0).count();
            }
        }
        const ui::Image shot = console.capture();
        const std::string base = "HybridCore-" + std::to_string(sz.w) + "x" + std::to_string(sz.h);
        ui::writePng(out / (base + ".png"), shot);
        ui::writePng(out / (base + "-vs-faceplate.png"), sideBySide(art->master, shot, std::min<std::uint32_t>(1080, static_cast<std::uint32_t>(sz.h))));

        // Chassis fidelity: where nothing is drawn on top, the screen must BE the faceplate.
        const ui::Canvas& cv = console.canvas();
        const auto refW = static_cast<std::uint32_t>(std::lround(ui::plate::kWidth * cv.scale));
        const auto refH = static_cast<std::uint32_t>(std::lround(ui::plate::kHeight * cv.scale));
        const ui::Image ref = (refW == art->master.width) ? art->master : ui::resizeSrgb(art->master, refW, refH);
        auto S = [&](float v) { return static_cast<int>(std::lround(v * cv.scale)); };
        const double plateDiff = meanAbsDiff(shot, ref, S(1150), S(60), S(2400), S(175));          // top bar, between title and badge
        const double frameDiff = meanAbsDiff(shot, ref, 0, S(250), S(56), S(880));               // left neon frame (above the band)
        const double meterLit  = static_cast<double>(litPixels(shot, S(700), S(1500), S(3800), S(1935 + cv.extraH)));
        const double display   = regionStdDev(shot, S(2660), S(265), S(3700), S(430));
        // 1:1 (4K) must be exact; scaled windows differ only by filtering (GPU trilinear vs CPU resample).
        const double tol = cv.scale == 1.0f ? 0.5 : 8.0;
        const bool ok = plateDiff < tol && frameDiff < tol && meterLit > 200 && display > 12.0 && ::glGetError() == GL_NO_ERROR;
        failures += ok ? 0 : 1;
        const auto& rs = console.renderer().stats();
        std::wprintf(L"\n%dx%d  canvas scale %.4f  stretch +%.0f  chassis texture %u px\n"
                     L"  frame: UI code %.2f ms CPU (layout + vertex recording)  with GL driver %.1f ms  until GPU done %.1f ms  static layer %.1f ms (once)\n"
                     L"  draw calls %u  quads %u  emissive quads %u  textures %.1f MB  render targets %.1f MB\n"
                     L"  faceplate match: top bar diff %.2f/255  frame diff %.2f/255 (limit %.1f)   meters lit px %.0f   display contrast %.1f  -> %ls\n"
                     L"  saved %hs.png and %hs-vs-faceplate.png\n",
                     sz.w, sz.h, cv.scale, cv.extraH, console.chassisTextureWidth(), recordMs / (kFrames - 1), cpuMs / (kFrames - 1), totalMs / (kFrames - 1),
                     console.timings().staticMs, rs.drawCalls, rs.quads, rs.bloomQuads, console.renderer().textureBytes() / 1048576.0,
                     console.renderer().targetBytes() / 1048576.0, plateDiff, frameDiff, tol, meterLit, display, ok ? L"PASS" : L"FAIL",
                     base.c_str(), base.c_str());
    }
    const app::HostStatus& st = host.status();
    std::wprintf(L"\nEngine after the shots: %hs\n  %hs\n", st.line1.c_str(), st.line2.c_str());
    for (std::uint32_t s = 0; s < kStemCount; ++s)
        std::wprintf(L"  stem %2u meter %6.1f dBFS%ls", s + 1, meters.stem[s] > 0 ? 20.0 * std::log10(meters.stem[s]) : -144.0, (s % 4 == 3) ? L"\n" : L"");
    std::wprintf(L"\n  main bus L %.1f / R %.1f dBFS\n", meters.busL[0] > 0 ? 20.0 * std::log10(meters.busL[0]) : -144.0,
                 meters.busR[0] > 0 ? 20.0 * std::log10(meters.busR[0]) : -144.0);
    host.close();
    std::wprintf(failures ? L"\nUI SNAPSHOT FAIL\n" : L"\nUI SNAPSHOT PASS\n");
    return failures ? 3 : 0;
}

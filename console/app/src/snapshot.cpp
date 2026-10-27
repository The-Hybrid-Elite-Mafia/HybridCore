// hem_console_snapshot — renders every HEM DIGITAL CONSOLE faceplate off-screen with the real screen code
// (ConsoleScreen, the same as HEMDigitalConsole.exe) and the live rig, and saves PNGs. Used by CI (Mesa software
// OpenGL) and works on the rig.
//
//   hem_console_snapshot [--out <folder>] [--sizes 3840x2160,1389x868] [--stems <folder>] [--seconds 6]
//
// Audio: the files in --stems (else 13 generated tones) run through the whole rig for --seconds on the offline
// clock before the shots, so meters, LEDs and the loudness numbers show real measured values. A few controls are
// set first, the way a user would, so lit and unlit states both show.
// Checks (exit 3 on failure): every page renders, its plate is routed, the image is not blank.
#include "hybrid/console/ColourStage.hpp"
#include "hybrid/console/ConsolePlayer.hpp"
#include "hybrid/console/StepEq.hpp"
#include "hybrid/console/TwinBus.hpp"
#include "hybrid/console/ui/ConsoleScreen.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/media/StemLoader.hpp"
#include "hybrid/ui/Gl.hpp"
#include "hybrid/ui/GlWindow.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

using namespace hc;
using namespace hc::console;
namespace U = hc::console::ui;
namespace fs = std::filesystem;

namespace {
struct Size { int w, h; };
fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}
double meanLuma(const hc::ui::Image& img) {
    double s = 0;
    for (std::size_t i = 0; i < img.rgba.size(); i += 4) s += 0.2126 * img.rgba[i] + 0.7152 * img.rgba[i + 1] + 0.0722 * img.rgba[i + 2];
    return s / static_cast<double>(std::max<std::size_t>(1, img.rgba.size() / 4));
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    io::configureWideConsole();
    fs::path out = "shots", stemsDir;
    std::vector<Size> sizes = {{3840, 2160}, {1389, 868}};
    double seconds = 6.0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--out" && i + 1 < argc) out = argv[++i];
        else if (k == L"--stems" && i + 1 < argc) stemsDir = argv[++i];
        else if (k == L"--seconds" && i + 1 < argc) seconds = std::wcstod(argv[++i], nullptr);
        else if (k == L"--sizes" && i + 1 < argc) {
            sizes.clear();
            const std::wstring s = argv[++i];
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
            std::wprintf(L"usage: hem_console_snapshot [--out <folder>] [--sizes 3840x2160,1389x868] [--stems <folder>] [--seconds 6]\n");
            return 1;
        }
    }
    std::error_code ec;
    fs::create_directories(out, ec);

    // ---- the rig, a user's settings, real audio through it ----
    auto rig = std::make_unique<ConsoleRig>();
    rig->prepare(48000.0, 512);
    U::AppState app;
    std::vector<std::unique_ptr<engine::StemAudio>> stems;
    if (!stemsDir.empty()) {
        for (const auto& f : media::collectAudioFiles({stemsDir})) {
            if (stems.size() >= 13) break;
            auto s = media::buildStem(f, 48000);
            if (!s) { std::printf("stem %s: %s\n", f.filename().string().c_str(), s.error().c_str()); continue; }
            app.stems[stems.size()].name = f.stem().string();
            app.stems[stems.size()].info = media::describeStem(**s);
            stems.push_back(std::move(*s));
        }
    }
    const auto frames = static_cast<std::uint64_t>(seconds * 48000.0);
    std::vector<std::vector<float>> tones;
    if (stems.empty()) {   // 13 generated parts: a low drone up to a high shimmer, each with its own movement
        for (int c = 0; c < 13; ++c) {
            std::vector<float> t(frames);
            const double hz = 55.0 * std::pow(2.0, c * 5.0 / 12.0);
            for (std::uint64_t i = 0; i < frames; ++i) {
                const double tt = static_cast<double>(i) / 48000.0;
                t[i] = static_cast<float>(0.12 * std::sin(2 * std::numbers::pi * hz * tt) * (0.55 + 0.45 * std::sin(2 * std::numbers::pi * (0.3 + 0.07 * c) * tt)));
            }
            tones.push_back(std::move(t));
            app.stems[static_cast<std::size_t>(c)].name = "Tone " + std::to_string(c + 1);
            app.stems[static_cast<std::size_t>(c)].info = "TEST TONE";
        }
    }
    // controls set like a user would
    auto& d = rig->desk();
    d.setChannel(0, StudioDesk::EqIn, 1); d.setChannel(0, StudioDesk::HfGain, 4); d.setChannel(0, StudioDesk::LfGain, -3); d.setChannel(0, StudioDesk::Hpf, 1);
    d.setChannel(1, StudioDesk::CompIn, 1); d.setChannel(1, StudioDesk::CompThreshold, -12);
    d.setChannel(2, StudioDesk::Fader, -8); d.setChannel(3, StudioDesk::MuteBusB, 1); d.setChannel(4, StudioDesk::Pan, -0.5f);
    d.setChannel(5, StudioDesk::Insert, 1); rig->channelTool(5).setType(ToolSlot::Shuffle); rig->channelTool(5).tool(ToolSlot::Shuffle).set(shuffle_ids::Mud, 4);
    d.setChannel(6, StudioDesk::Insert, 1); rig->channelTool(6).setType(ToolSlot::Echo); rig->channelTool(6).tool(ToolSlot::Echo).set(echo_ids::Mix, 20);
    rig->channelTool(0).setType(ToolSlot::Reso);
    rig->channelTool(0).tool(ToolSlot::Reso).set(reso_ids::id(0, reso_ids::On), 1); rig->channelTool(0).tool(ToolSlot::Reso).set(reso_ids::id(0, reso_ids::Depth), 6);
    d.setMaster(StudioDesk::BusCompIn, 1); d.setMaster(StudioDesk::BusCompThreshold, -6); d.setMaster(StudioDesk::BusCompAutoRelease, 1);
    d.setMaster(StudioDesk::MainInsert, 1);
    rig->stepEq().set(stepeq_ids::id(0, stepeq_ids::HiGain), 6); rig->stepEq().set(stepeq_ids::id(0, stepeq_ids::HiShelf), 1);
    rig->stepEq().set(stepeq_ids::id(1, stepeq_ids::HiGain), 6); rig->stepEq().set(stepeq_ids::id(1, stepeq_ids::HiShelf), 1);
    rig->twinBus().set(twinbus_ids::id(0, twinbus_ids::Threshold), 8); rig->twinBus().set(twinbus_ids::id(0, twinbus_ids::LfOn), 1);
    rig->colour().set(colour_ids::DriveIn, 1); rig->colour().set(colour_ids::Transformer, 1);
    {
        ConsoleRig::Io io;
        std::vector<float> mL(frames), mR(frames), oL(frames), oR(frames);
        for (std::size_t c = 0; c < 13; ++c) {
            if (c < stems.size()) {
                const auto n = std::min<std::uint64_t>(frames, stems[c]->frames);
                tones.emplace_back(stems[c]->left.data(), stems[c]->left.data() + n); tones.back().resize(frames, 0.0f);
                tones.emplace_back(stems[c]->right.data(), stems[c]->right.data() + n); tones.back().resize(frames, 0.0f);
                io.stemL[c] = tones[tones.size() - 2].data(); io.stemR[c] = tones.back().data();
            } else if (stems.empty()) { io.stemL[c] = tones[c].data(); io.stemR[c] = tones[c].data(); }
        }
        io.mainL = mL.data(); io.mainR = mR.data(); io.monL = oL.data(); io.monR = oR.data();
        const auto t0 = std::chrono::steady_clock::now();
        rig->process(io, static_cast<std::uint32_t>(frames));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("audio: %.1f s of %s through the whole rig in %.0f ms; main out %.1f LUFS integrated, %.1f dBTP\n", seconds,
                    stems.empty() ? "13 generated parts" : "the stems", ms, rig->meter().integratedLufs(), std::max(rig->meter().truePeakDbtp(0), rig->meter().truePeakDbtp(1)));
    }
    app.playing = true; app.position = seconds; app.length = 222.0;
    app.device = "SNAPSHOT (NO SOUND CARD)";
    app.dspLoad = 0.0f;
    app.session = "CI";

    // ---- GPU ----
    hc::ui::GlWindow window;
    std::string err;
    if (!window.create(L"HEM Digital Console snapshot", 64, 64, false, &err)) { std::printf("GL window: %s\n", err.c_str()); return 2; }
    std::printf("OpenGL: %s / %s\n", reinterpret_cast<const char*>(::glGetString(GL_VERSION)), reinterpret_cast<const char*>(::glGetString(GL_RENDERER)));
    U::ConsoleScreen screen(*rig, app, [](const U::ActionEvent&) {});
    if (!screen.init(exeDir() / "assets", 8, &err)) { std::printf("init: %s\n", err.c_str()); return 2; }
    int failures = 0;
    for (const Size& s : sizes) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!screen.resize(s.w, s.h, &err)) { std::printf("resize %dx%d: %s\n", s.w, s.h, err.c_str()); return 2; }
        for (int p = 0; p < U::kPageCount; ++p) {
            screen.setPage(static_cast<U::PageId>(p));
            if (!screen.waitForPlate(120)) { std::printf("FAIL plate not routed: page %d\n", p); ++failures; continue; }
            const auto f0 = std::chrono::steady_clock::now();
            screen.renderToCapture();
            const hc::ui::Image img = screen.capture();
            const double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f0).count();
            const GLenum glErr = ::glGetError();
            char name[96];
            std::snprintf(name, sizeof name, "HEM-Console-%d-%s-%dx%d.png", p + 1, U::pageName(static_cast<U::PageId>(p)), s.w, s.h);
            for (char* c = name; *c; ++c) if (*c == ' ') *c = '_';
            const bool ok = !img.empty() && hc::ui::writePng(out / name, img);
            const double luma = meanLuma(img);
            std::printf("%-44s %s  mean light %.1f/255, %u draw calls, %u quads, %.1f ms (incl. readback), GL error %u\n", name, ok ? "saved" : "FAILED", luma,
                        screen.renderer().stats().drawCalls, screen.renderer().stats().quads, frameMs, static_cast<unsigned>(glErr));
            if (!ok || luma < 4.0 || glErr != 0) ++failures;
        }
        std::printf("  %dx%d: all pages in %.1f s (plates routed on background threads)\n", s.w, s.h,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    }
    // a hovered knob shows its name and value (desk, laptop size)
    if (!sizes.empty()) {
        screen.resize(1389, 868, &err);
        screen.setPage(U::PageId::Desk);
        screen.waitForPlate(60);
        const auto& pg = screen.currentPage();
        for (const auto& w : pg.widgets)
            if (w.kind == U::Kind::Knob && w.ctl.info == &StudioDesk::channelParams()[StudioDesk::HfGain]) {
                screen.mouseMove(screen.canvas().toPixelX(w.r.cx()), screen.canvas().toPixelY(w.r.cy()), false);
                break;
            }
        screen.renderToCapture();
        hc::ui::writePng(out / "HEM-Console-1-DESK-hover-1389x868.png", screen.capture());
        std::printf("HEM-Console-1-DESK-hover-1389x868.png saved (HF knob of channel 1 under the mouse)\n");
    }
    screen.shutdown();
    std::printf("%s (%d failure(s))\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 3 : 0;
}

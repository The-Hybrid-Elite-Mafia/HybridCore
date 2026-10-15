// =============================================================================
//  HybridCore.exe — the standalone 13-stem console.
//  Threads: UI/message thread (this file, GPU rendering) and the audio thread
//  (owned by the WASAPI stream or the ASIO driver). They share only atomics and
//  wait-free rings; the audio thread never touches graphics, files or the UI.
// =============================================================================
#include "AudioHost.hpp"

#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/params/Settings.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/MixerView.hpp"
#include "hybrid/ui/Renderer.hpp"
#include "hybrid/ui/Theme.hpp"

#include "hybrid/ui/Gl.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>

using namespace hc;

namespace {

constexpr int kMinFrameMsWhenHidden = 50;

void fatal(const std::string& msg) {
    ::MessageBoxW(nullptr, app::fromUtf8(msg).c_str(), L"The Hybrid Core", MB_ICONERROR | MB_OK);
}

int findDevice(const std::vector<app::DeviceChoice>& list, const std::string& kind, const std::string& id) {
    for (std::size_t i = 0; i < list.size(); ++i) {
        const bool k = (kind == "asio") == (list[i].kind == app::DeviceChoice::Kind::Asio);
        if (k && app::toUtf8(list[i].id) == id) return static_cast<int>(i);
    }
    return 0;
}

// Command line (used by CI and for support): --exit-after <seconds> [--screenshot <file.png>]
struct Options {
    double                exitAfter = 0.0;
    std::filesystem::path screenshot;
};

Options parseOptions() {
    Options o;
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--exit-after" && i + 1 < argc) o.exitAfter = std::wcstod(argv[++i], nullptr);
        else if (k == L"--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
    }
    if (argv) ::LocalFree(argv);
    return o;
}

ui::Image readBackBuffer(int w, int h) {
    ui::Image img;
    img.width  = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    ::glReadBuffer(GL_BACK);
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    for (int y = 0; y < h / 2; ++y)
        std::swap_ranges(img.rgba.begin() + static_cast<std::ptrdiff_t>(y) * w * 4, img.rgba.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 4,
                         img.rgba.begin() + static_cast<std::ptrdiff_t>(h - 1 - y) * w * 4);
    for (std::size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
    return img;
}

struct App {
    params::Settings      settings;
    params::ParameterTree tree;
    app::AudioHost        audio;
    std::vector<app::DeviceChoice> devices;
    int                   deviceIndex = 0;
    ui::ThemeId           theme = ui::ThemeId::Pearl;
    std::string           plateInfo;

    void saveSettings() {
        settings.setSectionText("params", tree.serialize());
        settings.set("app", "theme", ui::palette(theme).fileStem);
        const auto& d = audio.current();
        settings.set("app", "device.kind", d.kind == app::DeviceChoice::Kind::Asio ? "asio" : "wasapi");
        settings.set("app", "device.id", app::toUtf8(d.id));
        settings.set("app", "source", audio.source() == engine::MixerProcessor::Source::TestTones ? "tones" : "inputs");
        settings.save(params::Settings::defaultFile());
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options opts = parseOptions();
    rt::configureProcessForAudio();
    io::ComApartment com(io::Apartment::SingleThreaded);   // ASIO drivers want an STA with a message pump

    const io::CpuReport cpu = io::probeCpu();
    if (!cpu.avx2 || !cpu.fma) {
        fatal("This CPU does not support AVX2/FMA. The Hybrid Core is built for the i7-14700HX.");
        return 2;
    }

    App a;
    a.settings.load(params::Settings::defaultFile());
    params::addMixerParameters(a.tree);
    a.tree.deserialize(a.settings.sectionText("params"));
    const params::MixerBindings bindings(a.tree);
    params::bindMixerParameters(a.tree, bindings, a.audio.matrix());
    a.tree.dispatchChanges();   // push every value into the matrix before audio starts

    a.audio.setSource(a.settings.get("app", "source", "tones") == "inputs" ? engine::MixerProcessor::Source::Hardware
                                                                           : engine::MixerProcessor::Source::TestTones);
    a.theme   = ui::themeFromName(a.settings.get("app", "theme", "pearl"));
    a.devices = app::AudioHost::listDevices();
    a.deviceIndex = findDevice(a.devices, a.settings.get("app", "device.kind", "wasapi"), a.settings.get("app", "device.id"));
    a.audio.open(a.devices[static_cast<std::size_t>(a.deviceIndex)]);

    ui::GlWindow window;
    std::string err;
    if (!window.create(L"The Hybrid Core", static_cast<int>(ui::kDesignWidth), static_cast<int>(ui::kDesignHeight), true, &err)) {
        fatal("Could not start the GPU display: " + err + "\nUpdate your graphics driver (OpenGL 3.3 is required).");
        return 3;
    }

    ui::Renderer renderer;
    if (!renderer.init(&err)) { fatal("GPU renderer failed: " + err); return 3; }

    const ui::Layout layout(a.tree);
    ui::Interaction interaction(layout, a.tree);
    const ui::MixerView view(layout, a.tree);
    ui::MeterState meters;

    const auto fontBytes = ui::FontAtlas::loadSystemFont();
    if (!fontBytes) { fatal("No usable system font found (Segoe UI / Arial)."); return 3; }
    ui::FontAtlas font;

    const std::filesystem::path dataDir = params::Settings::appDataDir();
    ui::Tier tier{};
    float    fontScale = 0.0f;

    auto loadPlate = [&](ui::Tier t) {
        a.plateInfo.clear();
        const ui::CompressedTexture tex = ui::loadFaceplate(a.theme, t, layout, dataDir, &a.plateInfo);
        std::string e;
        if (!renderer.setFaceplate(tex, &e)) a.plateInfo = "faceplate error: " + e;
        tier = t;
    };

    const auto started = std::chrono::steady_clock::now();
    bool shotTaken = false;
    auto lastFrame = started;
    auto lastSave  = lastFrame;
    std::uint64_t savedVersion = a.tree.version();

    while (window.pump()) {
        const int   fbW = window.pixelWidth(), fbH = window.pixelHeight();
        const auto  vp  = ui::Viewport::fit(static_cast<float>(fbW), static_cast<float>(fbH));

        // Faceplate tier follows the window size; text is re-baked when the scale moves.
        const ui::Tier want = ui::chooseTier(static_cast<std::uint32_t>(fbW), static_cast<std::uint32_t>(fbH));
        if (want.width != tier.width) loadPlate(want);
        if (std::fabs(vp.scale - fontScale) > 0.15f * std::max(fontScale, 0.01f)) {
            if (font.bake(*fontBytes, vp.scale)) renderer.setFont(font, &err);
            fontScale = vp.scale;
        }

        for (const auto& e : window.takeEvents()) {
            const float x = vp.toDesignX(e.x), y = vp.toDesignY(e.y);
            switch (e.type) {
            case ui::InputEvent::Type::MouseDown:
                switch (interaction.mouseDown(x, y, e.doubleClick, e.fine)) {
                case ui::HeaderAction::CycleDevice:
                    a.devices = app::AudioHost::listDevices();
                    a.deviceIndex = (a.deviceIndex + 1) % static_cast<int>(a.devices.size());
                    a.audio.open(a.devices[static_cast<std::size_t>(a.deviceIndex)]);
                    break;
                case ui::HeaderAction::ToggleSource:
                    a.audio.setSource(a.audio.source() == engine::MixerProcessor::Source::TestTones
                                          ? engine::MixerProcessor::Source::Hardware
                                          : engine::MixerProcessor::Source::TestTones);
                    break;
                case ui::HeaderAction::CycleTheme:
                    a.theme = ui::nextTheme(a.theme);
                    loadPlate(tier);
                    break;
                case ui::HeaderAction::None: break;
                }
                break;
            case ui::InputEvent::Type::MouseMove: interaction.mouseMove(x, y, e.fine); break;
            case ui::InputEvent::Type::MouseUp:   interaction.mouseUp(); break;
            case ui::InputEvent::Type::Wheel:     interaction.wheel(x, y, e.wheel, e.fine); break;
            case ui::InputEvent::Type::Resize:
            case ui::InputEvent::Type::Close:     break;
            }
        }

        a.tree.dispatchChanges();
        a.audio.poll();

        const auto  now = std::chrono::steady_clock::now();
        const float dt  = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        float sp[kStemCount], bl[kBusCount], br[kBusCount];
        for (std::uint32_t s = 0; s < kStemCount; ++s) sp[s] = a.audio.matrix().stemPeak(s);
        for (std::uint32_t b = 0; b < kBusCount; ++b) { bl[b] = a.audio.matrix().busPeak(b, 0); br[b] = a.audio.matrix().busPeak(b, 1); }
        if (!a.audio.status().running) { for (float& v : sp) v = 0; for (float& v : bl) v = 0; for (float& v : br) v = 0; }
        meters.update(sp, bl, br, std::min(dt, 0.1f));

        // Persist settings at most every 5 s, only when something changed (crash-safe file replace).
        if (now - lastSave > std::chrono::seconds(5)) {
            lastSave = now;
            if (a.tree.version() != savedVersion) { a.saveSettings(); savedVersion = a.tree.version(); }
        }

        if (window.minimized()) { ::Sleep(kMinFrameMsWhenHidden); continue; }

        ui::ViewState vs;
        vs.theme       = a.theme;
        vs.deviceLabel = a.audio.status().device;
        vs.sourceLabel = a.audio.source() == engine::MixerProcessor::Source::TestTones ? "TEST TONES" : "INPUTS";
        vs.status      = a.audio.status().line1;
        vs.detail      = a.audio.status().line2 + "  \xC2\xB7  " + a.plateInfo;
        vs.alert       = a.audio.status().alert;
        vs.hover       = interaction.hoverWidget();
        vs.active      = interaction.activeWidget();

        const ui::Palette& pal = ui::palette(a.theme);
        renderer.begin(fbW, fbH, vp, {pal.plateBottom.r * 0.6f, pal.plateBottom.g * 0.6f, pal.plateBottom.b * 0.6f, 1});
        view.draw(renderer, vs, meters);
        renderer.end();
        const double alive = std::chrono::duration<double>(now - started).count();
        if (!opts.screenshot.empty() && !shotTaken && alive >= std::max(1.0, opts.exitAfter - 1.0)) {
            ::glFinish();
            ui::writePng(opts.screenshot, readBackBuffer(fbW, fbH));
            shotTaken = true;
        }
        window.swap();   // vsync paces the UI; the audio thread is unaffected
        if (opts.exitAfter > 0.0 && alive >= opts.exitAfter) break;
    }

    a.audio.close();
    a.saveSettings();
    return 0;
}

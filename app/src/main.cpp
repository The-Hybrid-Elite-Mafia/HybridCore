// =============================================================================
//  HybridCore.exe — the standalone 13-stem console.
//  Threads: UI/message thread (this file, GPU rendering) and the audio thread
//  (owned by the WASAPI stream, the ASIO driver or the offline clock). They share only
//  atomics and wait-free rings; the audio thread never touches graphics, files or the UI.
// =============================================================================
#include "AudioHost.hpp"

#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/params/Settings.hpp"
#include "hybrid/ui/Console.hpp"
#include "hybrid/ui/GlWindow.hpp"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <string>

using namespace hc;

namespace {

constexpr int kMinFrameMsWhenHidden = 50;
constexpr int kMsaaSamples          = 8;

void fatal(const std::string& msg) {
    ::MessageBoxW(nullptr, app::fromUtf8(msg).c_str(), L"The Hybrid Core", MB_ICONERROR | MB_OK);
}

std::filesystem::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

const char* kindName(app::DeviceChoice::Kind k) {
    return k == app::DeviceChoice::Kind::Asio ? "asio" : k == app::DeviceChoice::Kind::Offline ? "offline" : "wasapi";
}

int findDevice(const std::vector<app::DeviceChoice>& list, const std::string& kind, const std::string& id) {
    for (std::size_t i = 0; i < list.size(); ++i)
        if (kind == kindName(list[i].kind) && (list[i].kind == app::DeviceChoice::Kind::Offline || app::toUtf8(list[i].id) == id))
            return static_cast<int>(i);
    return 0;
}

// Command line (CI and support): --exit-after <s> [--screenshot <file.png>] [--device offline]
struct Options {
    double                exitAfter = 0.0;
    std::filesystem::path screenshot;
    bool                  offline   = false;
};

Options parseOptions() {
    Options o;
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--exit-after" && i + 1 < argc) o.exitAfter = std::wcstod(argv[++i], nullptr);
        else if (k == L"--screenshot" && i + 1 < argc) o.screenshot = argv[++i];
        else if (k == L"--device" && i + 1 < argc) o.offline = std::wstring(argv[++i]) == L"offline";
    }
    if (argv) ::LocalFree(argv);
    return o;
}

struct App {
    params::Settings      settings;
    params::ParameterTree tree;
    app::AudioHost        audio;
    std::vector<app::DeviceChoice> devices;
    int                   deviceIndex = 0;

    void saveSettings(std::uint32_t bank) {
        settings.setSectionText("params", tree.serialize());
        const auto& d = audio.current();
        settings.set("app", "device.kind", kindName(d.kind));
        settings.set("app", "device.id", app::toUtf8(d.id));
        settings.set("app", "source", audio.source() == engine::MixerProcessor::Source::TestTones ? "tones" : "inputs");
        settings.set("app", "faderBank", std::to_string(bank));
        settings.save(params::Settings::defaultFile());
    }
    void selectDevice(int delta) {
        devices = app::AudioHost::listDevices();
        const int n = static_cast<int>(devices.size());
        deviceIndex = ((deviceIndex + delta) % n + n) % n;
        audio.open(devices[static_cast<std::size_t>(deviceIndex)]);
    }
    void clearAll(const char* field) {
        for (std::uint32_t s = 0; s < kStemCount; ++s) tree.find(params::stemParamId(s, field))->setPlain(0.0f);
    }
};

std::string formatLine(const app::HostStatus& st, bool tones) {
    char buf[200];
    if (!st.running) return tones ? "NO STREAM  \xC2\xB7  13 STEMS  \xC2\xB7  TEST TONES" : "NO STREAM  \xC2\xB7  13 STEMS  \xC2\xB7  HARDWARE INPUTS";
    std::snprintf(buf, sizeof buf, "%s  \xC2\xB7  %.1f kHz  \xC2\xB7  %u SMP  \xC2\xB7  13 STEMS  \xC2\xB7  %s", st.driver.c_str(), st.sampleRate / 1000.0,
                  st.blockFrames, tones ? "TEST TONES" : "HARDWARE INPUTS");
    return buf;
}

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
    a.devices = app::AudioHost::listDevices();
    a.deviceIndex = opts.offline ? static_cast<int>(a.devices.size()) - 1
                                 : findDevice(a.devices, a.settings.get("app", "device.kind", "wasapi"), a.settings.get("app", "device.id"));
    a.audio.open(a.devices[static_cast<std::size_t>(a.deviceIndex)]);

    ui::GlWindow window;
    std::string err;
    if (!window.create(L"The Hybrid Core", 1600, 900, true, &err)) {
        fatal("Could not start the GPU display: " + err + "\nUpdate your graphics driver (OpenGL 3.3 is required).");
        return 3;
    }

    ui::Console console(a.tree);
    const std::filesystem::path assets = exeDir() / "assets";
    if (!console.init(assets, params::Settings::appDataDir(), kMsaaSamples, &err)) {
        fatal("The console could not load its art: " + err + "\nReinstall The Hybrid Core (assets folder: " + assets.string() + ").");
        return 3;
    }
    console.setFaderBank(static_cast<std::uint32_t>(std::clamp(std::atoi(a.settings.get("app", "faderBank", "0").c_str()), 0, 3)));

    ui::MeterState meters;
    const auto started = std::chrono::steady_clock::now();
    bool shotTaken = false;
    auto lastFrame = started;
    auto lastSave  = lastFrame;
    std::uint64_t savedVersion = a.tree.version();
    std::uint32_t savedBank    = console.faderBank();

    while (window.pump()) {
        const int fbW = window.pixelWidth(), fbH = window.pixelHeight();
        if (!console.resize(fbW, fbH, &err)) { fatal("GPU error: " + err); break; }
        const ui::Canvas& cv = console.canvas();

        for (const auto& e : window.takeEvents()) {
            const float x = cv.toDesignX(e.x), y = cv.toDesignY(e.y);
            switch (e.type) {
            case ui::InputEvent::Type::MouseDown: {
                std::uint32_t idx = 0;
                switch (console.interaction().mouseDown(x, y, e.doubleClick, e.fine, &idx)) {
                case ui::UiAction::PrevDevice:   a.selectDevice(-1); break;
                case ui::UiAction::NextDevice:   a.selectDevice(+1); break;
                case ui::UiAction::SourceTones:  a.audio.setSource(engine::MixerProcessor::Source::TestTones); break;
                case ui::UiAction::SourceInputs: a.audio.setSource(engine::MixerProcessor::Source::Hardware); break;
                case ui::UiAction::ClearSolo:    a.clearAll("solo"); break;
                case ui::UiAction::ClearMute:    a.clearAll("mute"); break;
                case ui::UiAction::FaderBank:    console.setFaderBank(idx); break;
                case ui::UiAction::None:         break;
                }
                break;
            }
            case ui::InputEvent::Type::MouseMove: console.interaction().mouseMove(x, y, e.fine); break;
            case ui::InputEvent::Type::MouseUp:   console.interaction().mouseUp(); break;
            case ui::InputEvent::Type::Wheel:     console.interaction().wheel(x, y, e.wheel, e.fine); break;
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
            if (a.tree.version() != savedVersion || console.faderBank() != savedBank) {
                a.saveSettings(console.faderBank());
                savedVersion = a.tree.version();
                savedBank    = console.faderBank();
            }
        }

        if (window.minimized()) { ::Sleep(kMinFrameMsWhenHidden); continue; }

        const app::HostStatus& st = a.audio.status();
        const bool tones = a.audio.source() == engine::MixerProcessor::Source::TestTones;
        ui::ViewState vs;
        vs.deviceName   = st.deviceName.empty() ? "No device" : st.deviceName;
        vs.deviceFormat = formatLine(st, tones);
        vs.status       = (st.alert ? st.line2 : st.line1) + "   \xC2\xB7   faceplate " + console.faceplateInfo();
        vs.alert        = st.alert;
        vs.running      = st.running;
        vs.tonesOn      = tones;
        vs.engine.valid        = st.running;
        vs.engine.driver       = st.driver;
        vs.engine.sampleRate   = st.sampleRate;
        vs.engine.bufferFrames = st.blockFrames;
        vs.engine.latencyMs    = st.outLatencyMs;
        vs.engine.loadPct      = st.loadAvg * 100.0f;
        vs.engine.peakLoadPct  = st.loadPeak * 100.0f;
        vs.engine.dropouts     = st.dropouts;
        vs.hover   = console.interaction().hoverWidget();
        vs.active  = console.interaction().activeWidget();
        vs.pressed = console.interaction().pressedWidget();

        const double alive = std::chrono::duration<double>(now - started).count();
        if (!opts.screenshot.empty() && !shotTaken && alive >= std::max(1.0, opts.exitAfter - 1.0)) {
            console.renderToCapture(vs, meters);
            ui::writePng(opts.screenshot, console.capture());
            shotTaken = true;
        }
        console.render(vs, meters, 0);
        window.swap();   // vsync paces the UI; the audio thread is unaffected
        if (opts.exitAfter > 0.0 && alive >= opts.exitAfter) break;
    }

    a.audio.close();
    a.saveSettings(console.faderBank());
    return 0;
}

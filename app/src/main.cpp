// =============================================================================
//  HybridCore.exe — the standalone 13-stem console.
//  Threads: UI/message thread (this file, GPU rendering) and the audio thread
//  (owned by the WASAPI stream, the ASIO driver or the offline clock). They share only
//  atomics and wait-free rings; the audio thread never touches graphics, files or the UI.
// =============================================================================
#include "AudioHost.hpp"
#include "StemDeck.hpp"

#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/params/Settings.hpp"
#include "hybrid/ui/Console.hpp"
#include "hybrid/ui/GlWindow.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

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

using Source = engine::MixerProcessor::Source;

const char* sourceName(Source s) { return s == Source::TestTones ? "tones" : s == Source::Stems ? "stems" : "inputs"; }
Source sourceFromName(const std::string& n) { return n == "inputs" ? Source::Hardware : n == "stems" ? Source::Stems : Source::TestTones; }
std::string slotKey(std::uint32_t s) { char b[16]; std::snprintf(b, sizeof b, "slot%02u", s + 1); return b; }

// Windows file dialog, multi-select, WAV/MP3/FLAC. Empty when cancelled.
std::vector<std::filesystem::path> askForStems(HWND owner) {
    std::vector<std::filesystem::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filters[] = {{L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(L"Load stems (up to 13, in name order)");
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out.emplace_back(path);
                    ::CoTaskMemFree(path);
                }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

struct App {
    params::Settings      settings;
    params::ParameterTree tree;
    app::AudioHost        audio;
    app::StemDeck         stems{audio};
    std::vector<app::DeviceChoice> devices;
    int                   deviceIndex = 0;

    // uiState = fader bank + 4 × knob page.
    void saveSettings(std::uint32_t uiState) {
        const std::uint32_t bank = uiState % 4, page = uiState / 4;
        settings.setSectionText("params", tree.serialize());
        const auto& d = audio.current();
        settings.set("app", "device.kind", kindName(d.kind));
        settings.set("app", "device.id", app::toUtf8(d.id));
        settings.set("app", "source", sourceName(audio.source()));
        settings.set("app", "faderBank", std::to_string(bank));
        settings.set("app", "page", std::to_string(page));
        settings.set("app", "loop", audio.loop() ? "1" : "0");
        for (std::uint32_t s = 0; s < kStemCount; ++s) settings.set("stems", slotKey(s), app::toUtf8(stems.slot(s).file.wstring()));
        settings.save(params::Settings::defaultFile());
    }
    void restoreStems() {
        std::error_code ec;
        for (std::uint32_t s = 0; s < kStemCount; ++s) {
            const std::string p = settings.get("stems", slotKey(s));
            if (p.empty()) continue;
            const std::filesystem::path f(app::fromUtf8(p));
            if (std::filesystem::is_regular_file(f, ec)) stems.load(s, {f});
        }
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
    // Transport. Any transport command switches the source to the loaded stems.
    void playPause() {
        if (audio.source() != Source::Stems) audio.setSource(Source::Stems);
        if (audio.wantsPlay()) audio.pause(); else audio.play();
    }
    void seekSeconds(double delta) {
        const double song = static_cast<double>(stems.songFrames());
        if (song <= 0) return;
        const double at = std::clamp(static_cast<double>(audio.stemPlayhead()) + delta * kSampleRate, 0.0, song - 1.0);
        audio.seek(static_cast<std::uint64_t>(at));
    }
};

std::string sourceLine(Source src, const app::StemDeck& deck) {
    if (src == Source::TestTones) return "TEST TONES";
    if (src == Source::Hardware) return "HARDWARE INPUTS";
    char b[96];
    std::snprintf(b, sizeof b, "STEMS %u/13  (%.0f MB RAM)", deck.loadedCount(), static_cast<double>(deck.loadedBytes()) / 1048576.0);
    return b;
}

std::string formatLine(const app::HostStatus& st, const std::string& source) {
    char buf[240];
    if (!st.running) return "NO STREAM  \xC2\xB7  " + source;
    std::snprintf(buf, sizeof buf, "%s  \xC2\xB7  %.1f kHz  \xC2\xB7  %u SMP  \xC2\xB7  %s", st.driver.c_str(), st.sampleRate / 1000.0,
                  st.blockFrames, source.c_str());
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
    params::bindMixerParameters(a.tree, bindings, a.audio.matrix(), &a.audio.limiter());
    a.tree.dispatchChanges();   // push every value into the matrix before audio starts

    a.restoreStems();   // background loads; the saved source below wins over the "switch to stems" a load does
    a.audio.setSource(sourceFromName(a.settings.get("app", "source", "tones")));
    a.audio.setLoop(a.settings.get("app", "loop", "1") != "0");
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
    console.setPage(static_cast<ui::Page>(std::clamp(std::atoi(a.settings.get("app", "page", "0").c_str()), 0, 2)));
    auto uiState = [&console] { return console.faderBank() + 4u * static_cast<std::uint32_t>(console.page()); };

    ui::MeterState meters;
    const auto started = std::chrono::steady_clock::now();
    bool shotTaken = false;
    auto lastFrame = started;
    auto lastSave  = lastFrame;
    std::uint64_t savedVersion = a.tree.version();
    std::uint32_t savedBank    = uiState();
    bool          savedLoop    = a.audio.loop();

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
                case ui::UiAction::LoadStems: {
                    const auto files = askForStems(static_cast<HWND>(window.nativeHandle()));
                    if (!files.empty()) a.stems.replaceAll(files);
                    console.interaction().mouseUp();
                    break;
                }
                case ui::UiAction::CycleSource: {
                    const Source s = a.audio.source();
                    a.audio.setSource(s == Source::TestTones ? Source::Hardware : s == Source::Hardware ? Source::Stems : Source::TestTones);
                    break;
                }
                case ui::UiAction::ClearSolo:    a.clearAll("solo"); break;
                case ui::UiAction::ClearMute:    a.clearAll("mute"); break;
                case ui::UiAction::FaderBank:    console.setFaderBank((console.faderBank() + 1) % ui::kFaderBanks); break;
                case ui::UiAction::SelectPage:   console.setPage(static_cast<ui::Page>(idx)); break;
                case ui::UiAction::ToStart:      a.audio.seek(0); break;
                case ui::UiAction::PlayPause:    a.playPause(); break;
                case ui::UiAction::Stop:         a.audio.stop(); break;
                case ui::UiAction::ToggleLoop:   a.audio.setLoop(!a.audio.loop()); break;
                case ui::UiAction::Seek:         break;   // collected below with takeSeek
                case ui::UiAction::None:         break;
                }
                break;
            }
            case ui::InputEvent::Type::Key:
                switch (e.key) {
                case VK_SPACE:  a.playPause(); break;
                case VK_HOME:   a.audio.seek(0); break;
                case VK_END:    a.audio.stop(); break;
                case 'L':       a.audio.setLoop(!a.audio.loop()); break;
                case VK_LEFT:   a.seekSeconds(e.ctrl ? -30.0 : -5.0); break;
                case VK_RIGHT:  a.seekSeconds(e.ctrl ? 30.0 : 5.0); break;
                default:        break;
                }
                break;
            case ui::InputEvent::Type::MouseMove: console.interaction().mouseMove(x, y, e.fine); break;
            case ui::InputEvent::Type::MouseUp:   console.interaction().mouseUp(); break;
            case ui::InputEvent::Type::Wheel:     console.interaction().wheel(x, y, e.wheel, e.fine); break;
            case ui::InputEvent::Type::Drop: {   // onto a stem strip: from that slot; elsewhere: first empty slot
                const int at = console.layout().stemStripAt(x, y);
                const int first = at >= 0 ? at : std::max(0, a.stems.firstEmptySlot());
                a.stems.load(static_cast<std::uint32_t>(first), e.files);
                break;
            }
            case ui::InputEvent::Type::RightDown: {
                // Right-click: stem title = empty that slot; name panel = previous device; any S / M = clear all solos / mutes.
                const int hit = console.layout().hitTest(x, y);
                const ui::Widget* w = hit >= 0 ? &console.layout().widgets()[static_cast<std::size_t>(hit)] : nullptr;
                const int at = console.layout().stemStripAt(x, y);
                if (w && w->kind == ui::WidgetKind::NamePanel) a.selectDevice(-1);
                else if (w && w->kind == ui::WidgetKind::Button && w->caption == "S") a.clearAll("solo");
                else if (w && w->kind == ui::WidgetKind::Button && w->caption == "M" && at >= 0) a.clearAll("mute");
                else if (at >= 0 && ui::Layout::titleArea(console.layout().strips()[static_cast<std::size_t>(at)]).contains(x, y))
                    a.stems.clear(static_cast<std::uint32_t>(at));
                break;
            }
            case ui::InputEvent::Type::Resize:
            case ui::InputEvent::Type::Close:     break;
            }
        }

        if (float f = 0; console.interaction().takeSeek(f) && a.stems.songFrames() > 0) {
            if (a.audio.source() != Source::Stems) a.audio.setSource(Source::Stems);
            a.audio.seek(static_cast<std::uint64_t>(f * static_cast<double>(a.stems.songFrames() - 1)));
        }
        a.tree.dispatchChanges();
        a.audio.poll();
        a.stems.pump();

        const auto  now = std::chrono::steady_clock::now();
        const float dt  = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;

        float sp[kStemCount], bl[kBusCount], br[kBusCount];
        for (std::uint32_t s = 0; s < kStemCount; ++s) sp[s] = a.audio.matrix().stemPeak(s);
        for (std::uint32_t b = 0; b < kBusCount; ++b) { bl[b] = a.audio.matrix().busPeak(b, 0); br[b] = a.audio.matrix().busPeak(b, 1); }
        bl[0] = a.audio.limiter().mainPeak(0);   // Main is metered after the limiter (what reaches the speakers)
        br[0] = a.audio.limiter().mainPeak(1);
        if (!a.audio.status().running) { for (float& v : sp) v = 0; for (float& v : bl) v = 0; for (float& v : br) v = 0; }
        meters.update(sp, bl, br, std::min(dt, 0.1f));

        // Persist settings at most every 5 s, only when something changed (crash-safe file replace).
        if (now - lastSave > std::chrono::seconds(5)) {
            lastSave = now;
            if (a.tree.version() != savedVersion || uiState() != savedBank || a.audio.loop() != savedLoop || a.stems.loading()) {
                a.saveSettings(uiState());
                savedVersion = a.tree.version();
                savedBank    = uiState();
                savedLoop    = a.audio.loop();
            }
        }

        if (window.minimized()) { ::Sleep(kMinFrameMsWhenHidden); continue; }

        const app::HostStatus& st = a.audio.status();
        const Source src = a.audio.source();
        ui::ViewState vs;
        vs.deviceName   = st.deviceName.empty() ? "No device" : st.deviceName;
        vs.deviceFormat = formatLine(st, sourceLine(src, a.stems));
        const std::string notice = a.stems.notice();
        vs.status       = !notice.empty() ? notice : (st.alert ? st.line2 : st.line1) + "   \xC2\xB7   faceplate " + console.faceplateInfo();
        vs.alert        = st.alert;
        vs.running      = st.running;
        vs.source       = src == Source::TestTones ? ui::SourceView::Tones : src == Source::Stems ? ui::SourceView::Stems : ui::SourceView::Inputs;
        vs.loading      = a.stems.loading();
        for (std::uint32_t s = 0; s < kStemCount; ++s) {
            const app::StemSlot& sl = a.stems.slot(s);
            vs.slots[s].name     = sl.name;
            vs.slots[s].info     = sl.info;
            vs.slots[s].progress = a.stems.progress(s);
            vs.slots[s].failed   = !sl.error.empty();
        }
        vs.engine.valid        = st.running;
        vs.engine.driver       = st.driver;
        vs.engine.sampleRate   = st.sampleRate;
        vs.engine.bufferFrames = st.blockFrames;
        vs.engine.latencyMs    = st.outLatencyMs;
        vs.engine.loadPct      = st.loadAvg * 100.0f;
        vs.engine.peakLoadPct  = st.loadPeak * 100.0f;
        vs.engine.dropouts     = st.dropouts;
        vs.transport.playing   = a.audio.wantsPlay();
        vs.transport.loop      = a.audio.loop();
        vs.transport.position  = static_cast<double>(a.audio.stemPlayhead()) / kSampleRate;
        vs.transport.length    = static_cast<double>(a.stems.songFrames()) / kSampleRate;
        vs.overview            = a.stems.songOverview().empty() ? nullptr : &a.stems.songOverview();
        for (std::uint32_t s = 0; s < kStemCount; ++s) vs.gainReduction[s] = st.running ? a.audio.matrix().strip(s).gainReductionDb() : 0.0f;
        vs.limiterGr           = st.running ? a.audio.limiter().gainReductionDb() : 0.0f;
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
    a.saveSettings(uiState());
    return 0;
}

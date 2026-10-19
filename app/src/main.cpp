// =============================================================================
//  HybridCore.exe — the standalone 13-stem console.
//  Threads: UI/message thread (this file, GPU rendering) and the audio thread
//  (owned by the WASAPI stream, the ASIO driver or the offline clock). They share only
//  atomics and wait-free rings; the audio thread never touches graphics, files or the UI.
// =============================================================================
#include "AudioHost.hpp"
#include "StemDeck.hpp"
#include "ForgeController.hpp"
#include "RemakeJob.hpp"

#include "hybrid/analysis/Analyzer.hpp"
#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/forge/Forge.hpp"
#include "hybrid/media/MixExporter.hpp"
#include "hybrid/media/Session.hpp"
#include "hybrid/params/MixerParams.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/params/Settings.hpp"
#include "hybrid/ui/Console.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/ScopeView.hpp"
#include "hybrid/ui/FxRackView.hpp"
#include "hybrid/fx/Effects.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
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
    std::filesystem::path session;   // a .hcs given on the command line (double-click in Explorer)
    bool                  fxDemo    = false;   // CI: open the FX RACK with two effects on strip 1
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
        else if (k == L"--fx-demo") o.fxDemo = true;
        else if (!k.empty() && k[0] != L'-') o.session = k;
    }
    if (argv) ::LocalFree(argv);
    return o;
}

using Source = engine::MixerProcessor::Source;

const char* sourceName(Source s) { return s == Source::TestTones ? "tones" : s == Source::Stems ? "stems" : "inputs"; }
Source sourceFromName(const std::string& n) { return n == "inputs" ? Source::Hardware : n == "stems" ? Source::Stems : Source::TestTones; }
std::string slotKey(std::uint32_t s) { char b[16]; std::snprintf(b, sizeof b, "slot%02u", s + 1); return b; }

// Windows file dialog, multi-select, WAV/MP3/FLAC. Empty when cancelled.
std::vector<std::filesystem::path> askForStems(HWND owner, const wchar_t* title = L"Load stems (up to 13, in name order)") {
    std::vector<std::filesystem::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filters[] = {{L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetTitle(title);
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

// Windows save dialog with a "Save as type" list. *type (1-based) picks the starting entry and
// returns the one chosen. Empty when cancelled.
std::filesystem::path askSaveFileTyped(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters, UINT count, UINT* type,
                                       const wchar_t* ext, const std::filesystem::path& suggested) {
    std::filesystem::path out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    dlg->SetFileTypes(count, filters);
    if (type && *type >= 1 && *type <= count) dlg->SetFileTypeIndex(*type);
    dlg->SetDefaultExtension(ext);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    if (!suggested.filename().empty()) dlg->SetFileName(suggested.filename().c_str());
    std::error_code ec;
    if (!suggested.parent_path().empty() && std::filesystem::is_directory(suggested.parent_path(), ec)) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(suggested.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            ::CoTaskMemFree(path);
        }
        if (it) it->Release();
        if (type && !out.empty()) dlg->GetFileTypeIndex(type);
    }
    dlg->Release();
    return out;
}

std::filesystem::path askSaveFile(HWND owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* pattern, const wchar_t* ext,
                                  const std::filesystem::path& suggested) {
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}};
    return askSaveFileTyped(owner, title, filters, 1, nullptr, ext, suggested);
}

std::filesystem::path askOpenSession(HWND owner, const std::filesystem::path& folder) {
    std::filesystem::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filters[] = {{L"Sessions and audio files (.hcs, .wav, .mp3, .flac)", L"*.hcs;*.wav;*.mp3;*.flac"},
                                         {L"Hybrid Core session (.hcs)", L"*.hcs"},
                                         {L"Audio file (.wav, .mp3, .flac)", L"*.wav;*.mp3;*.flac"}};
    dlg->SetFileTypes(3, filters);
    dlg->SetTitle(L"Open a session or an audio file");
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    std::error_code ec;
    if (!folder.empty() && std::filesystem::is_directory(folder, ec)) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            out = path;
            ::CoTaskMemFree(path);
        }
        if (it) it->Release();
    }
    dlg->Release();
    return out;
}

std::string fmtTime(double seconds) {
    const int s = static_cast<int>(seconds + 0.5);
    char b[32];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

struct App {
    params::Settings      settings;
    params::ParameterTree tree;
    app::AudioHost        audio;
    app::StemDeck         stems{audio};
    std::vector<app::DeviceChoice> devices;
    int                   deviceIndex = 0;
    std::unique_ptr<app::ForgeController> forge;   // Beat Forge (created once the tree is bound)
    app::RemakeJob        remakeJob;        // REMAKE: one of your beats back into Beat Forge
    int                   remakeShown = -1; // last progress % shown
    std::filesystem::path session;          // current .hcs ("" = not saved yet)
    std::uint64_t         pendingSeek = UINT64_MAX;   // applied once the stems of an opened session are loaded
    media::MixExporter    exporter;
    std::string           notice;
    bool                  lastExportMastered = true;
    std::chrono::steady_clock::time_point noticeUntil{};
    // ANALYZER
    analysis::Analyzer    analyzer{static_cast<double>(kSampleRate)};
    std::vector<analysis::StereoFrame> tapBuf = std::vector<analysis::StereoFrame>(8192);
    bool                  tapOn = false, wasPlaying = false;
    std::string           scopeSource;      // file opened with OPEN ("" = the console's Main out)
    ui::ScopeCamera       camera;
    // RESET: press once = "SURE?", again within 4 s = every knob and fader back to its start
    // position (stems, the beat and its tempo / genre / key stay); then UNDO for 15 s.
    int                   resetStage = 0;
    std::chrono::steady_clock::time_point resetUntil{};
    std::string           resetBackup;
    void resetPressed() {
        const auto now = std::chrono::steady_clock::now();
        if (resetStage != 0 && now > resetUntil) resetStage = 0;
        if (resetStage == 0) {
            resetStage = 1;
            resetUntil = now + std::chrono::seconds(4);
            say("Press RESET again to put every knob and fader back (stems and beat stay)");
        } else if (resetStage == 1) {
            resetBackup = tree.serialize();
            const std::size_t n = tree.resetToDefaults([](const std::string& id) { return forge::keepOnReset(id); });
            resetStage = 2;
            resetUntil = now + std::chrono::seconds(15);
            say("RESET: " + std::to_string(n) + " controls back to start  \xC2\xB7  UNDO within 15 s puts them back");
        } else {
            tree.deserialize(resetBackup);
            resetBackup.clear();
            resetStage = 0;
            say("RESET undone: every control is back how you had it");
        }
    }
    [[nodiscard]] int resetStageNow() {
        if (resetStage != 0 && std::chrono::steady_clock::now() > resetUntil) { resetStage = 0; resetBackup.clear(); }
        return resetStage;
    }
    bool                  orbiting = false;
    float                 orbitX = 0, orbitY = 0;
    // FX RACK: HEM effect inserts on the stem strips (FX in the top bar).
    struct RackModel final : ui::FxRackModel {
        App* app = nullptr;
        int  type(std::uint32_t s, int k) const override { return app->audio.fxRack().type(s, k); }
        fx::Effect* effect(std::uint32_t s, int k) const override { return app->audio.fxRack().effect(s, k); }
        bool bypass(std::uint32_t s, int k) const override { return app->audio.fxRack().bypass(s, k); }
        void setBypass(std::uint32_t s, int k, bool on) override { app->audio.fxRack().setBypass(s, k, on); }
        void setType(std::uint32_t s, int k, int t) override {
            if (!app->audio.fxRack().setEffect(s, k, t)) app->say("FX RACK busy - try again");
            else if (t >= 0) app->say(std::string(fx::effects()[static_cast<std::size_t>(t)].name) + " on " + stemName(s) + ", slot " + std::to_string(k + 1));
        }
        float tempo() const override {
            const float song = app->audio.fxRack().songTempo();
            if (song > 0.0f) return song;
            const params::Parameter* bpm = app->tree.find(forge::id::bpm);
            return bpm ? bpm->plain() : 120.0f;
        }
        bool  tempoSet() const override { return app->audio.fxRack().songTempo() > 0.0f; }
        void  setSongTempo(float bpm) override {
            app->audio.fxRack().setSongTempo(bpm);   // reaches the effects on the next frame
        }
        std::string stemName(std::uint32_t s) const override {
            const std::string& n = app->stems.slot(s).name;
            if (!n.empty()) { const auto dot = n.rfind('.'); return dot != std::string::npos && dot > 0 ? n.substr(0, dot) : n; }
            return "STEM " + std::to_string(s + 1);
        }
    };
    RackModel             rackModel;
    ui::FxRackView        rackView;
    bool                  rackOpen = false;
    std::string           savedRack;
    float                 rackTempo = 0.0f;

    // Feeds the analyzer with what the audio thread sent (every UI frame).
    void pumpScope(bool show) {
        analysis::ScopeTap* tap = audio.scopeTap();
        if (!tap) return;
        const bool want = show && audio.status().running;
        if (want != tapOn) {
            tap->setEnabled(want);
            if (want) {   // throw away whatever was left from the last time, start fresh
                while (tap->pop(tapBuf.data(), tapBuf.size()) > 0) {}
                analyzer.reset();
            }
            tapOn = want;
        }
        const bool playing = audio.wantsPlay();
        const double pos = static_cast<double>(audio.stemPlayhead()) / kSampleRate;
        if (playing && !wasPlaying && pos < 0.25) analyzer.reset();   // playing from the start: measure the song afresh
        wasPlaying = playing;
        if (!want) return;
        for (std::size_t n; (n = tap->pop(tapBuf.data(), tapBuf.size())) > 0;) analyzer.add(tapBuf.data(), n, pos);
    }

    void say(std::string text) {
        notice      = std::move(text);
        noticeUntil = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    }
    [[nodiscard]] std::string currentNotice() const {
        return std::chrono::steady_clock::now() < noticeUntil ? notice : std::string();
    }
    [[nodiscard]] std::filesystem::path songFolder() const {
        if (!session.empty()) return session.parent_path();
        for (std::uint32_t s = 0; s < kStemCount; ++s)
            if (!stems.slot(s).file.empty()) return stems.slot(s).file.parent_path();
        return {};
    }
    [[nodiscard]] std::wstring songName() const {
        if (!session.empty()) return session.stem().wstring();
        const std::filesystem::path f = songFolder();
        return f.empty() ? std::wstring(L"Hybrid Core Mix") : f.filename().wstring();
    }

    bool saveSessionTo(const std::filesystem::path& file, const ui::Console& console) {
        media::SessionData d;
        for (std::uint32_t s = 0; s < kStemCount; ++s) d.stems[s] = stems.slot(s).file;
        d.params    = tree.serialize();
        d.loop      = audio.loop();
        d.page      = static_cast<std::uint32_t>(console.page());
        d.faderBank = console.faderBank();
        d.source    = sourceName(audio.source());
        d.playhead  = audio.stemPlayhead();
        if (forge && !forge->song().pattern.isEmpty()) { d.forgeSong = forge->songText(); d.forgeSeeds = forge->seedsText(); }
        d.fxRack = audio.fxRack().toText();
        std::string err;
        if (!media::saveSession(file, d, HC_VERSION_STR, &err)) { say("Save failed: " + err); return false; }
        session = file;
        settings.set("app", "session", app::toUtf8(file.wstring()));
        say("Saved " + app::toUtf8(file.filename().wstring()));
        return true;
    }
    void openSession(const std::filesystem::path& file, ui::Console& console) {
        scopeSource.clear();
        auto l = media::loadSession(file);
        if (!l) { say("Open failed: " + l.error()); return; }
        audio.stop();
        for (std::size_t i = 0; i < tree.size(); ++i) tree.at(i).resetToDefault();
        tree.deserialize(l->data.params);
        for (std::uint32_t s = 0; s < kStemCount; ++s) {
            if (l->data.stems[s].empty()) stems.clear(s);
            else stems.load(s, {l->data.stems[s]});
        }
        audio.setLoop(l->data.loop);
        console.setPage(static_cast<ui::Page>(l->data.page));
        console.setFaderBank(l->data.faderBank);
        audio.setSource(sourceFromName(l->data.source));
        pendingSeek = l->data.playhead;
        if (forge) forge->restore(l->data.forgeSong, l->data.forgeSeeds);
        const std::string fxNote = audio.fxRack().fromText(l->data.fxRack);
        session = file;
        settings.set("app", "session", app::toUtf8(file.wstring()));
        std::string msg = "Opened " + app::toUtf8(file.filename().wstring());
        if (!l->missing.empty()) {
            msg += "  \xC2\xB7  " + std::to_string(l->missing.size()) + " stem(s) not found: ";
            for (std::size_t i = 0; i < l->missing.size(); ++i) msg += (i ? ", " : "") + l->missing[i];
        }
        if (!fxNote.empty()) msg += "  \xC2\xB7  " + fxNote;
        say(msg);
    }
    // Where exports go when there is no session or stem folder: the last export folder, else
    // Music\The Hybrid Core (created on first use).
    [[nodiscard]] std::filesystem::path exportFolder() {
        std::error_code ec;
        const std::filesystem::path last(app::fromUtf8(settings.get("app", "exportDir")));
        if (!last.empty() && std::filesystem::is_directory(last, ec)) return last;
        PWSTR music = nullptr;
        std::filesystem::path out;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &music))) {
            out = std::filesystem::path(music) / L"The Hybrid Core";
            std::filesystem::create_directories(out, ec);
            if (ec) out.clear();
        }
        ::CoTaskMemFree(music);
        return out;
    }
    // OPEN: a session (.hcs), or an audio file to listen to (an export, a Suno song): it goes on
    // strip 1 alone, Beat Forge goes off (its song is kept; ON on the BEAT FORGE page brings it back).
    void openAny(const std::filesystem::path& file, ui::Console& console) {
        std::wstring ext = file.extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
        if (ext == L".hcs") { openSession(file, console); return; }
        audio.stop();
        const bool beatWasOn = forgeOn();
        if (params::Parameter* on = tree.find(forge::id::on)) on->setPlain(0.0f);
        stems.replaceAll({file});
        audio.setSource(Source::Stems);
        pendingSeek = 0;
        session.clear();
        settings.set("app", "session", "");
        scopeSource = app::toUtf8(file.filename().wstring());
        console.setScope(true);
        analyzer.reset();
        std::string msg = "Opened " + app::toUtf8(file.filename().wstring()) + " on strip 1 - press PLAY (SCOPE shows the ANALYZER)";
        if (beatWasOn) msg += "  \xC2\xB7  BEAT FORGE is off (ON on the BEAT FORGE page brings your beat back)";
        say(msg);
    }
    void startExport(HWND owner) {
        if (exporter.busy()) { exporter.cancel(); return; }   // EXPORT while exporting = cancel
        const bool beat = forge && audio.forgeActive() && !forge->song().pattern.isEmpty();
        if (stems.loadedCount() == 0 && !beat) { say("Load stems or make a beat in BEAT FORGE before exporting"); return; }
        std::filesystem::path folder = songFolder();
        std::wstring name = songName() + L" - Mix";
        if (folder.empty()) {
            folder = exportFolder();
            if (beat && session.empty()) {   // a Beat Forge beat on its own: "Beat Forge - TRAP 140 BPM"
                const auto* g = tree.find(forge::id::genre);
                const auto* b = tree.find(forge::id::bpm);
                name = L"Beat Forge";
                if (g) name += L" - " + app::fromUtf8(g->format());
                if (b) name += L" " + std::to_wstring(static_cast<int>(std::lround(b->plain()))) + L" BPM";
            }
        }
        // "Save as type" is the mastering switch: song loudness (default) or the clean mix,
        // untouched, for mastering later (Playground, Suno). The choice is remembered.
        const COMDLG_FILTERSPEC kinds[] = {{L"WAV, mastered to song loudness (-14 LUFS)", L"*.wav"},
                                           {L"WAV, clean, no mastering (for mastering later)", L"*.wav"}};
        UINT kind = settings.get("export", "master", "1") != "0" ? 1u : 2u;
        const auto target = askSaveFileTyped(owner, L"Export the mix (WAV, 48 kHz, 24-bit)", kinds, 2, &kind, L"wav", folder / (name + L".wav"));
        if (target.empty()) return;
        settings.set("export", "master", kind == 2 ? "0" : "1");
        settings.set("app", "exportDir", app::toUtf8(target.parent_path().wstring()));
        media::ExportRequest req;
        for (std::uint32_t s = 0; s < kStemCount; ++s)
            if (!stems.slot(s).name.empty()) req.stems[s] = stems.slot(s).file;
        req.params   = tree.serialize();
        if (beat) req.forgeSong = forge->songText();
        req.fxRack   = audio.fxRack().toText();
        req.out      = target;
        req.encoding = media::WavEncoding::Pcm24;
        // Song-level loudness (settings.ini [export] target=-14 sets the level).
        req.master = kind != 2;
        req.masterSettings.targetLufs = std::clamp(std::atof(settings.get("export", "target", "-14").c_str()), -30.0, -6.0);
        lastExportMastered = req.master;
        if (exporter.start(std::move(req))) say("Exporting " + app::toUtf8(target.filename().wstring()) + "...");
    }
    void pollExport() {
        auto r = exporter.poll();
        if (!r) return;
        if (!r->has_value()) {
            say(r->error() == "cancelled" ? std::string("Export cancelled") : "Export failed: " + r->error());
            return;
        }
        const media::ExportResult& x = **r;
        char b[160];
        std::snprintf(b, sizeof b, " (%s, %.1f LUFS, peak %.1f dBFS)", fmtTime(static_cast<double>(x.frames) / kSampleRate).c_str(),
                      x.master.lufsAfter, x.peak > 0 ? 20.0 * std::log10(x.peak) : -144.0);
        std::string msg = "Exported " + app::toUtf8(x.file.filename().wstring()) + b;
        if (x.master.gainDb > 0.05) {
            std::snprintf(b, sizeof b, "  \xC2\xB7  mastered: raised %.1f dB to song level", x.master.gainDb);
            msg += b;
        } else if (!lastExportMastered) {
            msg += "  \xC2\xB7  clean, no mastering";
        }
        if (x.clipped) msg += "  \xC2\xB7  " + std::to_string(x.clipped) + " samples clipped (turn LIM on)";
        if (!x.warning.empty()) msg += "  \xC2\xB7  " + x.warning;
        say(msg);
    }

    // uiState = fader bank + 4 × knob page.
    void saveSettings(std::uint32_t uiState) {
        const std::uint32_t bank = uiState % 4, page = (uiState / 4) % 16;
        settings.set("app", "scope", uiState >= 64 ? "1" : "0");
        settings.setSectionText("params", tree.serialize());
        const auto& d = audio.current();
        settings.set("app", "device.kind", kindName(d.kind));
        settings.set("app", "device.id", app::toUtf8(d.id));
        settings.set("app", "source", sourceName(audio.source()));
        settings.set("app", "faderBank", std::to_string(bank));
        settings.set("app", "page", std::to_string(page));
        settings.set("app", "loop", audio.loop() ? "1" : "0");
        for (std::uint32_t s = 0; s < kStemCount; ++s) settings.set("stems", slotKey(s), app::toUtf8(stems.slot(s).file.wstring()));
        if (forge) {
            settings.setSectionText("forge", forge->songText());
            settings.set("forge.meta", "seeds", forge->seedsText());
        }
        savedRack = audio.fxRack().toText();
        settings.setSectionText("fx", savedRack);
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
    bool forgeOn() const { const params::Parameter* p = tree.find(forge::id::on); return p && p->isOn(); }
    // BEAT FORGE buttons.
    void forgeGenerate() {
        scopeSource.clear();
        forge->generate();
        if (audio.source() != Source::Stems) audio.setSource(Source::Stems);
        const auto* g = tree.find(forge::id::genre);
        const auto* b = tree.find(forge::id::bpm);
        say("BEAT FORGE  \xC2\xB7  " + (g ? g->format() : std::string()) + "  \xC2\xB7  " + forge->chords() + "  \xC2\xB7  " +
            (b ? b->format() : std::string()) + "  \xC2\xB7  " + forge->form());
    }
    // REMAKE: pick your beat (one mixed file) or its stems (named drums / bass / 808 / keys...;
    // vocals are skipped). It is measured here, on this PC, and comes back as a Beat Forge song.
    void startRemake(HWND owner) {
        if (remakeJob.busy()) { remakeJob.cancel(); say("REMAKE: cancelling..."); return; }
        const auto files = askForStems(owner, L"REMAKE: pick your beat (one file) or its stems (drums, bass / 808, keys...)");
        if (files.empty()) return;
        if (remakeJob.start(files, kSampleRate)) { remakeShown = -1; say("REMAKE: listening to " + remakeJob.what() + "..."); }
    }
    void pollRemake() {
        if (remakeJob.busy()) {
            const int pc = static_cast<int>(remakeJob.progress() * 100.0f);
            if (pc / 5 != remakeShown / 5) { remakeShown = pc; say("REMAKE: listening to " + remakeJob.what() + "  " + std::to_string(pc) + " %  (REMAKE again cancels)"); }
            return;
        }
        auto r = remakeJob.poll();
        if (!r) return;
        if (!r->ok) { say(r->error == "cancelled" ? std::string("REMAKE cancelled") : "REMAKE failed: " + r->error); return; }
        scopeSource.clear();
        forge->applyRemake(*r);
        if (audio.source() != Source::Stems) audio.setSource(Source::Stems);
        for (std::uint32_t s = 0; s < static_cast<std::uint32_t>(forge::kTracks); ++s) stems.clear(s);   // strips 1-8 play the remake
        say(r->summary + "  \xC2\xB7  press PLAY  \xC2\xB7  GEN / re-roll to redo parts, BACK undoes");
    }
    void forgeReroll(int track) {
        forge->reroll(track);
        say(std::string("BEAT FORGE  \xC2\xB7  new ") + forge::kTrackNames[track] + " part (BACK undoes it)");
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

// The visible console window of a HybridCore.exe that is already running (nullptr if none).
HWND findRunningConsole() {
    struct Find { HWND hit = nullptr; } f;
    ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64] = {};
        ::GetClassNameW(h, cls, 64);
        if (std::wstring(cls) == ui::GlWindow::className() && ::IsWindowVisible(h)) {
            reinterpret_cast<Find*>(lp)->hit = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&f));
    return f.hit;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options opts = parseOptions();

    // One console at a time (two would fight over the sound card). A second start — e.g. a
    // double-clicked .hcs — hands its session to the running one and brings it to the front.
    // Test runs (--exit-after) are exempt.
    HANDLE instance = ::CreateMutexW(nullptr, FALSE, L"Local\\HybridCore.Console");
    if (instance && ::GetLastError() == ERROR_ALREADY_EXISTS && opts.exitAfter <= 0.0) {
        if (HWND other = findRunningConsole()) {
            if (!opts.session.empty()) {
                const std::wstring p = std::filesystem::absolute(opts.session).wstring();
                COPYDATASTRUCT cd{ui::GlWindow::kOpenFileTag, static_cast<DWORD>((p.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(p.c_str())};
                DWORD_PTR result = 0;
                ::SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &result);
            }
            if (::IsIconic(other)) ::ShowWindow(other, SW_RESTORE);
            ::SetForegroundWindow(other);
        }
        return 0;
    }
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
    forge::addForgeParameters(a.tree);
    a.tree.deserialize(a.settings.sectionText("params"));
    const params::MixerBindings bindings(a.tree);
    params::bindMixerParameters(a.tree, bindings, a.audio.matrix(), &a.audio.limiter());
    a.tree.dispatchChanges();   // push every value into the matrix before audio starts
    a.audio.forge().bind(a.tree);
    a.forge = std::make_unique<app::ForgeController>(a.tree, a.audio.forge());
    a.forge->restore(a.settings.sectionText("forge"), a.settings.get("forge.meta", "seeds"));
    a.rackModel.app = &a;
    (void)a.audio.fxRack().fromText(a.settings.sectionText("fx"));   // effect inserts as you left them
    a.savedRack = a.audio.fxRack().toText();
    {   // Say so when knobs and faders come back the way they were left last time.
        std::size_t moved = 0;
        for (std::size_t i = 0; i < a.tree.size(); ++i) {
            const params::Parameter& p = a.tree.at(i);
            if (!forge::keepOnReset(p.spec().id) && p.plain() != p.spec().def) ++moved;
        }
        if (moved > 0)
            a.say("Welcome back: " + std::to_string(moved) + " controls are where you left them  \xC2\xB7  RESET puts them back");
    }

    if (opts.session.empty()) a.restoreStems();   // background loads; the saved source below wins over the "switch to stems" a load does
    a.session = app::fromUtf8(a.settings.get("app", "session"));
    a.audio.setSource(sourceFromName(a.settings.get("app", "source", "tones")));
    a.audio.setLoop(a.settings.get("app", "loop", "1") != "0");
    a.devices = app::AudioHost::listDevices();
    a.deviceIndex = opts.offline ? static_cast<int>(a.devices.size()) - 1
                                 : findDevice(a.devices, a.settings.get("app", "device.kind", "wasapi"), a.settings.get("app", "device.id"));
    a.audio.open(a.devices[static_cast<std::size_t>(a.deviceIndex)]);
    if (!opts.offline && a.settings.get("app", "device.kind", "wasapi") == "asio" &&
        a.devices[static_cast<std::size_t>(a.deviceIndex)].kind != app::DeviceChoice::Kind::Asio) {
#if defined(HC_HAS_ASIO_SDK)
        a.say("Your ASIO device was not found - playing through WASAPI instead (DEVICE button to change)");
#else
        a.say("This version has no ASIO - playing through WASAPI instead (DEVICE button to change)");
#endif
    }

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
    console.setScope(a.settings.get("app", "scope", "0") == "1");
    console.setOverlay([&a, &console](ui::Renderer& r, const ui::Canvas& c) {
        if (!a.rackOpen) return;
        a.rackView.setArea(console.layout().scopeRect());
        a.rackView.draw(r, c.scale, a.rackModel);
    });
    if (opts.fxDemo) {   // CI: an effect on strip 1 shown in the open FX RACK
        const auto list = fx::effects();
        for (std::size_t t = 0; t < list.size(); ++t) {
            if (std::string(list[t].name) == "HEM PUNCH") (void)a.audio.fxRack().setEffect(0, 0, static_cast<int>(t));
            if (std::string(list[t].name) == "HEM TUNE") (void)a.audio.fxRack().setEffect(0, 1, static_cast<int>(t));
        }
        console.setScope(false);
        a.rackOpen = true;
        a.rackView.select(0, 0);
    }
    auto uiState = [&console] { return console.faderBank() + 4u * static_cast<std::uint32_t>(console.page()) + (console.scope() ? 64u : 0u); };
    if (!opts.session.empty()) a.openSession(opts.session, console);
    std::filesystem::path shownSession = L"?";
    const HWND hwnd = static_cast<HWND>(window.nativeHandle());

    ui::MeterState meters;
    const auto started = std::chrono::steady_clock::now();
    bool shotTaken = false;
    auto lastFrame = started;
    auto lastSave  = lastFrame;
    std::uint64_t savedVersion = a.tree.version();
    std::uint64_t savedForge   = a.forge->version();
    std::array<std::uint32_t, forge::kTracks> lastHits{};
    std::array<float, forge::kTracks> hitGlow{};
    std::vector<std::uint8_t> sections;
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
                if (a.rackOpen && a.rackView.mouseDown(x, y, e.doubleClick, e.fine, a.rackModel)) break;   // FX RACK
                if (console.scope() && ui::scopeViewArea(console.layout().scopeRect()).contains(x, y)) {   // turn the 3D view
                    if (e.doubleClick) a.camera = {};
                    a.orbiting = !e.doubleClick;
                    a.orbitX = x;
                    a.orbitY = y;
                    break;
                }
                std::uint32_t idx = 0;
                switch (console.interaction().mouseDown(x, y, e.doubleClick, e.fine, &idx)) {
                case ui::UiAction::PrevDevice:   a.selectDevice(-1); break;
                case ui::UiAction::NextDevice:   a.selectDevice(+1); break;
                case ui::UiAction::LoadStems: {
                    const auto files = askForStems(static_cast<HWND>(window.nativeHandle()));
                    if (!files.empty()) {
                        a.scopeSource.clear();
                        if (a.forgeOn()) {   // Beat Forge owns strips 1-8: your stems (vocals) go to 9-13
                            for (std::uint32_t s = forge::kTracks; s < kStemCount; ++s) a.stems.clear(s);
                            a.stems.load(forge::kTracks, files);
                            a.say("BEAT FORGE is on: your stems were loaded from strip 9 (strips 1-8 play the beat)");
                        } else {
                            a.stems.replaceAll(files);
                        }
                    }
                    console.interaction().mouseUp();
                    break;
                }
                case ui::UiAction::ForgeGenerate: a.forgeGenerate(); break;
                case ui::UiAction::ForgeUndo:     a.forge->undo(); a.say(a.forge->canUndo() ? "" : "BEAT FORGE  \xC2\xB7  undone"); break;
                case ui::UiAction::ForgeRemake:   a.startRemake(static_cast<HWND>(window.nativeHandle())); console.interaction().mouseUp(); break;
                case ui::UiAction::ForgeClear:    a.forge->clear(); a.say("BEAT FORGE  \xC2\xB7  pattern cleared (BACK brings it back)"); break;
                case ui::UiAction::CycleSource: {
                    const Source s = a.audio.source();
                    a.audio.setSource(s == Source::TestTones ? Source::Hardware : s == Source::Hardware ? Source::Stems : Source::TestTones);
                    break;
                }
                case ui::UiAction::ClearSolo:    a.clearAll("solo"); break;
                case ui::UiAction::ClearMute:    a.clearAll("mute"); break;
                case ui::UiAction::FaderBank:    console.setFaderBank(idx); break;
                case ui::UiAction::None: {   // BEAT FORGE page: click a track's name (strips 1-8) = a new part for that track
                    const int at = console.layout().stemStripAt(x, y);
                    if (console.page() == ui::Page::Forge && at >= 0 && at < forge::kTracks &&
                        ui::Layout::titleArea(console.layout().strips()[static_cast<std::size_t>(at)]).contains(x, y))
                        a.forgeReroll(at);
                    break;
                }
                case ui::UiAction::SelectPage:   a.rackOpen = false; console.setPage(static_cast<ui::Page>(idx)); break;   // a page shows its own strips
                case ui::UiAction::ToStart:      a.audio.seek(0); a.analyzer.reset(); break;
                case ui::UiAction::ResetAll: a.resetPressed(); console.interaction().mouseUp(); break;
                case ui::UiAction::ToggleFx:
                    a.rackOpen = !a.rackOpen;
                    if (a.rackOpen) {
                        console.setScope(false);
                        a.say("FX RACK: pick a strip, then a slot, then an effect. Its faceplate opens on the right.");
                    }
                    console.interaction().mouseUp();
                    break;
                case ui::UiAction::ToggleScope:
                    a.rackOpen = false;
                    console.setScope(!console.scope());
                    if (console.scope()) a.say("ANALYZER: press PLAY, or OPEN a WAV / MP3 to measure it. Drag the 3D view to turn it.");
                    console.interaction().mouseUp();
                    break;
                case ui::UiAction::PlayPause:    a.playPause(); break;
                case ui::UiAction::Stop:         a.audio.stop(); break;
                case ui::UiAction::ToggleLoop:   a.audio.setLoop(!a.audio.loop()); break;
                case ui::UiAction::Seek:         break;   // collected below with takeSeek
                case ui::UiAction::OpenSession: {
                    const auto f = askOpenSession(hwnd, a.songFolder().empty() ? a.exportFolder() : a.songFolder());
                    if (!f.empty()) a.openAny(f, console);
                    console.interaction().mouseUp();
                    break;
                }
                case ui::UiAction::SaveSession: {
                    std::filesystem::path f = a.session;
                    if (f.empty()) f = askSaveFile(hwnd, L"Save session", L"Hybrid Core session", L"*.hcs", L"hcs", a.songFolder() / (a.songName() + L".hcs"));
                    if (!f.empty()) a.saveSessionTo(f, console);
                    console.interaction().mouseUp();
                    break;
                }
                case ui::UiAction::ExportMix:    a.startExport(hwnd); console.interaction().mouseUp(); break;
                }
                break;
            }
            case ui::InputEvent::Type::Key:
                if (e.ctrl) {   // Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+E export
                    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    if (e.key == 'O') {
                        const auto f = askOpenSession(hwnd, a.songFolder().empty() ? a.exportFolder() : a.songFolder());
                        if (!f.empty()) a.openAny(f, console);
                    } else if (e.key == 'S') {
                        std::filesystem::path f = shift ? std::filesystem::path() : a.session;
                        if (f.empty()) f = askSaveFile(hwnd, L"Save session", L"Hybrid Core session", L"*.hcs", L"hcs", a.songFolder() / (a.songName() + L".hcs"));
                        if (!f.empty()) a.saveSessionTo(f, console);
                    } else if (e.key == 'E') {
                        a.startExport(hwnd);
                    }
                    break;
                }
                switch (e.key) {
                case VK_SPACE:  a.playPause(); break;
                case 'G':       a.forgeGenerate(); break;
                case VK_HOME:   a.audio.seek(0); break;
                case VK_END:    a.audio.stop(); break;
                case 'L':       a.audio.setLoop(!a.audio.loop()); break;
                case VK_LEFT:   a.seekSeconds(e.ctrl ? -30.0 : -5.0); break;
                case VK_RIGHT:  a.seekSeconds(e.ctrl ? 30.0 : 5.0); break;
                default:        break;
                }
                break;
            case ui::InputEvent::Type::MouseMove:
                if (a.orbiting) {
                    a.camera.yaw   = std::clamp(a.camera.yaw + (x - a.orbitX) * 0.0022f, -0.9f, 0.9f);
                    a.camera.pitch = std::clamp(a.camera.pitch + (y - a.orbitY) * 0.0018f, 0.15f, 1.1f);
                    a.orbitX = x;
                    a.orbitY = y;
                } else {
                    if (a.rackOpen) a.rackView.mouseMove(x, y, e.fine);
                    if (!a.rackOpen || !a.rackView.contains(x, y)) console.interaction().mouseMove(x, y, e.fine);
                }
                break;
            case ui::InputEvent::Type::MouseUp:
                a.orbiting = false;
                if (a.rackOpen) a.rackView.mouseUp(x, y);
                console.interaction().mouseUp();
                break;
            case ui::InputEvent::Type::Wheel:
                if (a.rackOpen && a.rackView.wheel(x, y, e.wheel, e.fine)) break;
                console.interaction().wheel(x, y, e.wheel, e.fine);
                break;
            case ui::InputEvent::Type::Drop: {   // onto a stem strip: from that slot; elsewhere: first empty slot
                if (a.rackOpen && a.rackView.contains(x, y)) break;
                if (console.scope() && e.files.size() == 1) { a.openAny(e.files.front(), console); break; }   // onto the ANALYZER: listen to it
                const int at = console.layout().stemStripAt(x, y);
                const int first = at >= 0 ? at : std::max(0, a.stems.firstEmptySlot());
                a.stems.load(static_cast<std::uint32_t>(first), e.files);
                break;
            }
            case ui::InputEvent::Type::RightDown: {
                if (a.rackOpen && a.rackView.contains(x, y)) break;
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
            case ui::InputEvent::Type::OpenFile:   // a .hcs double-clicked while the console was already open
                if (!e.files.empty()) a.openAny(e.files.front(), console);
                break;
            case ui::InputEvent::Type::Resize:
            case ui::InputEvent::Type::Close:     break;
            }
        }

        const std::uint64_t songLen = std::max(a.stems.songFrames(), a.audio.stemSongFrames());   // stems or the Beat Forge song
        if (float f = 0; console.interaction().takeSeek(f) && songLen > 0) {
            if (a.audio.source() != Source::Stems) a.audio.setSource(Source::Stems);
            a.audio.seek(static_cast<std::uint64_t>(f * static_cast<double>(songLen - 1)));
        }
        a.pollExport();
        a.pollRemake();
        if (a.pendingSeek != UINT64_MAX && !a.stems.loading()) {
            if (a.pendingSeek < songLen) a.audio.seek(a.pendingSeek);
            a.pendingSeek = UINT64_MAX;
        }
        if (a.session != shownSession) {   // title bar: version and session name
            shownSession = a.session;
            window.setTitle(L"The Hybrid Core " + app::fromUtf8(HC_VERSION_STR) + (a.session.empty() ? std::wstring() : L"  \u2014  " + a.session.stem().wstring()));
        }
        a.tree.dispatchChanges();
        a.audio.poll();
        a.stems.pump();
        a.pumpScope(console.scope());
        {   // tempo-synced effects (HEM ECHO): the song tempo set in the FX RACK, else Beat Forge's
            const params::Parameter* bpm = a.tree.find(forge::id::bpm);
            const float song = a.audio.fxRack().songTempo();
            const float want = song > 0.0f ? song : bpm ? bpm->plain() : 120.0f;
            if (want != a.rackTempo) {
                a.rackTempo = want;
                a.audio.fxRack().setTempo(want);
            }
        }

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
            if (a.tree.version() != savedVersion || uiState() != savedBank || a.audio.loop() != savedLoop || a.stems.loading() ||
                a.forge->version() != savedForge || a.audio.fxRack().toText() != a.savedRack) {
                a.saveSettings(uiState());
                savedForge   = a.forge->version();
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
        const std::string notice = !a.currentNotice().empty() ? a.currentNotice() : a.stems.notice();
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
        if (console.page() == ui::Page::Forge)   // empty strips 9-12 carry the composer controls on this page
            for (std::uint32_t s = 8; s < 12; ++s)
                if (vs.slots[s].name.empty() && vs.slots[s].progress < 0.0f) {
                    static const char* const kNames[4] = {"COMPOSE", "FEEL", "SPACE", "SOUNDS"};
                    vs.slots[s].name = kNames[s - 8];
                    vs.slots[s].info = "BEAT FORGE";
                }
        // BEAT FORGE: strips 1-8 show its tracks; names flash on every note; the overview shows the song form.
        if (a.audio.forgeActive()) {
            const bool onPage = console.page() == ui::Page::Forge;
            for (int t = 0; t < forge::kTracks; ++t) {
                const std::uint32_t h = a.audio.forge().trackHit[static_cast<std::size_t>(t)].load(std::memory_order_relaxed);
                if (h != lastHits[static_cast<std::size_t>(t)]) hitGlow[static_cast<std::size_t>(t)] = 1.0f;
                lastHits[static_cast<std::size_t>(t)] = h;
                hitGlow[static_cast<std::size_t>(t)] *= std::pow(0.02f, std::min(dt, 0.1f));   // fades in ~0.25 s
                ui::StemSlotView& sv = vs.slots[static_cast<std::size_t>(t)];
                sv.name = forge::kTrackNames[t];
                const params::Parameter* ty = forge::hasType(t) ? a.tree.find(forge::id::type(t)) : nullptr;
                const bool empty = a.forge->song().pattern.tracks[static_cast<std::size_t>(t)].count == 0;
                sv.info = onPage ? std::string(empty ? "CLICK: NEW PART" : "CLICK: RE-ROLL") : (ty ? ty->format() : std::string("BEAT FORGE"));
                sv.progress = -1.0f;
                sv.failed = false;
                sv.glow = hitGlow[static_cast<std::size_t>(t)];
            }
            const forge::Song& song = a.forge->song();
            const params::Parameter* full = a.tree.find(forge::id::arrange);
            sections.clear();
            if (song.arrangement.active() && full && full->isOn())
                sections.assign(song.arrangement.section.begin(), song.arrangement.section.begin() + song.arrangement.bars);
            vs.sections   = sections.empty() ? nullptr : &sections;
            vs.barSeconds = 16.0 * forge::ForgeEngine::stepSamples(kSampleRate, a.tree.find(forge::id::bpm)->plain()) / kSampleRate;
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
        vs.transport.length    = static_cast<double>(songLen) / kSampleRate;
        vs.overview            = a.stems.songOverview().empty() ? nullptr : &a.stems.songOverview();
        for (std::uint32_t s = 0; s < kStemCount; ++s) vs.gainReduction[s] = st.running ? a.audio.matrix().strip(s).gainReductionDb() : 0.0f;
        vs.limiterGr           = st.running ? a.audio.limiter().gainReductionDb() : 0.0f;
        vs.exportProgress      = a.exporter.progress();
        vs.scope       = console.scope();
        vs.analyzer    = st.running ? &a.analyzer : nullptr;
        vs.camera      = a.camera;
        vs.scopeSource = a.scopeSource;
        vs.resetStage  = a.resetStageNow();
        vs.fxRack      = a.rackOpen;
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

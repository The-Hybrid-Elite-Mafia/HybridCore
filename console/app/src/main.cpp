// =============================================================================
//  HEMDigitalConsole.exe — the HEM DIGITAL CONSOLE: 13 stems through the whole rig (desk, step EQ, twin bus,
//  colour stage, meter bridge, front-end tools), on the GPU.
//  Threads: the UI / message thread (this file) and the audio thread (owned by the WASAPI stream, the ASIO driver
//  or the offline clock). They share only atomics and wait-free rings; the audio thread never touches graphics,
//  files or the UI. Stems decode on loader threads; exports render on their own thread.
// =============================================================================
#include "ConsoleHost.hpp"

#include "hybrid/console/ConsolePlayer.hpp"
#include "hybrid/console/FrontTools.hpp"
#include "hybrid/console/ui/ConsoleScreen.hpp"
#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/media/StemLoader.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Image.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace hc;
using namespace hem::console;
namespace U = hc::console::ui;
using hc::console::kPlayerSlots;

namespace {

constexpr int kMsaa = 8;
const wchar_t* kAppName = L"HEM Digital Console";

bool gUnattended = false;   // test runs (--exit-after): report problems on stderr, never in a box nobody can click
void fatal(const std::string& msg) {
    if (gUnattended) { std::fprintf(stderr, "HEM Digital Console: %s\n", msg.c_str()); return; }
    ::MessageBoxW(nullptr, fromUtf8(msg).c_str(), kAppName, MB_ICONERROR | MB_OK);
}

fs::path exeDir() {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return fs::path(std::wstring(buf, n)).parent_path();
}
// %APPDATA%\HEM Digital Console — settings and the autosave live here; the installer never touches it.
fs::path dataDir() {
    PWSTR p = nullptr;
    fs::path out = exeDir();
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) out = fs::path(p) / L"HEM Digital Console";
    if (p) ::CoTaskMemFree(p);
    std::error_code ec;
    fs::create_directories(out, ec);
    return out;
}
std::string u8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
bool writeFile(const fs::path& p, const std::string& text) {
    const fs::path tmp = p.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);   // replaces the old file in one step: a crash mid-save never leaves half a file
    return !ec;
}

// ---- small INI ----
struct Settings {
    std::map<std::string, std::string> kv;
    fs::path file;
    void load(const fs::path& f) {
        file = f;
        std::istringstream in(readFile(f));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq != std::string::npos && line[0] != '[' && line[0] != ';') kv[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    [[nodiscard]] std::string get(const std::string& k, const std::string& def = {}) const { auto it = kv.find(k); return it == kv.end() ? def : it->second; }
    void set(const std::string& k, const std::string& v) { kv[k] = v; }
    void save() const {
        std::string s = "[hem-digital-console]\n";
        for (const auto& [k, v] : kv) s += k + "=" + v + "\n";
        (void)writeFile(file, s);
    }
};

// ---- Windows dialogs ----
std::vector<fs::path> askOpen(HWND owner, const wchar_t* title, bool multi, bool audio, const fs::path& folder) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC aud[] = {{L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}, {L"All files", L"*.*"}};
    const COMDLG_FILTERSPEC ses[] = {{L"HEM Digital Console session (.hemc)", L"*.hemc"}};
    if (audio) dlg->SetFileTypes(2, aud); else dlg->SetFileTypes(1, ses);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM | (multi ? FOS_ALLOWMULTISELECT : 0));
    std::error_code ec;
    if (!folder.empty() && fs::is_directory(folder, ec)) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* it = nullptr;
                PWSTR path = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out.emplace_back(path); ::CoTaskMemFree(path); }
                if (it) it->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}
fs::path askSave(HWND owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* pattern, const wchar_t* ext, const fs::path& suggested) {
    fs::path out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}};
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(ext);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    if (!suggested.filename().empty()) dlg->SetFileName(suggested.filename().c_str());
    std::error_code ec;
    if (!suggested.parent_path().empty() && fs::is_directory(suggested.parent_path(), ec)) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(suggested.parent_path().c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; ::CoTaskMemFree(path); }
        if (it) it->Release();
    }
    dlg->Release();
    return out;
}
fs::path askFolder(HWND owner, const wchar_t* title, const fs::path& start) {
    fs::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    std::error_code ec;
    if (!start.empty() && fs::is_directory(start, ec)) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; ::CoTaskMemFree(path); }
        if (it) it->Release();
    }
    dlg->Release();
    return out;
}

struct Options {
    double exitAfter = 0;
    fs::path screenshot, session;
    bool offline = false;
    int page = -1;
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
        else if (k == L"--page" && i + 1 < argc) o.page = static_cast<int>(std::wcstol(argv[++i], nullptr, 10));
        else if (!k.empty() && k[0] != L'-') o.session = k;
    }
    if (argv) ::LocalFree(argv);
    return o;
}

using Clock = std::chrono::steady_clock;

// ---- the app ----
struct App {
    hc::console::ConsoleRig rig;
    U::AppState st;
    ConsoleHost host{rig};
    media::StemLoader loader{4};
    media::StemLoader extLoader{2};
    std::array<fs::path, kPlayerSlots> files{}, loading{};
    std::array<std::unique_ptr<engine::StemAudio>, kPlayerSlots> pending{};   // waiting for room in the audio thread's queue
    std::vector<Device> devices;
    int deviceIndex = 0;
    hc::console::ConsoleExporter exporter;
    hc::console::ExportJob::Kind exportKind = hc::console::ExportJob::Kind::Mix;
    fs::path session, data = dataDir();
    Settings settings;
    std::array<hc::console::TapTempo, kPlayerSlots> taps;
    std::string notice;
    Clock::time_point noticeUntil{}, resetUntil{};
    std::string resetBackup, savedSnapshot;
    HWND hwnd = nullptr;

    void say(std::string t, int seconds = 8) { notice = std::move(t); noticeUntil = Clock::now() + std::chrono::seconds(seconds); }
    [[nodiscard]] hc::console::ConsoleProcessor& player() { return host.player(); }
    // What a save would write (controls + stem files, loaded or still loading): compared to find unsaved changes.
    [[nodiscard]] std::string snapshot() {
        const auto d = sessionData();
        std::string s = d.rigState;
        for (const auto& f : d.files) s += u8(f) + "\n";
        return s;
    }
    [[nodiscard]] fs::path songFolder() const {
        if (!session.empty()) return session.parent_path();
        for (const auto& f : files) if (!f.empty()) return f.parent_path();
        PWSTR p = nullptr;
        fs::path out;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Music, 0, nullptr, &p)) && p) out = p;
        if (p) ::CoTaskMemFree(p);
        return out;
    }
    [[nodiscard]] std::wstring songName() const {
        if (!session.empty()) return session.stem().wstring();
        for (const auto& f : files) if (!f.empty()) return f.parent_path().filename().wstring();
        return L"HEM Mix";
    }

    // ---- stems ----
    void loadSlot(int slot, const fs::path& f) {
        if (slot < 0 || slot >= kPlayerSlots) return;
        pending[static_cast<std::size_t>(slot)].reset();
        loading[static_cast<std::size_t>(slot)] = f;
        auto& s = st.stems[static_cast<std::size_t>(slot)];
        s.error.clear();
        s.name = u8(f.stem());
        s.info = "";
        if (slot < hc::console::ConsoleRig::kChannels) loader.load(static_cast<std::uint32_t>(slot), f);
        else extLoader.load(static_cast<std::uint32_t>(slot - hc::console::ConsoleRig::kChannels), f);
    }
    void clearSlot(int slot) {
        if (slot < 0 || slot >= kPlayerSlots) return;
        if (slot < hc::console::ConsoleRig::kChannels) loader.cancel(static_cast<std::uint32_t>(slot));
        else extLoader.cancel(static_cast<std::uint32_t>(slot - hc::console::ConsoleRig::kChannels));
        pending[static_cast<std::size_t>(slot)].reset();
        std::unique_ptr<engine::StemAudio> none;
        (void)player().submit(slot, none);
        files[static_cast<std::size_t>(slot)].clear();
        loading[static_cast<std::size_t>(slot)].clear();
        st.stems[static_cast<std::size_t>(slot)] = {};
    }
    void loadMany(int first, const std::vector<fs::path>& in) {
        const auto list = media::collectAudioFiles(in);
        if (list.empty()) { say("No WAV, MP3 or FLAC files there"); return; }
        int slot = std::clamp(first, 0, hc::console::ConsoleRig::kChannels - 1);
        int used = 0;
        for (const auto& f : list) {
            if (slot >= hc::console::ConsoleRig::kChannels) break;
            loadSlot(slot++, f);
            ++used;
        }
        if (static_cast<int>(list.size()) > used) say(std::to_string(list.size() - static_cast<std::size_t>(used)) + " file(s) left out: the desk has 13 channels");
    }
    void pumpLoaders() {
        auto take = [&](media::StemLoader& l, int base) {
            media::StemLoader::Result r;
            while (l.poll(r)) {
                const int slot = base + static_cast<int>(r.slot);
                auto& s = st.stems[static_cast<std::size_t>(slot)];
                if (!r.audio) {
                    s.error = r.error;
                    s.name.clear();
                    loading[static_cast<std::size_t>(slot)].clear();
                    say("Can't load " + u8(r.file.filename()) + ": " + r.error);
                    continue;
                }
                s.name = u8(r.file.stem());
                // short line for the name strip: codec and length
                s.info = media::describeStem(*r.audio);
                files[static_cast<std::size_t>(slot)] = r.file;
                loading[static_cast<std::size_t>(slot)].clear();
                pending[static_cast<std::size_t>(slot)] = std::move(r.audio);
            }
        };
        take(loader, 0);
        take(extLoader, hc::console::ConsoleRig::kChannels);
        for (int k = 0; k < kPlayerSlots; ++k) {
            auto& p = pending[static_cast<std::size_t>(k)];
            if (p) (void)player().submit(k, p);   // stays pending while the queue is full
            st.stems[static_cast<std::size_t>(k)].progress = k < hc::console::ConsoleRig::kChannels
                ? loader.progress(static_cast<std::uint32_t>(k)) : extLoader.progress(static_cast<std::uint32_t>(k - hc::console::ConsoleRig::kChannels));
        }
    }

    // ---- sessions ----
    void newSession() {
        for (int k = 0; k < kPlayerSlots; ++k) clearSlot(k);
        rig.setDefaults();
        player().stop();
        session.clear();
        st.listen = 0; player().setListen(0);
        savedSnapshot = snapshot();
        say("New session: every control at its start position, no stems");
    }
    bool openSession(const fs::path& f, bool quiet = false) {
        const std::string text = readFile(f);
        std::vector<int> missing;
        auto s = hc::console::readSession(text, f, &missing);
        if (!s) { if (!quiet) say("Can't open " + u8(f.filename()) + ": " + s.error()); return false; }
        for (int k = 0; k < kPlayerSlots; ++k) clearSlot(k);
        player().stop();
        rig.setDefaults();
        rig.loadText(s->rigState);
        for (int k = 0; k < kPlayerSlots; ++k)
            if (!s->files[static_cast<std::size_t>(k)].empty()) loadSlot(k, s->files[static_cast<std::size_t>(k)]);
        st.listen = s->listen; player().setListen(s->listen);
        player().setLoop(s->loop);
        if (!quiet) {
            session = f;
            say(missing.empty() ? "Opened " + u8(f.filename()) : "Opened " + u8(f.filename()) + ": " + std::to_string(missing.size()) + " stem file(s) not found");
        }
        savedSnapshot = snapshot();
        return true;
    }
    [[nodiscard]] hc::console::SessionData sessionData() {
        hc::console::SessionData d;
        for (int k = 0; k < kPlayerSlots; ++k)
            d.files[static_cast<std::size_t>(k)] = !files[static_cast<std::size_t>(k)].empty() ? files[static_cast<std::size_t>(k)] : loading[static_cast<std::size_t>(k)];
        d.listen = st.listen;
        d.loop = host.player().loop();
        d.rigState = rig.saveText();
        return d;
    }
    bool saveSessionTo(const fs::path& f) {
        if (!writeFile(f, hc::console::writeSession(sessionData(), f))) { say("Can't save " + u8(f)); return false; }
        session = f;
        savedSnapshot = snapshot();
        say("Saved " + u8(f.filename()));
        return true;
    }
    void autosave() {
        const fs::path f = data / L"autosave.hemc";
        (void)writeFile(f, hc::console::writeSession(sessionData(), f));
        settings.set("session", u8(session));
        settings.set("dirty", savedSnapshot != snapshot() ? "1" : "0");
        settings.save();
    }

    // ---- export ----
    void startExport(bool stems) {
        bool any = false;
        for (int k = 0; k < hc::console::ConsoleRig::kChannels; ++k) any = any || !files[static_cast<std::size_t>(k)].empty();
        if (!any) { say("Load stems first"); return; }
        if (exporter.busy()) { say("An export is already running"); return; }
        hc::console::ExportJob job;
        job.kind = stems ? hc::console::ExportJob::Kind::Stems : hc::console::ExportJob::Kind::Mix;
        job.files = files;
        for (int k = 0; k < hc::console::ConsoleRig::kChannels; ++k) job.names[static_cast<std::size_t>(k)] = st.stems[static_cast<std::size_t>(k)].name;
        job.rigState = rig.saveText();
        if (stems) {
            job.out = askFolder(hwnd, L"PRINT STEMS: pick a folder for the channel files", songFolder());
            if (job.out.empty()) return;
            job.out /= songName() + L" stems";
        } else {
            job.out = askSave(hwnd, L"Export the mix (MAIN OUT)", L"WAV audio", L"*.wav", L"wav", songFolder() / (songName() + L" (HEM mix).wav"));
            if (job.out.empty()) return;
        }
        exportKind = job.kind;
        if (exporter.start(std::move(job))) say(stems ? "Printing stems..." : "Exporting the mix...", 3);
    }
    void pollExport() {
        st.exportProgress = exporter.busy() ? std::max(0.0f, exporter.progress()) : -1.0f;
        std::expected<hc::console::ExportResult, std::string> r = std::unexpected(std::string());
        if (!exporter.takeResult(r)) return;
        if (!r) { say("Export stopped: " + r.error(), 12); return; }
        char b[400];
        if (exportKind == hc::console::ExportJob::Kind::Mix)
            std::snprintf(b, sizeof b, "Exported %s  %.0f s  %.1f LUFS  %.1f dBTP%s%s", u8(r->files.empty() ? fs::path() : r->files[0].filename()).c_str(),
                          static_cast<double>(r->frames) / 48000.0, r->integratedLufs, r->truePeakDbtp, r->warning.empty() ? "" : "  ", r->warning.c_str());
        else
            std::snprintf(b, sizeof b, "PRINT STEMS: %zu files in %s%s%s", r->files.size(), r->files.empty() ? "" : u8(r->files[0].parent_path().filename()).c_str(),
                          r->warning.empty() ? "" : "  ", r->warning.c_str());
        say(b, 15);
    }

    // ---- device ----
    void openDevice(int index) {
        if (devices.empty()) return;
        deviceIndex = ((index % static_cast<int>(devices.size())) + static_cast<int>(devices.size())) % static_cast<int>(devices.size());
        const Device& d = devices[static_cast<std::size_t>(deviceIndex)];
        const bool ok = host.open(d);
        if (d.kind != Device::Kind::Offline) {
            settings.set("device.kind", d.kind == Device::Kind::Asio ? "asio" : "wasapi");
            settings.set("device.id", toUtf8(d.id));
        }
        if (!ok) say(host.status().problem, 12);
    }

    // ---- RESET: press once = SURE?, again within 4 s = every control to its start; then UNDO for 15 s ----
    void resetPressed() {
        const auto now = Clock::now();
        if (st.resetStage != 0 && now > resetUntil) st.resetStage = 0;
        if (st.resetStage == 0) {
            st.resetStage = 1;
            resetUntil = now + std::chrono::seconds(4);
            say("Press RESET again to put every control back to its start (stems stay)", 4);
        } else if (st.resetStage == 1) {
            resetBackup = rig.saveText();
            rig.setDefaults();
            st.resetStage = 2;
            resetUntil = now + std::chrono::seconds(15);
            say("RESET done. Press UNDO within 15 s to put everything back", 15);
        } else {
            rig.loadText(resetBackup);
            resetBackup.clear();
            st.resetStage = 0;
            say("RESET undone: every control is back how you had it");
        }
    }

    void handle(const U::ActionEvent& e, U::ConsoleScreen& screen) {
        auto& p = player();
        switch (e.act) {
        case U::Act::LoadStems: {
            const auto f = askOpen(hwnd, L"Load stems (up to 13, in name order)", true, true, songFolder());
            if (!f.empty()) { for (int k = 0; k < hc::console::ConsoleRig::kChannels; ++k) clearSlot(k); loadMany(0, f); }
            break;
        }
        case U::Act::LoadSlot: {
            const auto f = askOpen(hwnd, e.arg == hc::console::kExt1 ? L"Load a file into EXT 1" : e.arg == hc::console::kExt2 ? L"Load a file into EXT 2" : L"Load a file into this channel",
                                   false, true, songFolder());
            if (!f.empty()) loadSlot(e.arg, f.front());
            break;
        }
        case U::Act::ClearSlot: clearSlot(e.arg); break;
        case U::Act::Play:
            if (p.songFrames() == 0) { say("Load stems first (LOAD STEMS, or drag files onto the window)"); break; }
            if (p.wantsPlay()) p.pause(); else p.play();
            break;
        case U::Act::Stop: p.stop(); break;
        case U::Act::Rewind: p.seek(0); break;
        case U::Act::Loop: p.setLoop(!p.loop()); break;
        case U::Act::Seek:
            if (p.songFrames() > 0) p.seek(static_cast<std::uint64_t>(static_cast<double>(e.value) * static_cast<double>(p.songFrames() - 1)));
            break;
        case U::Act::Export: startExport(false); break;
        case U::Act::PrintStems: startExport(true); break;
        case U::Act::NewSession:
            if (savedSnapshot != snapshot() &&
                ::MessageBoxW(hwnd, L"Start a new session? Changes since the last save will be lost.", kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES) break;
            newSession();
            break;
        case U::Act::OpenSession: {
            const auto f = askOpen(hwnd, L"Open a HEM Digital Console session", false, false, songFolder());
            if (!f.empty()) openSession(f.front());
            break;
        }
        case U::Act::SaveSession: {
            fs::path f = session;
            if (f.empty()) f = askSave(hwnd, L"Save the session", L"HEM Digital Console session", L"*.hemc", L"hemc", songFolder() / (songName() + L".hemc"));
            if (!f.empty()) saveSessionTo(f);
            break;
        }
        case U::Act::Device: openDevice(deviceIndex + 1); break;
        case U::Act::Listen:
            st.listen = (st.listen + 1) % 3;
            p.setListen(st.listen);
            break;
        case U::Act::ToolTarget:
            st.toolTarget = ((st.toolTarget + e.arg) % (U::kColourToolTarget + 1) + (U::kColourToolTarget + 1)) % (U::kColourToolTarget + 1);
            break;
        case U::Act::Tap: {
            const double t = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
            auto& tt = taps[static_cast<std::size_t>(std::clamp(st.toolTarget, 0, kPlayerSlots - 1))];
            if (tt.tap(t)) {
                hc::console::ToolSlot& slot = st.toolTarget >= U::kColourToolTarget ? rig.colourTool() : rig.channelTool(st.toolTarget);
                slot.tool(hc::console::ToolSlot::Echo).set(hc::console::echo_ids::Tempo, static_cast<float>(tt.bpm()));
                char b[64];
                std::snprintf(b, sizeof b, "TAP: %.1f BPM", tt.bpm());
                say(b, 3);
            } else {
                say("TAP: keep tapping the beat", 2);
            }
            break;
        }
        case U::Act::ResetMeter: rig.requestMeterReset(); break;
        case U::Act::ResetAll: resetPressed(); break;
        case U::Act::Page: settings.set("page", std::to_string(static_cast<int>(screen.page()))); break;
        default: break;
        }
    }
};

HWND findRunning() {
    HWND hit = nullptr;
    ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        wchar_t cls[64] = {}, title[128] = {};
        ::GetClassNameW(h, cls, 64);
        ::GetWindowTextW(h, title, 128);
        if (std::wstring(cls) == ui::GlWindow::className() && std::wstring(title).starts_with(L"HEM Digital Console") && ::IsWindowVisible(h)) {
            *reinterpret_cast<HWND*>(lp) = h;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&hit));
    return hit;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const Options opts = parseOptions();
    gUnattended = opts.exitAfter > 0.0;
    // One console at a time (two would fight over the sound card). A second start (a double-clicked .hemc)
    // hands its session to the running one. Test runs (--exit-after) are exempt.
    HANDLE instance = ::CreateMutexW(nullptr, FALSE, L"Local\\HEM.DigitalConsole");
    if (instance && ::GetLastError() == ERROR_ALREADY_EXISTS && opts.exitAfter <= 0.0) {
        if (HWND other = findRunning()) {
            if (!opts.session.empty()) {
                const std::wstring p = fs::absolute(opts.session).wstring();
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
    if (!cpu.avx2 || !cpu.fma) { fatal("This CPU does not support AVX2/FMA. The HEM Digital Console is built for the i7-14700HX."); return 2; }

    auto app = std::make_unique<App>();
    App& a = *app;
    a.settings.load(a.data / L"settings.ini");

    ui::GlWindow window;
    std::string err;
    if (!window.create(L"HEM Digital Console", 1389, 868, true, &err)) { fatal("Could not start the GPU display: " + err + "\nUpdate your graphics driver (OpenGL 3.3 is required)."); return 3; }
    a.hwnd = static_cast<HWND>(window.nativeHandle());
    if (opts.exitAfter <= 0.0) ::ShowWindow(a.hwnd, SW_MAXIMIZE);

    std::vector<U::ActionEvent> actions;
    U::ConsoleScreen screen(a.rig, a.st, [&actions](const U::ActionEvent& e) { actions.push_back(e); });
    const fs::path assets = exeDir() / "assets";
    if (!screen.init(assets, kMsaa, &err)) { fatal("The console could not load its art: " + err + "\nReinstall the HEM Digital Console (assets: " + u8(assets) + ")."); return 3; }

    // audio device: the saved one, else the default output
    a.devices = ConsoleHost::listDevices();
    int want = 0;
    if (opts.offline) want = static_cast<int>(a.devices.size()) - 1;
    else {
        const std::string kind = a.settings.get("device.kind", "wasapi"), id = a.settings.get("device.id");
        for (std::size_t i = 0; i < a.devices.size(); ++i)
            if ((a.devices[i].kind == Device::Kind::Asio ? "asio" : "wasapi") == kind && toUtf8(a.devices[i].id) == id && a.devices[i].kind != Device::Kind::Offline) want = static_cast<int>(i);
    }
    a.openDevice(want);

    // session: from the command line, else everything as it was when the console was last closed
    if (!opts.session.empty()) a.openSession(opts.session);
    else {
        std::error_code ec;
        const fs::path auto_ = a.data / L"autosave.hemc";
        if (fs::exists(auto_, ec) && a.openSession(auto_, true)) {
            a.session = fromU8(a.settings.get("session"));
            if (a.settings.get("dirty", "1") == "1") a.savedSnapshot.clear();   // unsaved changes last time: still unsaved
            a.say("Welcome back: your last session is restored");
        } else {
            a.savedSnapshot = a.snapshot();   // nothing to save yet
            a.say("LOAD STEMS (or drag files onto the window) to start", 12);
        }
    }
    const int page = opts.page >= 0 ? opts.page : std::atoi(a.settings.get("page", "0").c_str());
    screen.setPage(static_cast<U::PageId>(std::clamp(page, 0, U::kPageCount - 1)));

    const auto started = Clock::now();
    auto lastAutosave = started;
    bool shotTaken = false;
    fs::path shownSession = L"?";
    bool shownDirty = false;
    int frame = 0;
    while (window.pump()) {
        const int fbW = window.pixelWidth(), fbH = window.pixelHeight();
        if (fbW > 0 && fbH > 0 && !window.minimized() && !screen.resize(fbW, fbH, &err)) { fatal("GPU error: " + err); break; }
        for (const auto& e : window.takeEvents()) {
            switch (e.type) {
            case ui::InputEvent::Type::MouseDown: screen.mouseDown(e.x, e.y, e.doubleClick, e.fine); break;
            case ui::InputEvent::Type::RightDown: screen.rightDown(e.x, e.y); break;
            case ui::InputEvent::Type::MouseMove: screen.mouseMove(e.x, e.y, e.fine); break;
            case ui::InputEvent::Type::MouseUp: screen.mouseUp(e.x, e.y); break;
            case ui::InputEvent::Type::Wheel: screen.wheel(e.x, e.y, e.wheel, e.fine); break;
            case ui::InputEvent::Type::Drop: {
                if (e.files.size() == 1 && e.files[0].extension() == hc::console::kSessionExtension) { a.openSession(e.files[0]); break; }
                // onto a channel's name strip: from that channel; elsewhere: the first empty channel
                int first = -1;
                const float x = screen.canvas().toDesignX(e.x), y = screen.canvas().toDesignY(e.y);
                for (const auto& w : screen.currentPage().widgets)
                    if (w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName && w.r.contains(x, y)) first = w.customArg;
                if (first >= hc::console::ConsoleRig::kChannels) { a.loadSlot(first, e.files.front()); break; }
                if (first < 0) for (int k = 0; k < hc::console::ConsoleRig::kChannels && first < 0; ++k) if (a.files[static_cast<std::size_t>(k)].empty() && a.loading[static_cast<std::size_t>(k)].empty()) first = k;
                a.loadMany(std::max(first, 0), e.files);
                break;
            }
            case ui::InputEvent::Type::Key:
                if (e.ctrl) {
                    if (e.key == 'S') actions.push_back({U::Act::SaveSession});
                    else if (e.key == 'O') actions.push_back({U::Act::OpenSession});
                    else if (e.key == 'N') actions.push_back({U::Act::NewSession});
                    else if (e.key == 'E') actions.push_back({U::Act::Export});
                    break;
                }
                if (e.key == VK_SPACE) actions.push_back({U::Act::Play});
                else if (e.key == VK_HOME) actions.push_back({U::Act::Rewind});
                else if (e.key == VK_END) actions.push_back({U::Act::Stop});
                else if (e.key == 'L') actions.push_back({U::Act::Loop});
                else if (e.key >= '1' && e.key <= '6') screen.setPage(static_cast<U::PageId>(e.key - '1'));
                break;
            case ui::InputEvent::Type::OpenFile:
                if (!e.files.empty()) a.openSession(e.files.front());
                break;
            default: break;
            }
        }
        for (const auto& e : actions) a.handle(e, screen);
        actions.clear();

        // state for the screen
        a.host.poll();
        a.pumpLoaders();
        a.pollExport();
        auto& p = a.player();
        a.st.playing = p.wantsPlay();
        a.st.loop = p.loop();
        a.st.position = static_cast<double>(p.playhead()) / 48000.0;
        a.st.length = static_cast<double>(p.songFrames()) / 48000.0;
        a.st.device = a.host.status().line;
        a.st.alert = a.host.status().alert;
        a.st.dspLoad = a.host.status().load;
        if (a.st.resetStage != 0 && Clock::now() > a.resetUntil) { a.st.resetStage = 0; a.resetBackup.clear(); }
        a.st.status = Clock::now() < a.noticeUntil ? a.notice : a.host.status().problem;
        if (++frame % 30 == 0) a.st.dirty = a.savedSnapshot != a.snapshot();
        a.st.session = a.session.empty() ? "UNTITLED" : u8(a.session.stem());
        if (a.session != shownSession || a.st.dirty != shownDirty) {
            shownSession = a.session; shownDirty = a.st.dirty;
            window.setTitle(std::wstring(L"HEM Digital Console ") + fromUtf8(HEM_CONSOLE_VERSION) + L"  —  " +
                            (a.session.empty() ? std::wstring(L"untitled") : a.session.stem().wstring()) + (a.st.dirty ? L" *" : L""));
        }
        if (Clock::now() - lastAutosave > std::chrono::seconds(60)) { a.autosave(); lastAutosave = Clock::now(); }

        if (window.minimized()) { ::Sleep(50); continue; }
        screen.render(0);
        window.swap();

        if (opts.exitAfter > 0.0 && std::chrono::duration<double>(Clock::now() - started).count() >= opts.exitAfter) {
            if (!opts.screenshot.empty() && !shotTaken) {
                screen.waitForPlate(30);
                screen.renderToCapture();
                shotTaken = ui::writePng(opts.screenshot, screen.capture());
            }
            break;
        }
    }
    a.host.close();
    a.autosave();
    return opts.screenshot.empty() || shotTaken ? 0 : 4;
}

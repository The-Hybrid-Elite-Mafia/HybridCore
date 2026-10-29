// =============================================================================
//  HEMDigitalConsole.exe — the HEM DIGITAL CONSOLE: 13 stems through the whole rig (desk, step EQ, twin bus,
//  colour stage, meter bridge, front-end tools), on the GPU.
//  Threads: the UI / message thread (this file) and the audio thread (owned by the WASAPI stream, the ASIO driver
//  or the offline clock). They share only atomics and wait-free rings; the audio thread never touches graphics,
//  files or the UI. Stems decode on loader threads; exports render on their own thread.
//  Files: sessions (.hemc) are saved safely (written to a side file, flushed, then swapped in), a recovery copy is
//  written every 3 minutes while the console runs and offered after a crash, moved stems are relinked by name.
//  DPI: Per-Monitor V2 from the manifest (resources/HEMDigitalConsole.manifest); the faceplate always fits the window.
// =============================================================================
#include "ConsoleHost.hpp"

#include "hybrid/console/ConsolePlayer.hpp"
#include "hybrid/console/FrontTools.hpp"
#include "hybrid/console/SessionFiles.hpp"
#include "hybrid/console/ui/ConsoleScreen.hpp"
#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/media/StemLoader.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Image.hpp"

#include <windows.h>
#include <commctrl.h>
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
#include <set>
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
constexpr int kMinClientDipW = 800, kMinClientDipH = 450;      // smallest window (DIPs): every button still readable
constexpr auto kRecoveryEvery = std::chrono::minutes(3);

// The common file dialogs remember their last folder per purpose (IFileDialog::SetClientGuid).
constexpr GUID kOpenDialogGuid  = {0x6f1d6b52, 0x3c1e, 0x4a8e, {0x9b, 0x21, 0x48, 0x45, 0x4d, 0x4f, 0x50, 0x4e}};
constexpr GUID kStemsDialogGuid = {0x6f1d6b52, 0x3c1e, 0x4a8e, {0x9b, 0x21, 0x48, 0x45, 0x4d, 0x53, 0x54, 0x4d}};
constexpr GUID kSaveDialogGuid  = {0x6f1d6b52, 0x3c1e, 0x4a8e, {0x9b, 0x21, 0x48, 0x45, 0x4d, 0x53, 0x41, 0x56}};
constexpr GUID kFolderDialogGuid = {0x6f1d6b52, 0x3c1e, 0x4a8e, {0x9b, 0x21, 0x48, 0x45, 0x4d, 0x46, 0x4c, 0x44}};
constexpr GUID kExportDialogGuid = {0x6f1d6b52, 0x3c1e, 0x4a8e, {0x9b, 0x21, 0x48, 0x45, 0x4d, 0x45, 0x58, 0x50}};

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
fs::path knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) ::CoTaskMemFree(p);
    return out;
}
// %APPDATA%\HEM Digital Console — settings, the last session, the recovery copy; the installer never touches it.
fs::path dataDir(const fs::path& overrideDir) {
    fs::path out = !overrideDir.empty() ? overrideDir : knownFolder(FOLDERID_RoamingAppData) / L"HEM Digital Console";
    if (out.empty()) out = exeDir();
    std::error_code ec;
    fs::create_directories(out, ec);
    return out;
}
fs::path downloadsFolder() {
    fs::path d = knownFolder(FOLDERID_Downloads);
    std::error_code ec;
    if (d.empty() || !fs::is_directory(d, ec)) d = knownFolder(FOLDERID_Documents);
    return d;
}
std::string u8(const fs::path& p) { const auto s = p.u8string(); return std::string(s.begin(), s.end()); }
fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
int gSlowSaveMs = 0;   // test only (--test-slow-save): a pause after every 1 KB, so a test can kill the app mid-save
// Safe save: the text goes to "<file>.saving", is flushed to the disk, then replaces the file in one step
// (MoveFileEx, write-through). A crash or power cut at any point leaves either the old file or the new one, never half.
bool writeFile(const fs::path& p, const std::string& text, bool slow = false) {
    const fs::path tmp = p.wstring() + L".saving";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    const std::size_t chunk = slow && gSlowSaveMs > 0 ? 1024 : text.size();
    for (std::size_t at = 0; ok && at < text.size(); at += chunk) {
        const DWORD n = static_cast<DWORD>(std::min(chunk, text.size() - at));
        DWORD wrote = 0;
        ok = ::WriteFile(h, text.data() + at, n, &wrote, nullptr) && wrote == n;
        if (slow && gSlowSaveMs > 0) ::Sleep(static_cast<DWORD>(gSlowSaveMs));
    }
    ok = ok && ::FlushFileBuffers(h);
    ::CloseHandle(h);
    if (ok) ok = ::MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!ok) ::DeleteFileW(tmp.c_str());
    return ok;
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

// ---- Windows dialogs (Common Item Dialog) ----
// The dialog opens where it was last used for the same purpose (SetClientGuid); the very first time in `defFolder`
// (SetDefaultFolder, as Microsoft recommends, so the user's last folder is never overridden).
void setDefaultFolder(IFileDialog* dlg, const fs::path& folder) {
    std::error_code ec;
    if (folder.empty() || !fs::is_directory(folder, ec)) return;
    IShellItem* f = nullptr;
    if (SUCCEEDED(::SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetDefaultFolder(f); f->Release(); }
}
struct Filter { const wchar_t* name; const wchar_t* spec; };
std::vector<fs::path> askOpen(HWND owner, const wchar_t* title, const GUID& guid, const std::vector<Filter>& filters, bool multi, const fs::path& defFolder) {
    std::vector<fs::path> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    std::vector<COMDLG_FILTERSPEC> spec;
    for (const Filter& f : filters) spec.push_back({f.name, f.spec});
    dlg->SetClientGuid(guid);
    dlg->SetFileTypes(static_cast<UINT>(spec.size()), spec.data());
    dlg->SetFileTypeIndex(1);   // the first filter is the default
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | (multi ? static_cast<FILEOPENDIALOGOPTIONS>(FOS_ALLOWMULTISELECT) : 0u));
    setDefaultFolder(dlg, defFolder);
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
fs::path askSave(HWND owner, const wchar_t* title, const GUID& guid, const wchar_t* filterName, const wchar_t* pattern, const wchar_t* ext,
                 const std::wstring& name, const fs::path& defFolder, const fs::path& forceFolder = {}) {
    fs::path out;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}};
    dlg->SetClientGuid(guid);
    dlg->SetFileTypes(1, filters);
    dlg->SetDefaultExtension(ext);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!name.empty()) dlg->SetFileName(name.c_str());
    setDefaultFolder(dlg, defFolder);
    std::error_code ec;
    if (!forceFolder.empty() && fs::is_directory(forceFolder, ec)) {   // SAVE AS of a saved session: next to it
        IShellItem* f = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(forceFolder.c_str(), nullptr, IID_PPV_ARGS(&f)))) { dlg->SetFolder(f); f->Release(); }
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
fs::path askFolder(HWND owner, const wchar_t* title, const GUID& guid, const fs::path& defFolder) {
    fs::path out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    dlg->SetClientGuid(guid);
    dlg->SetTitle(title);
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    setDefaultFolder(dlg, defFolder);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* it = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) { out = path; ::CoTaskMemFree(path); }
        if (it) it->Release();
    }
    dlg->Release();
    return out;
}
// A task dialog with our own buttons; returns the id of the button pressed (0 = closed).
int askButtons(HWND owner, const std::wstring& title, const std::wstring& text, const std::vector<std::pair<int, std::wstring>>& buttons, int def,
               PCWSTR icon = TD_WARNING_ICON) {
    std::vector<TASKDIALOG_BUTTON> b;
    for (const auto& [id, t] : buttons) b.push_back({id, t.c_str()});
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof(c);
    c.hwndParent = owner;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.pszWindowTitle = kAppName;
    c.pszMainIcon = icon;
    c.pszMainInstruction = title.c_str();
    c.pszContent = text.c_str();
    c.cButtons = static_cast<UINT>(b.size());
    c.pButtons = b.data();
    c.nDefaultButton = def;
    int pressed = 0;
    if (FAILED(::TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return 0;
    return pressed == IDCANCEL ? 0 : pressed;
}

struct Options {
    double exitAfter = 0;
    fs::path screenshot, session, dataDir;
    bool offline = false;
    bool maximized = false;   // test runs: open maximized like a normal start (to check the real screen size)
    int page = -1;
    int winW = 0, winH = 0;   // test runs: --window WxH = client size in pixels, not maximized
    bool testHooks = false;   // --test-hooks: answer WM_COPYDATA commands (UI automation tests)
    fs::path clickTest;       // --click-test <report>: press every control with real window messages, write a report, exit
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
        else if (k == L"--zoom" && i + 1 < argc) ++i;   // older test scripts: the console always fits now
        else if (k == L"--maximized") o.maximized = true;
        else if (k == L"--window" && i + 1 < argc) { o.winW = static_cast<int>(std::wcstol(argv[++i], nullptr, 10)); const wchar_t* x = std::wcschr(argv[i], L'x'); o.winH = x ? static_cast<int>(std::wcstol(x + 1, nullptr, 10)) : 0; }
        else if (k == L"--data-dir" && i + 1 < argc) o.dataDir = argv[++i];
        else if (k == L"--test-hooks") o.testHooks = true;
        else if (k == L"--test-slow-save" && i + 1 < argc) gSlowSaveMs = static_cast<int>(std::wcstol(argv[++i], nullptr, 10));
        else if (k == L"--click-test" && i + 1 < argc) o.clickTest = argv[++i];
        else if (!k.empty() && k[0] != L'-') o.session = k;
    }
    if (argv) ::LocalFree(argv);
    if (!o.testHooks) gSlowSaveMs = 0;
    return o;
}

using Clock = std::chrono::steady_clock;

std::wstring listNames(const std::vector<fs::path>& files, std::size_t most = 20) {
    std::wstring s;
    for (std::size_t i = 0; i < files.size() && i < most; ++i) s += L"\n  \x2022 " + files[i].filename().wstring();
    if (files.size() > most) s += L"\n  ... and " + std::to_wstring(files.size() - most) + L" more";
    return s;
}

// ---- the app ----
struct App {
    explicit App(const fs::path& dir) : data(dataDir(dir)) {}
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
    fs::path session, data;
    Settings settings;
    std::vector<fs::path> recent;
    std::array<hc::console::TapTempo, kPlayerSlots> taps;
    std::string notice;
    Clock::time_point noticeUntil{}, resetUntil{};
    std::string resetBackup, savedSnapshot;
    std::string meterRecalled;   // the meter settings as last recalled (EDITED shows once they differ)
    bool wasPlaying = false;
    bool testHooks = false;
    HWND hwnd = nullptr;

    // METER LIBRARY: the user slots live in %APPDATA%\HEM Digital Console\meter-presets.ini ("userN.<id>=value"),
    // so they belong to the console, not to one session (like the presets folder on the reference unit).
    void meterRecall() {
        auto& mb = rig.meter();
        const int k = st.meterLibSlot;
        if (!U::meterLibIsUser(k)) {
            mb.applyFactoryPreset(k);
        } else {
            const std::string prefix = "user" + std::to_string(k - hc::console::MeterBridge::factoryPresetCount() + 1) + ".";
            std::istringstream in(readFile(data / L"meter-presets.ini"));
            std::string line, body;
            while (std::getline(in, line)) if (line.starts_with(prefix)) body += line.substr(prefix.size()) + "\n";
            if (body.empty()) { say(std::string(U::meterLibName(k)) + " is empty: STORE into it first"); return; }
            mb.applySettingsText(body);
        }
        st.meterPreset = U::meterLibName(k);
        meterRecalled = mb.settingsText();
        st.meterEdited = false;
        settings.set("meter.preset", st.meterPreset);
        settings.set("meter.recalled", "");
        say(std::string("METER: ") + st.meterPreset + " recalled", 4);
    }
    void meterStore() {
        const int k = st.meterLibSlot;
        if (!U::meterLibIsUser(k)) { say("Factory presets can't be overwritten: pick a USER slot"); return; }
        const std::string prefix = "user" + std::to_string(k - hc::console::MeterBridge::factoryPresetCount() + 1) + ".";
        std::istringstream in(readFile(data / L"meter-presets.ini"));
        std::string line, out = "; HEM Digital Console - meter LIBRARY user presets\n";
        while (std::getline(in, line)) if (!line.starts_with(prefix) && !line.starts_with(";") && !line.empty()) out += line + "\n";
        std::istringstream cur(rig.meter().settingsText());
        while (std::getline(cur, line)) out += prefix + line + "\n";
        if (!writeFile(data / L"meter-presets.ini", out)) { say("Could not save the meter preset"); return; }
        st.meterPreset = U::meterLibName(k);
        meterRecalled = rig.meter().settingsText();
        st.meterEdited = false;
        settings.set("meter.preset", st.meterPreset);
        say(std::string("METER: stored in ") + st.meterPreset, 4);
    }

    void say(std::string t, int seconds = 8) { notice = std::move(t); noticeUntil = Clock::now() + std::chrono::seconds(seconds); }
    [[nodiscard]] hc::console::ConsoleProcessor& player() { return host.player(); }
    // What a save would write (controls + stem files, loaded or still loading): compared to find unsaved changes.
    [[nodiscard]] std::string snapshot() {
        const auto d = sessionData();
        std::string s = d.rigState;
        for (const auto& f : d.files) s += u8(f) + "\n";
        s += std::to_string(d.listen) + (d.loop ? "L" : "-");
        return s;
    }
    [[nodiscard]] bool dirty() { return savedSnapshot != snapshot(); }
    [[nodiscard]] fs::path songFolder() const {
        if (!session.empty()) return session.parent_path();
        for (const auto& f : files) if (!f.empty()) return f.parent_path();
        for (const auto& f : loading) if (!f.empty()) return f.parent_path();
        return downloadsFolder();
    }
    [[nodiscard]] std::wstring songName() const {
        if (!session.empty()) return session.stem().wstring();
        for (const auto& f : files) if (!f.empty()) return f.parent_path().filename().wstring();
        for (const auto& f : loading) if (!f.empty()) return f.parent_path().filename().wstring();
        return L"HEM Mix";
    }

    // ---- recent sessions (settings: recent1 .. recent10) ----
    void loadRecent() {
        recent.clear();
        for (int k = 1; k <= hc::console::kRecentMax; ++k) {
            const std::string v = settings.get("recent" + std::to_string(k));
            if (!v.empty()) recent.push_back(fromU8(v));
        }
    }
    void storeRecent() {
        for (int k = 1; k <= hc::console::kRecentMax; ++k)
            settings.set("recent" + std::to_string(k), k <= static_cast<int>(recent.size()) ? u8(recent[static_cast<std::size_t>(k - 1)]) : std::string());
        settings.save();
    }
    void addRecent(const fs::path& f) { hc::console::pushRecent(recent, f); storeRecent(); }

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
    // A new set of stems (LOAD STEMS, OPEN with audio files, several files dropped): the 13 channels are emptied,
    // Suno stems go to their own channels, the rest fill the free channels in name order.
    void loadStemSet(const std::vector<fs::path>& in) {
        const auto list = media::collectAudioFiles(in);
        if (list.empty()) { say("No WAV, MP3 or FLAC files there"); return; }
        const hc::console::StemPlan plan = hc::console::planStems(list);
        for (int k = 0; k < hc::console::ConsoleRig::kChannels; ++k) clearSlot(k);
        int used = 0;
        for (int k = 0; k < hc::console::ConsoleRig::kChannels; ++k)
            if (!plan.channel[static_cast<std::size_t>(k)].empty()) { loadSlot(k, plan.channel[static_cast<std::size_t>(k)]); ++used; }
        // a whole-song mix / master among the stems: into EXT 1 as a reference (off the main mix; MONITOR SOURCE EXT 1
        // to hear it), never onto a channel where it would double the song
        std::vector<fs::path> notLoaded = plan.leftOut;
        std::string mixNote;
        if (!plan.fullMix.empty()) {
            clearSlot(hc::console::kExt1);
            loadSlot(hc::console::kExt1, plan.fullMix.front());
            mixNote = ". Full mix " + u8(plan.fullMix.front().filename()) + " is in EXT 1 (reference, not in the mix)";
            for (std::size_t i = 1; i < plan.fullMix.size(); ++i) notLoaded.push_back(plan.fullMix[i]);
        }
        if (!notLoaded.empty()) {
            say(std::to_string(notLoaded.size()) + " file(s) not loaded" + mixNote, 20);
            if (!gUnattended)
                askButtons(hwnd, std::to_wstring(notLoaded.size()) + L" file(s) were not loaded",
                           L"Loaded " + std::to_wstring(used) + L" stems on the desk" +
                               (plan.fullMix.empty() ? std::wstring() : L" and the full mix \x201C" + plan.fullMix.front().filename().wstring() + L"\x201D in EXT 1 (a reference, not in the mix)") +
                               L".\n\nNOT loaded:" + listNames(notLoaded) +
                               (plan.leftOut.empty() ? std::wstring() : L"\n\nThe desk has 13 channels.") +
                               (plan.fullMix.size() > 1 ? L"\nOther full mixes / masters are not put on the desk." : L"") +
                               L"\n\nTo use one of them, drop it onto a channel strip (it replaces that channel).",
                           {{100, L"OK"}}, 100, TD_INFORMATION_ICON);
        } else {
            say("Loading " + std::to_string(used) + " stem(s)" + (plan.byName > 0 ? ": " + std::to_string(plan.byName) + " placed by stem name, the rest in name order" : " in name order") + mixNote, 12);
        }
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
    void markSaved() { savedSnapshot = snapshot(); st.dirty = false; }
    void newSession() {
        for (int k = 0; k < kPlayerSlots; ++k) clearSlot(k);
        rig.setDefaults();
        player().stop();
        session.clear();
        st.listen = 0; player().setListen(0);
        player().setLoop(false);
        markSaved();
        say("New session: every control at its start position, no stems");
    }
    // Opens a session file. quiet = the console's own files at start (last session / recovery): no message, the
    // session path is set by the caller. Moved stems: asks where they are (relink by name).
    bool openSession(const fs::path& f, bool quiet = false) {
        std::error_code ec;
        if (!fs::exists(f, ec)) { if (!quiet) say("Can't open " + u8(f.filename()) + ": the file is not there any more"); return false; }
        const std::string text = readFile(f);
        std::vector<int> missing;
        auto s = hc::console::readSession(text, f, &missing);
        if (!s) {
            if (!quiet) {
                say("Can't open " + u8(f.filename()) + ": " + s.error(), 15);
                if (!gUnattended) askButtons(hwnd, L"Can't open " + f.filename().wstring(), fromUtf8(s.error()), {{100, L"OK"}}, 100, TD_ERROR_ICON);
            }
            return false;
        }
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
            addRecent(f);
        }
        markSaved();
        if (!missing.empty()) relink(*s, missing, f);
        else if (!quiet) say("Opened " + u8(f.filename()));
        return true;
    }
    // Stems a session asks for that are not where they were (moved, renamed folder, other drive).
    void relink(const hc::console::SessionData& s, const std::vector<int>& missing, const fs::path& sessionFile) {
        std::vector<fs::path> wanted;
        for (int k : missing) wanted.push_back(s.wanted[static_cast<std::size_t>(k)]);
        int found = 0;
        if (!gUnattended || testHooks) {
            const int b = askButtons(hwnd, std::to_wstring(missing.size()) + L" stem file(s) of this session were not found",
                                     L"They may have been moved or renamed. Missing:" + listNames(wanted) +
                                         L"\n\nShow me the folder they are in now and I'll find them by name.",
                                     {{101, L"Find the folder..."}, {102, L"Open without them"}}, 101);
            if (b == 101) {
                const fs::path folder = askFolder(hwnd, L"Where are the stems now?", kFolderDialogGuid, sessionFile.parent_path());
                if (!folder.empty()) {
                    const auto hits = hc::console::relinkStems(wanted, folder);
                    for (std::size_t i = 0; i < missing.size(); ++i)
                        if (!hits[i].empty()) { loadSlot(missing[i], hits[i]); ++found; }
                }
            }
        }
        const int still = static_cast<int>(missing.size()) - found;
        if (found > 0) say("Found " + std::to_string(found) + " moved stem(s)" + (still > 0 ? ", " + std::to_string(still) + " still missing" : "") + ". SAVE to keep the new places", 15);
        else say(std::to_string(missing.size()) + " stem file(s) not found: the session opened without them", 15);
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
        if (!writeFile(f, hc::console::writeSession(sessionData(), f), true)) {
            say("Can't save " + u8(f) + " (is the folder read-only or the disk full?)", 15);
            if (!gUnattended) askButtons(hwnd, L"The session was NOT saved", L"Could not write " + f.wstring() + L"\nTry SAVE AS to another folder.", {{100, L"OK"}}, 100, TD_ERROR_ICON);
            return false;
        }
        session = f;
        addRecent(f);
        markSaved();
        settings.set("session", u8(session));
        settings.set("dirty", "0");
        settings.save();
        say("Saved " + u8(f.filename()));
        return true;
    }
    // SAVE (asks for a name the first time) / SAVE AS. Returns true once the session is on disk.
    bool save(bool as) {
        fs::path f = session;
        if (as || f.empty()) {
            f = askSave(hwnd, as ? L"Save the session as" : L"Save the session", kSaveDialogGuid, L"HEM Digital Console session (*.hemc)", L"*.hemc", L"hemc",
                        songName(), downloadsFolder(), as && !session.empty() ? session.parent_path() : fs::path());
            if (f.empty()) return false;
        }
        return saveSessionTo(f);
    }
    // Before NEW, OPEN (a session) and closing: false = the user cancelled.
    bool confirmDiscard(const wchar_t* what) {
        if (!dirty()) return true;
        const int b = askButtons(hwnd, L"Save changes to " + (session.empty() ? std::wstring(L"this untitled session") : session.stem().wstring()) + L"?",
                                 std::wstring(L"You have changes that are not saved. ") + what, {{201, L"Save"}, {202, L"Don't save"}, {203, L"Cancel"}}, 201);
        if (b == 201) return save(false);
        return b == 202;
    }
    // The last state at a clean close (restored at the next start).
    void writeLastState() {
        const fs::path f = data / L"autosave.hemc";
        (void)writeFile(f, hc::console::writeSession(sessionData(), f));
        settings.set("session", u8(session));
        settings.set("dirty", dirty() ? "1" : "0");
        settings.save();
    }
    // Every 3 minutes while running: offered after a crash.
    void writeRecovery() {
        const fs::path f = data / L"recovery.hemc";
        (void)writeFile(f, hc::console::writeSession(sessionData(), f));
        settings.set("recovery.session", u8(session));
        settings.save();
    }

    // ---- OPEN: sessions and / or audio ----
    void openFiles(const std::vector<fs::path>& picked) {
        std::vector<fs::path> sessions, audio;
        for (const auto& f : picked) {
            std::wstring ext = f.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext == L".hemc") sessions.push_back(f); else audio.push_back(f);
        }
        if (!sessions.empty()) {
            if (!confirmDiscard(L"Open another session?")) return;
            openSession(sessions.front());
            if (sessions.size() > 1 || !audio.empty()) say("Opened " + u8(sessions.front().filename()) + " (one session at a time: the other files were not opened)", 12);
            return;
        }
        if (!audio.empty()) loadStemSet(audio);
    }
    void openDialog() {
        const auto f = askOpen(hwnd, L"Open a session or audio files", kOpenDialogGuid,
                               {{L"Sessions + Audio", L"*.hemc;*.wav;*.wave;*.mp3;*.flac"}, {L"Sessions (*.hemc)", L"*.hemc"}, {L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}},
                               true, downloadsFolder());
        if (!f.empty()) openFiles(f);
    }
    void recentMenu() {
        HMENU m = ::CreatePopupMenu();
        if (!m) return;
        int id = 1;
        for (const auto& f : recent) {
            const std::wstring item = std::to_wstring(id % 10) + L"  " + f.stem().wstring() + L"    (" + f.parent_path().wstring() + L")";
            ::AppendMenuW(m, MF_STRING, static_cast<UINT_PTR>(id++), item.c_str());
        }
        if (recent.empty()) ::AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No recent sessions yet");
        else { ::AppendMenuW(m, MF_SEPARATOR, 0, nullptr); ::AppendMenuW(m, MF_STRING, 100, L"Clear this list"); }
        POINT pt{};
        ::GetCursorPos(&pt);
        ::SetForegroundWindow(hwnd);
        const int cmd = static_cast<int>(::TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, hwnd, nullptr));
        ::DestroyMenu(m);
        if (cmd == 100) { recent.clear(); storeRecent(); say("Recent sessions list cleared"); return; }
        if (cmd < 1 || cmd > static_cast<int>(recent.size())) return;
        const fs::path f = recent[static_cast<std::size_t>(cmd - 1)];
        std::error_code ec;
        if (!fs::exists(f, ec)) {
            say("Not there any more: " + u8(f) + " (taken off the list)", 12);
            std::erase(recent, f);
            storeRecent();
            return;
        }
        if (!confirmDiscard(L"Open another session?")) return;
        openSession(f);
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
            job.out = askFolder(hwnd, L"PRINT STEMS: pick a folder for the channel files", kExportDialogGuid, songFolder());
            if (job.out.empty()) return;
            job.out /= songName() + L" stems";
        } else {
            job.out = askSave(hwnd, L"Export the mix (MAIN OUT)", kExportDialogGuid, L"WAV audio", L"*.wav", L"wav", songName() + L" (HEM mix)", songFolder());
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
            const auto f = askOpen(hwnd, L"Load stems (up to 13: Suno stems go to their own channels)", kStemsDialogGuid,
                                   {{L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}}, true, downloadsFolder());
            if (!f.empty()) loadStemSet(f);
            break;
        }
        case U::Act::LoadSlot: {
            const auto f = askOpen(hwnd, e.arg == hc::console::kExt1 ? L"Load a file into EXT 1" : e.arg == hc::console::kExt2 ? L"Load a file into EXT 2" : L"Load a file into this channel",
                                   kStemsDialogGuid, {{L"Audio (WAV, MP3, FLAC)", L"*.wav;*.wave;*.mp3;*.flac"}}, false, downloadsFolder());
            if (!f.empty()) loadSlot(e.arg, f.front());
            break;
        }
        case U::Act::ClearSlot: clearSlot(e.arg); break;
        case U::Act::Play:
            if (p.songFrames() == 0) { say("Load stems first (LOAD STEMS, OPEN, or drag files onto the desk)"); break; }
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
            if (confirmDiscard(L"Start a new session?")) newSession();
            break;
        case U::Act::OpenSession: openDialog(); break;
        case U::Act::Recent: recentMenu(); break;
        case U::Act::SaveSession: (void)save(false); break;
        case U::Act::SaveAs: (void)save(true); break;
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
        case U::Act::MeterRecall: meterRecall(); break;
        case U::Act::MeterStore: meterStore(); break;
        case U::Act::ResetAll: resetPressed(); break;
        case U::Act::Page: settings.set("page", std::to_string(static_cast<int>(screen.page()))); break;
        default: break;
        }
    }
};

// ---- window size, position and maximized state (settings "window=left,top,right,bottom,showCmd") ----
std::string placementText(HWND h) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!::GetWindowPlacement(h, &wp)) return {};
    char b[96];
    std::snprintf(b, sizeof b, "%ld,%ld,%ld,%ld,%u", wp.rcNormalPosition.left, wp.rcNormalPosition.top, wp.rcNormalPosition.right, wp.rcNormalPosition.bottom,
                  wp.showCmd == SW_SHOWMAXIMIZED || (wp.flags & WPF_RESTORETOMAXIMIZED) ? 1u : 0u);
    return b;
}
// False when there is nothing saved or the saved spot is no longer on any screen (then: maximized).
bool applyPlacement(HWND h, const std::string& s) {
    long l = 0, t = 0, r = 0, b = 0;
    unsigned maxed = 0;
    if (std::sscanf(s.c_str(), "%ld,%ld,%ld,%ld,%u", &l, &t, &r, &b, &maxed) != 5 || r - l < 200 || b - t < 150) return false;
    RECT rc{l, t, r, b};
    if (!::MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) return false;   // that monitor is gone
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    wp.rcNormalPosition = rc;
    wp.showCmd = maxed ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    return ::SetWindowPlacement(h, &wp) != FALSE;
}

// ---- --test-hooks: what UI automation tests ask for (WM_COPYDATA, GlWindow::kCommandTag) ----
// "report <file>": window, screen and every visible control on the page in SCREEN pixels, stems, title, dirty.
// "state <file>": every saved control (the rig's text) plus listen / loop.  "page <n>": show tab n (0-based tab order).
void writeReport(App& a, U::ConsoleScreen& screen, ui::GlWindow& window, const fs::path& out) {
    std::string s;
    char b[512];
    wchar_t title[512] = {};
    ::GetWindowTextW(a.hwnd, title, 512);
    s += "title=" + toUtf8(title) + "\n";
    s += std::string("dirty=") + (a.dirty() ? "1" : "0") + "\n";
    s += "session=" + u8(a.session) + "\n";
    RECT cr{};
    ::GetClientRect(a.hwnd, &cr);
    POINT o{0, 0};
    ::ClientToScreen(a.hwnd, &o);
    std::snprintf(b, sizeof b, "client=%ld,%ld,%ld,%ld\n", o.x, o.y, o.x + cr.right, o.y + cr.bottom); s += b;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(::MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    std::snprintf(b, sizeof b, "monitor=%ld,%ld,%ld,%ld\nwork=%ld,%ld,%ld,%ld\n", mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right, mi.rcMonitor.bottom,
                  mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom); s += b;
    std::snprintf(b, sizeof b, "dpi=%u\nscale=%.4f\nmaximized=%d\npage=%s\n", ::GetDpiForWindow(a.hwnd), static_cast<double>(screen.canvas().scale),
                  ::IsZoomed(a.hwnd) ? 1 : 0, U::pageName(screen.page())); s += b;
    s += "placement=" + placementText(a.hwnd) + "\n";
    s += "status=" + a.st.status + "\n";
    for (int k = 0; k < kPlayerSlots; ++k) {
        const auto i = static_cast<std::size_t>(k);
        s += "stem" + std::to_string(k + 1) + "=" + u8(!a.files[i].empty() ? a.files[i] : a.loading[i]) + (a.files[i].empty() && !a.loading[i].empty() ? " (loading)" : "") + "\n";
    }
    for (std::size_t r = 0; r < a.recent.size(); ++r) s += "recent" + std::to_string(r + 1) + "=" + u8(a.recent[r]) + "\n";
    const auto& cv = screen.canvas();
    int n = 0;
    for (const U::Widget& w : screen.currentPage().widgets) {
        if (!U::widgetVisible(w)) continue;
        const char* kind = w.kind == U::Kind::Button ? "button" : w.kind == U::Kind::Switch ? "switch" : w.kind == U::Kind::Knob ? "knob" :
                           w.kind == U::Kind::Stepped ? "stepped" : w.kind == U::Kind::Fader ? "fader" : w.kind == U::Kind::Selector ? "selector" :
                           w.kind == U::Kind::Seek ? "seek" : w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName ? "stem" : nullptr;
        if (!kind) continue;
        const long x0 = o.x + std::lround(cv.toPixelX(w.r.x)), y0 = o.y + std::lround(cv.toPixelY(w.r.y));
        const long x1 = o.x + std::lround(cv.toPixelX(w.r.x + w.r.w)), y1 = o.y + std::lround(cv.toPixelY(w.r.y + w.r.h));
        std::string label = w.kind == U::Kind::Button ? w.label : (!w.label.empty() ? w.label : w.ctl.valid() ? std::string(w.ctl.info->name) : std::string());
        std::string key = w.ctl.valid() ? std::string(w.ctl.info->id) : std::string();
        for (char& c : label) if (c == '|' || c == '\n') c = ' ';
        std::snprintf(b, sizeof b, "widget=%d|%s|%s|%s|%ld|%ld|%ld|%ld|%d\n", n++, kind, label.c_str(), key.c_str(), x0, y0, x1, y1, w.ctl.valid() ? 1 : 0);
        s += b;
    }
    (void)window;
    (void)writeFile(out, s);
}
// ---- --click-test: every control pressed with real window messages ------------------------------------------------
// The same path a mouse takes (WM_LBUTTONDOWN / WM_MOUSEMOVE / WM_LBUTTONUP / WM_MOUSEWHEEL -> GlWindow -> screen ->
// controller -> rig), on every page and every panel that comes and goes. Actions are recorded, not carried out (no file
// dialogs, nothing loaded or saved). Writes one line per control and returns 0 when every control responded.
template <typename Dispatch>
int runClickTest(ui::GlWindow& window, U::ConsoleScreen& screen, App& a, std::vector<U::ActionEvent>& actions, const Dispatch& dispatch,
                 const fs::path& reportPath) {
    using namespace hc::console;
    std::string report = "HEM Digital Console " HEM_CONSOLE_VERSION " - control check (real window messages)\n";
    int tested = 0, failed = 0;
    std::string err;
    auto settle = [&] {
        window.pump();
        const int w = window.pixelWidth(), h = window.pixelHeight();
        if (w > 0 && h > 0) screen.resize(w, h, &err);
        for (const auto& e : window.takeEvents()) dispatch(e);
        screen.render(0);
        window.swap();
    };
    for (int k = 0; k < 10; ++k) settle();
    {   // the real cursor away from the screen edges (a press at the edge wraps the cursor round for long knob drags)
        RECT rc{};
        ::GetWindowRect(a.hwnd, &rc);
        ::SetCursorPos((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
    }
    char head[160];
    std::snprintf(head, sizeof head, "window %d x %d pixels, zoom %.2f, 1 design unit = %.3f px\n\n", window.pixelWidth(), window.pixelHeight(),
                  static_cast<double>(screen.zoom()), static_cast<double>(screen.canvas().scale));
    report += head;
    const HWND h = a.hwnd;
    auto msg = [&](UINT m, WPARAM wp, int x, int y) { ::SendMessageW(h, m, wp, MAKELPARAM(x, y)); };
    auto wheelMsg = [&](int x, int y, int delta) {
        POINT pt{x, y};
        ::ClientToScreen(h, &pt);
        ::SendMessageW(h, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(delta))), MAKELPARAM(pt.x, pt.y));
    };
    const std::string meterBefore = a.rig.meter().settingsText();
    const std::string rigBefore = a.rig.saveText();
    std::set<std::pair<int, int>> done;   // (page, widget) already checked

    auto kindName = [](U::Kind k) {
        switch (k) {
        case U::Kind::Knob: return "knob"; case U::Kind::Stepped: return "stepped knob"; case U::Kind::Switch: return "switch";
        case U::Kind::Fader: return "fader"; case U::Kind::Selector: return "selector"; case U::Kind::Button: return "button";
        case U::Kind::Seek: return "song bar"; case U::Kind::Custom: return "name strip"; default: return "?";
        }
    };
    // one page in one state of its changing panels
    auto pass = [&](U::PageId pg, const std::string& variant) {
        screen.setPage(pg);
        screen.waitForPlate(30);
        settle(); settle();
        const auto& ws = screen.currentPage().widgets;
        const U::AppState saved = a.st;
        for (std::size_t i = 0; i < ws.size(); ++i) {
            const U::Widget& w = ws[i];
            if (!U::widgetVisible(w)) continue;
            const bool click = w.kind == U::Kind::Knob || w.kind == U::Kind::Stepped || w.kind == U::Kind::Switch || w.kind == U::Kind::Fader ||
                               w.kind == U::Kind::Selector || w.kind == U::Kind::Button || w.kind == U::Kind::Seek ||
                               (w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName);
            if (!click || !done.insert({static_cast<int>(pg), static_cast<int>(i)}).second) continue;
            const auto& cv = screen.canvas();
            const int px = static_cast<int>(std::lround(cv.toPixelX(w.r.cx()))), py = static_cast<int>(std::lround(cv.toPixelY(w.r.cy())));
            const float v0 = w.ctl.valid() ? w.ctl.get() : 0.0f;
            const float n0 = U::widgetNorm(w);
            actions.clear();
            bool ok = false, wheelOk = true;
            std::string what;
            switch (w.kind) {
            case U::Kind::Knob:
            case U::Kind::Stepped: {
                const int dy = static_cast<int>(std::lround(U::Controller::kDragUnitsForFullRange * 0.3f * cv.scale)) * (n0 < 0.5f ? -1 : 1);
                msg(WM_LBUTTONDOWN, MK_LBUTTON, px, py); settle();
                for (int s = 1; s <= 4; ++s) { msg(WM_MOUSEMOVE, MK_LBUTTON, px, py + dy * s / 4); settle(); }
                msg(WM_LBUTTONUP, 0, px, py + dy); settle();
                const float v1 = w.ctl.get();
                ok = v1 != v0;
                w.ctl.set(v0); settle();
                wheelMsg(px, py, n0 < 0.5f ? 120 : -120); settle();
                wheelOk = w.ctl.get() != v0;
                w.ctl.set(v0);
                what = "drag " + U::widgetValueText(w) + " -> moved, wheel " + (wheelOk ? "moved" : "DID NOT MOVE");
                break;
            }
            case U::Kind::Fader: {
                const float t = n0 < 0.5f ? 0.85f : 0.2f;
                const int fy = static_cast<int>(std::lround(cv.toPixelY(w.r.y + w.r.h * (1.0f - t))));
                msg(WM_LBUTTONDOWN, MK_LBUTTON, px, fy); settle();
                msg(WM_LBUTTONUP, 0, px, fy); settle();
                ok = w.ctl.get() != v0;
                w.ctl.set(v0); settle();
                wheelMsg(px, py, n0 < 0.5f ? 120 : -120); settle();
                wheelOk = w.ctl.get() != v0;
                w.ctl.set(v0);
                what = std::string("click on the slot, wheel ") + (wheelOk ? "moved" : "DID NOT MOVE");
                break;
            }
            case U::Kind::Switch:
            case U::Kind::Selector: {
                msg(WM_LBUTTONDOWN, MK_LBUTTON, px, py); settle();
                msg(WM_LBUTTONUP, 0, px, py); settle();
                ok = w.ctl.get() != v0;
                what = std::string(w.kind == U::Kind::Switch ? "click toggles" : "click steps") + (ok ? "" : " - NOTHING HAPPENED");
                w.ctl.set(v0);
                break;
            }
            default: {   // buttons, the song bar, name strips: the action they send
                msg(WM_LBUTTONDOWN, MK_LBUTTON, px, py); settle();
                msg(WM_LBUTTONUP, 0, px, py); settle();
                const U::Act want = w.kind == U::Kind::Seek ? U::Act::Seek : w.kind == U::Kind::Custom ? U::Act::LoadSlot : w.act;
                for (const auto& e : actions) if (e.act == want && (w.kind == U::Kind::Seek || e.arg == U::widgetArg(w))) ok = true;
                what = ok ? "click sends its action" : "click sent NOTHING";
                break;
            }
            }
            // undo what the press did to the screen (page, panels) before the next control
            a.st.meterTab = saved.meterTab; a.st.bigFirst = saved.bigFirst; a.st.toolTarget = saved.toolTarget; a.st.meterLibSlot = saved.meterLibSlot;
            screen.setPage(pg);
            settle(); settle();   // e.g. after ZOOM was stepped and put back: the next control is found where it really is
            ++tested;
            const bool good = ok && wheelOk;
            if (!good) ++failed;
            std::string name = !w.tip.empty() ? w.tip : w.ctl.valid() ? std::string(w.ctl.info->name) : w.label;
            if (name.size() > 70) name = name.substr(0, 67) + "...";
            char line[400];
            std::snprintf(line, sizeof line, "%-4s %-9s %-26s %-13s %-70s %s\n", good ? "OK" : "FAIL", U::pageName(pg), variant.c_str(), kindName(w.kind), name.c_str(), what.c_str());
            report += line;
        }
    };
    for (U::PageId pg : U::kTabOrder) {
        switch (pg) {
        case U::PageId::StepEq:
            for (int m : {ConsoleRig::EqSpecial, ConsoleRig::EqStandard}) { a.rig.setEqModel(m); pass(pg, m ? "model SPECIAL EDITION" : "model STANDARD"); }
            a.rig.setEqModel(ConsoleRig::EqSpecial);
            break;
        case U::PageId::Meter:
            a.rig.meter().set(meter_ids::Layout, 6);
            for (int v = 0; v < 3; ++v)
                for (int t = 0; t < 5; ++t) {
                    a.rig.meter().set(meter_ids::View, static_cast<float>(v));
                    a.st.meterTab = t;
                    a.st.meterLibSlot = t == 4 ? MeterBridge::factoryPresetCount() : 0;
                    pass(pg, std::string("view ") + (v == 0 ? "RADAR" : v == 1 ? "RTA" : "SCOPE") + ", EDIT tab " + std::to_string(t + 1));
                }
            a.st.meterTab = 0; a.st.meterLibSlot = 0;
            break;
        case U::PageId::Tools:
            for (int t = ToolSlot::Off; t < ToolSlot::kTypeCount; ++t) {
                a.st.toolTarget = 0;
                a.rig.channelTool(0).setType(t);
                pass(pg, std::string("channel 1 tool ") + ToolSlot::typeKey(t));
            }
            a.rig.channelTool(0).setType(ToolSlot::Off);
            break;
        default: pass(pg, "-"); break;
        }
    }
    a.rig.loadText(rigBefore);
    a.rig.meter().applySettingsText(meterBefore);
    char tail[200];
    std::snprintf(tail, sizeof tail, "\n%d controls pressed, %d did not respond -> %s\n", tested, failed, failed ? "FAIL" : "PASS");
    report += tail;
    (void)writeFile(reportPath, report);
    return failed ? 5 : 0;
}


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
    // Per-Monitor V2 DPI awareness comes from the manifest (Microsoft's recommended way), before any window exists.
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

    auto app = std::make_unique<App>(opts.dataDir);
    App& a = *app;
    a.testHooks = opts.testHooks;
    a.settings.load(a.data / L"settings.ini");
    a.loadRecent();

    // Crash check: this marker exists only while a console runs; finding it at start means the last run did not close.
    const fs::path runningMarker = a.data / L"running.marker", recoveryFile = a.data / L"recovery.hemc";
    std::error_code ec;
    const bool crashedLastTime = fs::exists(runningMarker, ec) && opts.clickTest.empty() && opts.exitAfter <= 0.0;
    if (opts.clickTest.empty() && opts.exitAfter <= 0.0) (void)writeFile(runningMarker, "running\n");

    ui::GlWindow window;
    std::string err;
    if (!window.create(L"HEM Digital Console", 1389, 868, false, &err)) { fatal("Could not start the GPU display: " + err + "\nUpdate your graphics driver (OpenGL 3.3 is required)."); return 3; }
    a.hwnd = static_cast<HWND>(window.nativeHandle());
    window.setMinClientSize(kMinClientDipW, kMinClientDipH);
    window.setAppHandlesClose(opts.exitAfter <= 0.0 && opts.clickTest.empty());
    if (opts.winW > 0 && opts.winH > 0) {   // tests: an exact client size
        RECT r{0, 0, opts.winW, opts.winH};
        ::AdjustWindowRectExForDpi(&r, static_cast<DWORD>(::GetWindowLongPtrW(a.hwnd, GWL_STYLE)), FALSE, 0, ::GetDpiForWindow(a.hwnd));
        ::SetWindowPos(a.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_SHOWWINDOW);
    } else if (opts.exitAfter <= 0.0 || opts.maximized) {
        // where it was last time (size, position, maximized); the first time, or if that screen is gone: maximized
        if (opts.maximized || !applyPlacement(a.hwnd, a.settings.get("window"))) ::ShowWindow(a.hwnd, SW_SHOWMAXIMIZED);
        if (opts.exitAfter <= 0.0 && a.settings.get("fullscreen", "0") == "1") window.setFullscreen(true);
    } else {
        ::ShowWindow(a.hwnd, SW_SHOWNORMAL);
    }
    ::UpdateWindow(a.hwnd);

    std::vector<U::ActionEvent> actions;
    a.st.zoom = 1.0f;   // the faceplate always fits the window
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

    // session: from the command line; else after a crash the recovery copy (if wanted); else everything as it was
    // when the console was last closed
    const fs::path lastState = a.data / L"autosave.hemc";
    bool started = false;
    if (!opts.session.empty()) {
        started = a.openSession(opts.session);
    } else if (crashedLastTime && fs::exists(recoveryFile, ec)) {
        auto when = fs::last_write_time(recoveryFile, ec);
        const auto mins = ec ? -1 : std::chrono::duration_cast<std::chrono::minutes>(fs::file_time_type::clock::now() - when).count();
        const int b = askButtons(a.hwnd, L"The console did not close properly last time",
                                 L"A recovery copy of your work was saved " + (mins < 0 ? std::wstring(L"before it stopped") : std::to_wstring(mins) + L" minute(s) ago") +
                                     L" (it is saved every 3 minutes). Recover it?",
                                 {{301, L"Recover my work"}, {302, L"Don't recover"}}, 301, TD_WARNING_ICON);
        if (b == 301 && a.openSession(recoveryFile, true)) {
            a.session = fromU8(a.settings.get("recovery.session"));
            a.savedSnapshot.clear();   // recovered work is not saved yet
            a.say("Recovered your work. SAVE to keep it", 15);
            started = true;
        }
    }
    if (!started && opts.session.empty()) {
        if (fs::exists(lastState, ec) && a.openSession(lastState, true)) {
            a.session = fromU8(a.settings.get("session"));
            if (a.settings.get("dirty", "1") == "1") a.savedSnapshot.clear();   // unsaved changes last time: still unsaved
            a.say("Welcome back: your last session is restored");
        } else {
            a.markSaved();   // nothing to save yet
            a.say("LOAD STEMS, OPEN or drag files onto the desk to start", 12);
        }
    }
    a.st.meterPreset = a.settings.get("meter.preset", "DEFAULT");
    a.meterRecalled = a.rig.meter().settingsText();   // what is on now counts as "as recalled" until something changes
    const int page = opts.page >= 0 ? opts.page : std::atoi(a.settings.get("page", "0").c_str());
    screen.setPage(static_cast<U::PageId>(std::clamp(page, 0, U::kPageCount - 1)));

    const auto startedAt = Clock::now();
    auto lastRecovery = startedAt;
    bool shotTaken = false;
    fs::path shownSession = L"?";
    bool shownDirty = false;
    int frame = 0;
    std::string savedPlacement = a.settings.get("window");
    auto refreshTitle = [&](bool force) {
        if (!force && a.session == shownSession && a.st.dirty == shownDirty) return;
        shownSession = a.session; shownDirty = a.st.dirty;
        window.setTitle(std::wstring(L"HEM Digital Console ") + fromUtf8(HEM_CONSOLE_VERSION) + L"  \x2014  " +
                        (a.session.empty() ? std::wstring(L"untitled") : a.session.stem().wstring()) + (a.st.dirty ? L" *" : L""));
    };
    auto rememberWindow = [&] {
        if (window.fullscreen() || window.minimized() || opts.winW > 0) return;
        const std::string p = placementText(a.hwnd);
        if (!p.empty() && p != savedPlacement) { savedPlacement = p; a.settings.set("window", p); a.settings.save(); }
    };
    // a single file dropped onto a channel goes to that channel only; several files (or a folder) are a new set of stems
    auto dropFiles = [&](const ui::InputEvent& e) {
        if (e.files.size() == 1 && e.files[0].extension() == hc::console::kSessionExtension) {
            if (a.confirmDiscard(L"Open the dropped session?")) a.openSession(e.files[0]);
            return;
        }
        const float x = screen.canvas().toDesignX(e.x), y = screen.canvas().toDesignY(e.y);
        int target = -1;
        for (const auto& w : screen.currentPage().widgets)
            if (w.kind == U::Kind::Custom && w.custom == U::CustomId::StemName && w.r.contains(x, y)) target = w.customArg;
        if (target < 0) target = U::channelAt(screen.page(), a.st.bigFirst, x, y);
        std::error_code e2;
        const bool oneFile = e.files.size() == 1 && !fs::is_directory(e.files[0], e2);
        if (oneFile && target >= 0) {
            if (!media::collectAudioFiles(e.files).empty()) { a.loadSlot(target, e.files[0]); a.say("Loading " + u8(e.files[0].filename()) + " into " + (target < hc::console::ConsoleRig::kChannels ? "channel " + std::to_string(target + 1) : target == hc::console::kExt1 ? std::string("EXT 1") : std::string("EXT 2"))); }
            else a.say("Not a WAV, MP3 or FLAC file: " + u8(e.files[0].filename()));
            return;
        }
        if (oneFile) {   // one file next to the strips: its Suno channel if that is free, else the first free channel
            if (media::collectAudioFiles(e.files).empty()) { a.say("Not a WAV, MP3 or FLAC file: " + u8(e.files[0].filename())); return; }
            const int role = hc::console::stemRoleOf(e.files[0]);
            int ch = role >= 0 ? hc::console::stemRoles()[static_cast<std::size_t>(role)].channel : -1;
            auto freeCh = [&](int k) { return a.files[static_cast<std::size_t>(k)].empty() && a.loading[static_cast<std::size_t>(k)].empty(); };
            if (ch < 0 || !freeCh(ch)) { ch = -1; for (int k = 0; k < hc::console::ConsoleRig::kChannels && ch < 0; ++k) if (freeCh(k)) ch = k; }
            if (ch < 0) { a.say("Every channel is in use: drop the file onto the channel it should replace", 12); return; }
            a.loadSlot(ch, e.files[0]);
            a.say("Loading " + u8(e.files[0].filename()) + " into channel " + std::to_string(ch + 1));
            return;
        }
        a.loadStemSet(e.files);
    };
    auto command = [&](const std::string& c) {
        if (!opts.testHooks) return;
        const auto sp = c.find(' ');
        const std::string verb = c.substr(0, sp), arg = sp == std::string::npos ? std::string() : c.substr(sp + 1);
        if (verb == "report") writeReport(a, screen, window, fromU8(arg));
        else if (verb == "state") (void)writeFile(fromU8(arg), a.rig.saveText() + "app.listen=" + std::to_string(a.st.listen) + "\napp.loop=" + (a.player().loop() ? "1" : "0") + "\n");
        else if (verb == "write-recovery") a.writeRecovery();   // what the 3-minute timer does, now
        else if (verb == "page") screen.setPage(U::kTabOrder[static_cast<std::size_t>(std::clamp(std::atoi(arg.c_str()), 0, U::kPageCount - 1))]);
        else if (verb == "drop") {   // "drop x y|file|file...": the same as files dropped from Explorer at client pixel x, y
            ui::InputEvent e;
            e.type = ui::InputEvent::Type::Drop;
            std::istringstream in(arg);
            std::string part;
            std::getline(in, part, '|');
            std::sscanf(part.c_str(), "%f %f", &e.x, &e.y);
            while (std::getline(in, part, '|')) if (!part.empty()) e.files.push_back(fromU8(part));
            dropFiles(e);
        }
    };
    bool closing = false;
    // every window event goes through here (the main loop and the click test)
    auto dispatch = [&](const ui::InputEvent& e) {
        switch (e.type) {
        case ui::InputEvent::Type::MouseDown: screen.mouseDown(e.x, e.y, e.doubleClick, e.fine); break;
        case ui::InputEvent::Type::RightDown: screen.rightDown(e.x, e.y); break;
        case ui::InputEvent::Type::MouseMove: screen.mouseMove(e.x, e.y, e.fine); break;
        case ui::InputEvent::Type::MouseUp: screen.mouseUp(e.x, e.y); break;
        case ui::InputEvent::Type::Wheel: screen.wheel(e.x, e.y, e.wheel, e.shift, false, e.shift, e.wheelX); break;
        case ui::InputEvent::Type::MiddleDown: screen.middleDown(e.x, e.y); break;
        case ui::InputEvent::Type::MiddleUp: screen.middleUp(e.x, e.y); break;
        case ui::InputEvent::Type::Drop: dropFiles(e); break;
        case ui::InputEvent::Type::Close: closing = true; break;
        case ui::InputEvent::Type::Command: command(e.text); break;
        case ui::InputEvent::Type::Key:
            if (e.key == VK_F11) {
                window.setFullscreen(!window.fullscreen());
                a.settings.set("fullscreen", window.fullscreen() ? "1" : "0");
                break;
            }
            if (e.key == VK_ESCAPE && window.fullscreen()) { window.setFullscreen(false); a.settings.set("fullscreen", "0"); break; }
            if (e.ctrl) {
                if (e.key == 'S') actions.push_back({e.shift ? U::Act::SaveAs : U::Act::SaveSession});
                else if (e.key == 'O') actions.push_back({U::Act::OpenSession});
                else if (e.key == 'N') actions.push_back({U::Act::NewSession});
                else if (e.key == 'E') actions.push_back({U::Act::Export});
                break;
            }
            if (screen.key(e.key, false)) break;
            if (e.key == VK_SPACE) actions.push_back({U::Act::Play});
            else if (e.key == VK_HOME) actions.push_back({U::Act::Rewind});
            else if (e.key == VK_END) actions.push_back({U::Act::Stop});
            else if (e.key == 'L') actions.push_back({U::Act::Loop});
            else if (e.key >= '1' && e.key < '1' + U::kPageCount) screen.setPage(U::kTabOrder[static_cast<std::size_t>(e.key - '1')]);
            break;
        case ui::InputEvent::Type::OpenFile:
            if (!e.files.empty() && a.confirmDiscard(L"Open the other session?")) a.openSession(e.files.front());
            break;
        default: break;
        }
    };

    if (!opts.clickTest.empty()) return runClickTest(window, screen, a, actions, dispatch, opts.clickTest);

    refreshTitle(true);
    while (window.pump()) {
        const int fbW = window.pixelWidth(), fbH = window.pixelHeight();
        if (fbW > 0 && fbH > 0 && !window.minimized() && !screen.resize(fbW, fbH, &err)) { fatal("GPU error: " + err); break; }
        for (const auto& e : window.takeEvents()) dispatch(e);
        for (const auto& e : actions) a.handle(e, screen);
        const bool acted = !actions.empty();
        actions.clear();
        if (closing) {
            closing = false;
            if (a.confirmDiscard(L"Close the console?")) window.requestClose();
        }

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
        if (++frame % 30 == 0 || acted) {
            a.st.dirty = a.dirty();
            a.st.meterEdited = !a.meterRecalled.empty() && a.meterRecalled != a.rig.meter().settingsText();
        }
        if (frame % 60 == 0) rememberWindow();
        // the meter's FOLLOW TRANSPORT / RESET ON PLAY follow the console's own transport
        a.rig.meter().setTransportPlaying(a.st.playing);
        if (a.st.playing && !a.wasPlaying && a.rig.meter().get(hc::console::meter_ids::ResetOnPlay) > 0.5f) a.rig.requestMeterReset();
        a.wasPlaying = a.st.playing;
        a.st.session = a.session.empty() ? "UNTITLED" : u8(a.session.stem());
        refreshTitle(false);
        if (Clock::now() - lastRecovery > kRecoveryEvery) { a.writeRecovery(); lastRecovery = Clock::now(); }

        if (window.minimized()) { ::Sleep(50); continue; }
        screen.render(0);
        window.swap();

        if (opts.exitAfter > 0.0 && std::chrono::duration<double>(Clock::now() - startedAt).count() >= opts.exitAfter) {
            if (!opts.screenshot.empty() && !shotTaken) {
                screen.waitForPlate(30);
                screen.renderToCapture();
                shotTaken = ui::writePng(opts.screenshot, screen.capture());
            }
            break;
        }
    }
    rememberWindow();
    a.host.close();
    a.writeLastState();
    if (opts.exitAfter <= 0.0) {   // a clean close: no crash, no recovery copy needed
        ::DeleteFileW(runningMarker.c_str());
        ::DeleteFileW(recoveryFile.c_str());
    }
    return opts.screenshot.empty() || shotTaken ? 0 : 4;
}

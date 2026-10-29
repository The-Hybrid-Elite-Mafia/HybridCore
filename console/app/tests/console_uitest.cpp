// =============================================================================
//  hem_console_uitest.exe — UI automation test of the real HEMDigitalConsole.exe (CI only, never installed).
//
//  What is real here: the app is the shipped exe; mouse clicks and key presses go through Windows' own input queue
//  (SendInput: the same path as a real mouse and keyboard); the Windows file dialogs, "Save changes?" boxes, the
//  RECENT menu and the folder picker are driven with Microsoft UI Automation (as a screen reader would). Screen
//  resolution and Windows scale are changed for real when Windows allows it on the test machine, and the run says
//  plainly when it could not.
//  What the app tells the test (--test-hooks, WM_COPYDATA): where its controls are drawn, which stems are on which
//  channel, the title, and every control's value (to compare a reopened session). One exception to "real input":
//  dropping files is sent as a command (Explorer drag-and-drop can't be scripted); the app runs the same code.
//
//  Usage: hem_console_uitest --app <HEMDigitalConsole.exe> --work <empty folder> --out <folder for shots + report>
//  Exit code 0 = every check passed. The report (uitest-report.txt) lists every check with its measured result.
// =============================================================================
#include "hybrid/ui/Image.hpp"

#include <windows.h>
#include <shellscalingapi.h>
#include <uiautomation.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// ---------------------------------------------------------------------------------------------------------------
std::string gReport;
int gChecks = 0, gFails = 0;
fs::path gOut;
void log(const std::string& s) {
    std::printf("%s\n", s.c_str());
    std::fflush(stdout);
    gReport += s + "\n";
}
bool check(bool ok, const std::string& what) {
    ++gChecks;
    if (!ok) ++gFails;
    log(std::string(ok ? "  PASS  " : "  FAIL  ") + what);
    return ok;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string u8(const fs::path& p) { return narrow(p.wstring()); }
std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
bool waitFor(const std::function<bool()>& f, std::chrono::milliseconds limit, std::chrono::milliseconds step = 100ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(step);
    }
    return f();
}

// ---- test audio: 16-bit 48 kHz stereo WAV, a tone per stem ----
void writeWav(const fs::path& f, double hz, double seconds) {
    const std::uint32_t frames = static_cast<std::uint32_t>(48000 * seconds), bytes = frames * 4;
    std::ofstream o(f, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    o.write("RIFF", 4); u32(36 + bytes); o.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
    o.write("data", 4); u32(bytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto v = static_cast<std::int16_t>(6000.0 * std::sin(6.283185307179586 * hz * i / 48000.0));
        u16(static_cast<std::uint16_t>(v)); u16(static_cast<std::uint16_t>(v));
    }
}

// ---- real input (SendInput) ----
void sendKey(WORD vk, bool up) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    ::SendInput(1, &in, sizeof(in));
}
void chord(std::initializer_list<WORD> keys) {
    for (WORD k : keys) sendKey(k, false);
    std::this_thread::sleep_for(30ms);
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) sendKey(*it, true);
    std::this_thread::sleep_for(150ms);
}
void mouseAt(long x, long y) { ::SetCursorPos(static_cast<int>(x), static_cast<int>(y)); std::this_thread::sleep_for(40ms); }
void mouseButton(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    ::SendInput(1, &in, sizeof(in));
    std::this_thread::sleep_for(40ms);
}
void clickAt(long x, long y) {
    mouseAt(x, y);
    mouseButton(true);
    mouseButton(false);
    std::this_thread::sleep_for(150ms);
}
// The app's window to the front with a click on its title bar (a click activates a window; no keys involved - a lone
// ALT tap would put the window in menu mode).
void bringToFront(HWND h) {
    if (::GetForegroundWindow() == h) return;
    TITLEBARINFO ti{};
    ti.cbSize = sizeof(ti);
    if (::GetTitleBarInfo(h, &ti) && ti.rcTitleBar.right - ti.rcTitleBar.left > 300) {
        clickAt(ti.rcTitleBar.left + 160, (ti.rcTitleBar.top + ti.rcTitleBar.bottom) / 2);
    }
    ::SetForegroundWindow(h);
    std::this_thread::sleep_for(250ms);
}

// ---- the app process ----
struct Widget { int id = 0; std::string kind, label, key; RECT r{}; bool control = false; };
struct Report {
    std::map<std::string, std::string> kv;
    std::vector<Widget> widgets;
    RECT client{}, monitor{}, work{};
    [[nodiscard]] std::string get(const std::string& k) const { auto it = kv.find(k); return it == kv.end() ? std::string() : it->second; }
    [[nodiscard]] const Widget* find(const std::string& kind, const std::string& label) const {
        for (const auto& w : widgets) if (w.kind == kind && w.label == label) return &w;
        return nullptr;
    }
};
RECT parseRect(const std::string& s) {
    RECT r{};
    std::sscanf(s.c_str(), "%ld,%ld,%ld,%ld", &r.left, &r.top, &r.right, &r.bottom);
    return r;
}

struct App {
    fs::path exe, data;
    PROCESS_INFORMATION pi{};
    HWND hwnd = nullptr;
    int n = 0;
    bool start(const std::vector<std::wstring>& extra) {
        std::wstring cmd = L"\"" + exe.wstring() + L"\" --test-hooks --device offline --data-dir \"" + data.wstring() + L"\"";
        for (const auto& e : extra) cmd += L" " + e;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        if (!::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, exe.parent_path().c_str(), &si, &pi)) return false;
        hwnd = nullptr;
        waitFor([&] { hwnd = findWindow(); return hwnd && ::IsWindowVisible(hwnd); }, 60s);
        return hwnd != nullptr;
    }
    [[nodiscard]] HWND findWindow() const {
        struct Ctx { DWORD pid; HWND hit; } ctx{pi.dwProcessId, nullptr};
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            wchar_t cls[64] = {};
            ::GetClassNameW(h, cls, 64);
            if (pid == c->pid && std::wstring(cls) == L"HybridCoreGlWindow" && ::IsWindowVisible(h)) { c->hit = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hit;
    }
    // A top-level dialog (#32770) or menu (#32768) of the app's process.
    [[nodiscard]] HWND findDialog(const wchar_t* cls = L"#32770") const {
        struct Ctx { DWORD pid; const wchar_t* cls; HWND hit; } ctx{pi.dwProcessId, cls, nullptr};
        ::EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            wchar_t name[64] = {};
            ::GetClassNameW(h, name, 64);
            if (pid == c->pid && std::wstring(name) == c->cls && ::IsWindowVisible(h)) { c->hit = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
        return ctx.hit;
    }
    bool command(const std::string& c) {
        if (!hwnd) return false;
        COPYDATASTRUCT cd{0x48434D44, static_cast<DWORD>(c.size() + 1), const_cast<char*>(c.c_str())};
        DWORD_PTR res = 0;
        return ::SendMessageTimeoutW(hwnd, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &res) != 0;
    }
    // Asks the app for a fresh report (written on its next frame).
    bool report(Report& out) {
        const fs::path f = data / ("report-" + std::to_string(++n) + ".txt");
        std::error_code ec;
        fs::remove(f, ec);
        if (!command("report " + u8(f))) return false;
        if (!waitFor([&] { return fs::exists(f, ec) && fs::file_size(f, ec) > 0; }, 15s)) return false;
        std::this_thread::sleep_for(50ms);
        out = {};
        std::istringstream in(readText(f));
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == "widget") {
                Widget w;
                std::vector<std::string> p;
                std::istringstream ws(v);
                std::string part;
                while (std::getline(ws, part, '|')) p.push_back(part);
                if (p.size() < 9) continue;
                w.id = std::atoi(p[0].c_str()); w.kind = p[1]; w.label = p[2]; w.key = p[3];
                w.r = {std::atol(p[4].c_str()), std::atol(p[5].c_str()), std::atol(p[6].c_str()), std::atol(p[7].c_str())};
                w.control = p[8] == "1";
                out.widgets.push_back(w);
            } else {
                out.kv[k] = v;
            }
        }
        out.client = parseRect(out.get("client"));
        out.monitor = parseRect(out.get("monitor"));
        out.work = parseRect(out.get("work"));
        return true;
    }
    bool state(std::map<std::string, std::string>& out) {
        const fs::path f = data / ("state-" + std::to_string(++n) + ".txt");
        std::error_code ec;
        fs::remove(f, ec);
        if (!command("state " + u8(f)) || !waitFor([&] { return fs::exists(f, ec) && fs::file_size(f, ec) > 0; }, 15s)) return false;
        std::this_thread::sleep_for(50ms);
        out.clear();
        std::istringstream in(readText(f));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto eq = line.find('=');
            if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        }
        return !out.empty();
    }
    [[nodiscard]] bool running() const { return pi.hProcess && ::WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT; }
    bool waitExit(std::chrono::milliseconds t) { return pi.hProcess && ::WaitForSingleObject(pi.hProcess, static_cast<DWORD>(t.count())) == WAIT_OBJECT_0; }
    void kill() { if (running()) { ::TerminateProcess(pi.hProcess, 99); ::WaitForSingleObject(pi.hProcess, 10000); } }
    void close() { if (pi.hProcess) { ::CloseHandle(pi.hProcess); ::CloseHandle(pi.hThread); pi = {}; } }
    void clickWidget(const Widget& w) { bringToFront(hwnd); clickAt((w.r.left + w.r.right) / 2, (w.r.top + w.r.bottom) / 2); }
};

// ---- UI Automation ----
IUIAutomation* gUia = nullptr;
IUIAutomationElement* findIn(IUIAutomationElement* root, PROPERTYID prop, const std::wstring& value, CONTROLTYPEID type = 0) {
    if (!root) return nullptr;
    VARIANT v{};
    v.vt = VT_BSTR;
    v.bstrVal = ::SysAllocString(value.c_str());
    IUIAutomationCondition* c1 = nullptr;
    gUia->CreatePropertyCondition(prop, v, &c1);
    ::SysFreeString(v.bstrVal);
    IUIAutomationCondition* cond = c1;
    IUIAutomationCondition* c2 = nullptr;
    IUIAutomationCondition* both = nullptr;
    if (type) {
        VARIANT t{};
        t.vt = VT_I4;
        t.lVal = type;
        gUia->CreatePropertyCondition(UIA_ControlTypePropertyId, t, &c2);
        gUia->CreateAndCondition(c1, c2, &both);
        cond = both;
    }
    IUIAutomationElement* hit = nullptr;
    root->FindFirst(TreeScope_Descendants, cond, &hit);
    if (c1) c1->Release();
    if (c2) c2->Release();
    if (both) both->Release();
    return hit;
}
IUIAutomationElement* elementOf(HWND h) {
    IUIAutomationElement* e = nullptr;
    if (h) gUia->ElementFromHandle(h, &e);
    return e;
}
std::wstring nameOf(IUIAutomationElement* e) {
    BSTR b = nullptr;
    std::wstring s;
    if (e && SUCCEEDED(e->get_CurrentName(&b)) && b) { s = b; ::SysFreeString(b); }
    return s;
}
bool invoke(IUIAutomationElement* e) {
    if (!e) return false;
    IUIAutomationInvokePattern* p = nullptr;
    bool ok = false;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), reinterpret_cast<void**>(&p))) && p) {
        ok = SUCCEEDED(p->Invoke());
        p->Release();
    }
    return ok;
}
bool setValue(IUIAutomationElement* e, const std::wstring& v) {
    if (!e) return false;
    IUIAutomationValuePattern* p = nullptr;
    bool ok = false;
    if (SUCCEEDED(e->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), reinterpret_cast<void**>(&p))) && p) {
        BSTR b = ::SysAllocString(v.c_str());
        ok = SUCCEEDED(p->SetValue(b));
        ::SysFreeString(b);
        p->Release();
    }
    return ok;
}
// Every text the dialog shows (names of its parts), to check what it says.
std::wstring dialogText(HWND dlg) {
    IUIAutomationElement* root = elementOf(dlg);
    std::wstring all;
    if (!root) return all;
    IUIAutomationCondition* t = nullptr;
    gUia->CreateTrueCondition(&t);
    IUIAutomationElementArray* arr = nullptr;
    root->FindAll(TreeScope_Descendants, t, &arr);
    int n = 0;
    if (arr) arr->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* e = nullptr;
        arr->GetElement(i, &e);
        if (e) { all += nameOf(e) + L"\n"; e->Release(); }
    }
    if (arr) arr->Release();
    if (t) t->Release();
    root->Release();
    return all;
}
// A file dialog of the app: type the full path(s) into "File name:" and press the default button.
// For several files in one folder: navigate there first, then the quoted names.
bool fileDialogPick(App& app, const std::wstring& folder, const std::vector<std::wstring>& names, std::wstring* titleOut = nullptr, std::wstring* textOut = nullptr) {
    HWND dlg = nullptr;
    if (!waitFor([&] { dlg = app.findDialog(); return dlg != nullptr; }, 15s)) return false;
    std::this_thread::sleep_for(700ms);
    wchar_t title[256] = {};
    ::GetWindowTextW(dlg, title, 256);
    if (titleOut) *titleOut = title;
    if (textOut) *textOut = dialogText(dlg);
    IUIAutomationElement* root = elementOf(dlg);
    IUIAutomationElement* edit = findIn(root, UIA_AutomationIdPropertyId, L"1148", UIA_EditControlTypeId);   // open dialogs
    if (!edit) edit = findIn(root, UIA_AutomationIdPropertyId, L"1001", UIA_EditControlTypeId);          // save dialogs
    if (!edit) edit = findIn(root, UIA_NamePropertyId, L"File name:", UIA_EditControlTypeId);
    if (!edit) edit = findIn(root, UIA_NamePropertyId, L"Folder:", UIA_EditControlTypeId);
    IUIAutomationElement* okBtn = findIn(root, UIA_AutomationIdPropertyId, L"1", UIA_ButtonControlTypeId);
    bool ok = edit && okBtn;
    if (ok && !folder.empty() && names.size() != 1) {   // go to the folder first
        ok = setValue(edit, folder);
        ::SetForegroundWindow(dlg);
        if (ok) { invoke(okBtn); std::this_thread::sleep_for(900ms); }
    }
    std::wstring value;
    if (names.size() == 1) value = folder.empty() ? names[0] : (fs::path(folder) / names[0]).wstring();
    else for (const auto& n : names) value += L"\"" + n + L"\" ";
    if (ok) ok = setValue(edit, value);
    std::this_thread::sleep_for(200ms);
    if (ok) ok = invoke(okBtn);
    if (edit) edit->Release();
    if (okBtn) okBtn->Release();
    if (root) root->Release();
    // gone?
    return ok && waitFor([&] { return app.findDialog() != dlg || !::IsWindow(dlg) || !::IsWindowVisible(dlg); }, 15s);
}
// Presses a named button in the app's dialog (task dialog). Returns the dialog's text too.
bool pressDialogButton(App& app, const std::wstring& button, std::wstring* textOut = nullptr, std::chrono::milliseconds wait = 15s) {
    HWND dlg = nullptr;
    if (!waitFor([&] { dlg = app.findDialog(); return dlg != nullptr; }, wait)) return false;
    std::this_thread::sleep_for(500ms);
    if (textOut) *textOut = dialogText(dlg);
    IUIAutomationElement* root = elementOf(dlg);
    IUIAutomationElement* b = findIn(root, UIA_NamePropertyId, button, UIA_ButtonControlTypeId);
    const bool ok = invoke(b);
    if (b) b->Release();
    if (root) root->Release();
    return ok && waitFor([&] { return !::IsWindow(dlg) || !::IsWindowVisible(dlg); }, 15s);
}

// ---- screen ----
bool screenshot(const fs::path& file) {
    const int x = ::GetSystemMetrics(SM_XVIRTUALSCREEN), y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int w = ::GetSystemMetrics(SM_CXVIRTUALSCREEN), h = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC scr = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(scr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = ::CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = ::SelectObject(mem, bmp);
    ::BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY | CAPTUREBLT);
    hc::ui::Image img;
    img.width = static_cast<std::uint32_t>(w);
    img.height = static_cast<std::uint32_t>(h);
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    const auto* src = static_cast<const std::uint8_t*>(bits);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        img.rgba[i * 4 + 0] = src[i * 4 + 2];
        img.rgba[i * 4 + 1] = src[i * 4 + 1];
        img.rgba[i * 4 + 2] = src[i * 4 + 0];
        img.rgba[i * 4 + 3] = 255;
    }
    ::SelectObject(mem, old);
    ::DeleteObject(bmp);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, scr);
    return hc::ui::writePng(file, img);
}

// The last lines of the app's own trace (--test-hooks writes it).
void logTrace(App& app, int lines) {
    std::istringstream in(readText(app.data / "trace.log"));
    std::vector<std::string> all;
    std::string l;
    while (std::getline(in, l)) all.push_back(l);
    log("     app trace (last " + std::to_string(std::min<std::size_t>(all.size(), static_cast<std::size_t>(lines))) + " of " + std::to_string(all.size()) + " lines):");
    for (std::size_t i = all.size() > static_cast<std::size_t>(lines) ? all.size() - static_cast<std::size_t>(lines) : 0; i < all.size(); ++i) log("       " + all[i]);
}
// No dialog of the app may be open before a step starts. One still there (from an earlier step) is named in the report
// and closed, so the next step can't mistake it for its own.
void waitNoDialog(App& app, const std::string& where) {
    if (waitFor([&] { return app.findDialog() == nullptr; }, 8s)) return;
    HWND d = app.findDialog();
    wchar_t t[256] = {};
    ::GetWindowTextW(d, t, 256);
    log("  NOTE before " + where + ": a dialog was still open: \"" + narrow(t) + "\" - " + narrow(dialogText(d).substr(0, 300)) + " (closed)");
    ::PostMessageW(d, WM_CLOSE, 0, 0);
    waitFor([&] { return app.findDialog() == nullptr; }, 5s);
}
// When the app stops answering: what it shows (an error box?) goes in the report.
void whyNoAnswer(App& app, const std::string& tag) {
    HWND d = app.findDialog();
    if (d) {
        wchar_t t[256] = {};
        ::GetWindowTextW(d, t, 256);
        log("     the app shows a dialog: \"" + narrow(t) + "\": " + narrow(dialogText(d).substr(0, 400)));
    } else {
        log(std::string("     no dialog; app ") + (app.running() ? "running but not answering" : "not running"));
    }
    screenshot(gOut / ("uitest-noanswer-" + std::to_string(++app.n) + ".png"));
    logTrace(app, 12);
    (void)tag;
}

// Resolution of the main screen (ChangeDisplaySettingsEx: the real Windows setting, for this session).
bool setResolution(int w, int h) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = static_cast<DWORD>(w);
    dm.dmPelsHeight = static_cast<DWORD>(h);
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    return ::ChangeDisplaySettingsExW(nullptr, &dm, nullptr, CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
}
std::string screenModes() {
    std::string s;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    std::vector<std::pair<DWORD, DWORD>> seen;
    for (DWORD i = 0; ::EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        const auto m = std::make_pair(dm.dmPelsWidth, dm.dmPelsHeight);
        if (std::find(seen.begin(), seen.end(), m) == seen.end()) { seen.push_back(m); s += std::to_string(m.first) + "x" + std::to_string(m.second) + " "; }
    }
    return s;
}

// Windows display scale of the main screen. Windows has no documented call for this; the Settings app uses
// DisplayConfigSetDeviceInfo with the private types -3 (get) / -4 (set), as the open-source "SetDPI" tool does. The
// test reads the DPI back from Windows (GetDpiForMonitor) and reports what it really got.
struct DpiScaleGet { DISPLAYCONFIG_DEVICE_INFO_HEADER header; std::int32_t minRel, curRel, maxRel; };
struct DpiScaleSet { DISPLAYCONFIG_DEVICE_INFO_HEADER header; std::int32_t rel; };
constexpr int kScales[] = {100, 125, 150, 175, 200, 225, 250, 300, 350, 400, 450, 500};
bool primarySource(LUID& adapter, UINT32& id) {
    UINT32 np = 0, nm = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS || np == 0) return false;
    adapter = paths[0].sourceInfo.adapterId;
    id = paths[0].sourceInfo.id;
    return true;
}
int monitorDpi() {
    UINT x = 96, y = 96;
    const POINT origin{0, 0};
    ::GetDpiForMonitor(::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), MDT_EFFECTIVE_DPI, &x, &y);
    return static_cast<int>(x);
}
// Returns the scale Windows reports after the change (percent), or -1 when Windows refused / can't offer it.
int setScale(int percent, std::string* offered) {
    LUID adapter{};
    UINT32 id = 0;
    if (!primarySource(adapter, id)) return -1;
    DpiScaleGet g{};
    g.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-3);
    g.header.size = sizeof(g);
    g.header.adapterId = adapter;
    g.header.id = id;
    if (::DisplayConfigGetDeviceInfo(&g.header) != ERROR_SUCCESS) return -1;
    const int rec = std::abs(g.minRel);   // index of the recommended scale
    if (offered) {
        offered->clear();
        for (int r = g.minRel; r <= g.maxRel; ++r) if (rec + r >= 0 && rec + r < static_cast<int>(std::size(kScales))) *offered += std::to_string(kScales[rec + r]) + "% ";
    }
    int want = -1;
    for (int i = 0; i < static_cast<int>(std::size(kScales)); ++i) if (kScales[i] == percent) want = i;
    const int rel = want - rec;
    if (want < 0 || rel < g.minRel || rel > g.maxRel) return -1;
    DpiScaleSet s{};
    s.header.type = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(-4);
    s.header.size = sizeof(s);
    s.header.adapterId = adapter;
    s.header.id = id;
    s.rel = rel;
    if (::DisplayConfigSetDeviceInfo(&s.header) != ERROR_SUCCESS) return -1;
    std::this_thread::sleep_for(1500ms);
    return monitorDpi() * 100 / 96;
}

// ---- checks ----
bool inside(const RECT& in, const RECT& out) { return in.left >= out.left && in.top >= out.top && in.right <= out.right && in.bottom <= out.bottom; }
// Every control drawn on the page is inside the window and on the screen, and Windows says the window is what is
// under its centre (nothing covers it). Returns the number of controls checked.
int checkOnScreen(App& app, const Report& r, const std::string& where) {
    int total = 0, outside = 0, covered = 0;
    std::string firstBad;
    for (const auto& w : r.widgets) {
        ++total;
        const bool in = inside(w.r, r.client) && inside(w.r, r.monitor);
        const POINT c{(w.r.left + w.r.right) / 2, (w.r.top + w.r.bottom) / 2};
        HWND under = ::WindowFromPoint(c);
        const bool mine = under == app.hwnd || ::IsChild(app.hwnd, under);
        if (!in) ++outside;
        if (in && !mine) ++covered;
        if ((!in || !mine) && firstBad.empty()) firstBad = w.kind + " '" + w.label + "'";
    }
    std::string buttons;
    for (const auto& w : r.widgets) if (w.kind == "button" && w.r.top > (r.client.top + r.client.bottom) / 2) buttons += w.label + " ";
    check(total > 0 && outside == 0 && covered == 0,
          where + ": " + std::to_string(total) + " controls, " + std::to_string(outside) + " outside the window/screen, " + std::to_string(covered) +
              " covered" + (firstBad.empty() ? "" : " (first: " + firstBad + ")") + "; bottom bar: " + buttons);
    return total;
}

std::string clientText(const Report& r) {
    char b[160];
    std::snprintf(b, sizeof b, "screen %ldx%ld, window client %ldx%ld, DPI %s, faceplate scale %s, maximized %s", r.monitor.right - r.monitor.left,
                  r.monitor.bottom - r.monitor.top, r.client.right - r.client.left, r.client.bottom - r.client.top, r.get("dpi").c_str(),
                  r.get("scale").c_str(), r.get("maximized").c_str());
    return b;
}
std::optional<Widget> button(const Report& r, const char* label) {
    if (const Widget* w = r.find("button", label)) return *w;
    return std::nullopt;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    fs::path appExe, work, out;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--app") appExe = argv[++i];
        else if (k == L"--work") work = argv[++i];
        else if (k == L"--out") out = argv[++i];
    }
    if (appExe.empty() || work.empty() || out.empty()) { std::printf("usage: hem_console_uitest --app <exe> --work <dir> --out <dir>\n"); return 2; }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // read real pixels
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), reinterpret_cast<void**>(&gUia)))) {
        std::printf("UI Automation not available\n");
        return 2;
    }
    gOut = out;
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    fs::create_directories(out, ec);
    log("HEM Digital Console - UI automation test (real clicks + keys via SendInput, Windows dialogs via UI Automation)");

    // ---- 12 stems + the whole song, named exactly as in Steven's "reading the same letter" Suno folder (Oct 2026) ----
    const fs::path stems = work / "reading the same letter";
    fs::create_directories(stems, ec);
    const std::vector<std::wstring> suno = {L"0 Reading the Same Letter.wav", L"1 Woodwinds.wav", L"10 Drums.wav", L"11 Backing_Vocals.wav", L"12 Vocals.wav",
                                            L"2 Brass.wav", L"3 FX.wav", L"4 Synth.wav", L"5 Strings.wav", L"6 Percussion.wav", L"7 Keyboard.wav",
                                            L"8 Guitar.wav", L"9 Bass.wav"};
    for (std::size_t i = 0; i < suno.size(); ++i) writeWav(stems / suno[i], 110.0 * static_cast<double>(i + 1), 2.0);
    std::ofstream(stems / "Reading the Same Letter (Vocals).mid") << "MThd";   // Suno puts MIDI files next to the stems: not audio, skipped
    // the channel each must land on (1-based); the whole song goes to EXT 1 (slot 14)
    const std::map<std::wstring, int> wantCh = {{L"12 Vocals.wav", 1}, {L"11 Backing_Vocals.wav", 2}, {L"10 Drums.wav", 3}, {L"6 Percussion.wav", 4},
                                                {L"9 Bass.wav", 5}, {L"8 Guitar.wav", 6}, {L"7 Keyboard.wav", 7}, {L"4 Synth.wav", 8},
                                                {L"5 Strings.wav", 9}, {L"2 Brass.wav", 10}, {L"1 Woodwinds.wav", 11}, {L"3 FX.wav", 12},
                                                {L"0 Reading the Same Letter.wav", 14}};
    App app;
    app.exe = fs::absolute(appExe);
    app.data = work / "appdata";
    fs::create_directories(app.data, ec);

    // =========================================================================================================
    log("\n[1] Screen fit: every control on screen and not covered, at each screen size and Windows scale");
    log("  screen modes Windows offers here: " + screenModes());
    DEVMODEW orig{};
    orig.dmSize = sizeof(orig);
    ::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &orig);
    log("  screen at start: " + std::to_string(orig.dmPelsWidth) + "x" + std::to_string(orig.dmPelsHeight) + ", " + std::to_string(monitorDpi() * 100 / 96) + "% scale");
    struct Res { int w, h; };
    int fitRuns = 0;
    for (const Res res : {Res{1366, 768}, Res{1920, 1080}, Res{2560, 1600}}) {
        const bool resOk = setResolution(res.w, res.h);
        DEVMODEW now{};
        now.dmSize = sizeof(now);
        ::EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &now);
        const bool gotRes = static_cast<int>(now.dmPelsWidth) == res.w && static_cast<int>(now.dmPelsHeight) == res.h;
        log("  -- " + std::to_string(res.w) + "x" + std::to_string(res.h) + ": " + (gotRes ? "screen set" : std::string("Windows here can't show this size (now ") +
            std::to_string(now.dmPelsWidth) + "x" + std::to_string(now.dmPelsHeight) + ")" + (resOk ? "" : ", change refused")));
        if (!gotRes) continue;
        for (int scale : {100, 150, 200}) {
            std::string offered;
            const int got = setScale(scale, &offered);
            if (got != scale) {
                log("     " + std::to_string(scale) + "%: not tested - Windows offers " + (offered.empty() ? std::string("no scale change here") : offered) + "at this size" +
                    (got > 0 ? " (got " + std::to_string(got) + "%)" : ""));
                continue;
            }
            fs::remove_all(app.data, ec);
            fs::create_directories(app.data, ec);
            if (!app.start({})) { check(false, "app started"); continue; }   // first start: maximized
            std::this_thread::sleep_for(3s);
            bringToFront(app.hwnd);
            Report r;
            const bool rep = app.report(r);
            const std::string tag = std::to_string(res.w) + "x" + std::to_string(res.h) + " at " + std::to_string(scale) + "%";
            if (!rep) whyNoAnswer(app, tag);
            if (check(rep, tag + ": app answered")) {
                log("     " + clientText(r));
                check(r.get("maximized") == "1", tag + ": opens maximized the first time");
                checkOnScreen(app, r, tag + " (DESK, maximized)");
                for (int pg = 1; pg < 7; ++pg) {   // the other six tabs
                    app.command("page " + std::to_string(pg));
                    std::this_thread::sleep_for(800ms);
                    Report rp;
                    if (app.report(rp)) checkOnScreen(app, rp, tag + " (" + rp.get("page") + ")");
                }
                app.command("page 0");
                std::this_thread::sleep_for(1s);
                const fs::path shot = out / ("uitest-" + std::to_string(res.w) + "x" + std::to_string(res.h) + "-" + std::to_string(scale) + "pct.png");
                check(screenshot(shot), tag + ": screenshot " + u8(shot.filename()));
                // a small window (not maximized): still everything on screen, and Windows keeps the minimum size
                ::ShowWindow(app.hwnd, SW_RESTORE);
                ::SetWindowPos(app.hwnd, nullptr, 20, 20, 300, 200, SWP_NOZORDER);
                std::this_thread::sleep_for(1500ms);
                Report tiny;
                if (app.report(tiny)) {
                    const long cw = tiny.client.right - tiny.client.left, ch = tiny.client.bottom - tiny.client.top;
                    const int dpi = std::atoi(tiny.get("dpi").c_str());
                    check(cw >= 800 * dpi / 96 - 2 && ch >= 450 * dpi / 96 - 2, tag + ": smallest window " + std::to_string(cw) + "x" + std::to_string(ch) + " px (asked 300x200; minimum 800x450 at 100%)");
                    checkOnScreen(app, tiny, tag + " (smallest window)");
                    screenshot(out / ("uitest-" + std::to_string(res.w) + "x" + std::to_string(res.h) + "-" + std::to_string(scale) + "pct-small.png"));
                }
                ++fitRuns;
            }
            ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
            if (!app.waitExit(20s)) { check(false, tag + ": closed"); whyNoAnswer(app, tag); app.kill(); }
            app.close();
        }
        setScale(100, nullptr);
    }
    ::ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);   // back to the start size
    if (fitRuns == 0) log("  NOTE: no screen size / scale combination could be set here: the fit checks below run at the test machine's own screen");

    // the rest runs at the machine's own screen size
    fs::remove_all(app.data, ec);
    fs::create_directories(app.data, ec);

    // =========================================================================================================
    log("\n[2] LOAD STEMS: the 12 Suno stems, real clicks and the real Windows file dialog");
    if (!app.start({})) { check(false, "app started"); return 1; }
    std::this_thread::sleep_for(3s);
    Report r;
    app.report(r);
    log("  " + clientText(r));
    checkOnScreen(app, r, "DESK at this machine's screen");
    screenshot(out / "uitest-desk-start.png");
    const auto load = button(r, "LOAD STEMS");
    std::wstring dlgTitle, dlgText;
    if (check(load.has_value(), "LOAD STEMS button found")) {
        app.clickWidget(*load);
        check(fileDialogPick(app, stems.wstring(), suno, &dlgTitle, &dlgText), "file dialog opened by the click; all 13 files picked and opened");
        log("  dialog title: " + narrow(dlgTitle));
    }
    auto loadedCount = [&](const char* must) {
        Report q;
        if (!app.report(q)) return -1;
        int n = 0;
        for (int k : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14}) {
            const std::string v = q.get("stem" + std::to_string(k));
            if (!v.empty() && v.find("(loading)") == std::string::npos && (!must || v.find(must) != std::string::npos)) ++n;
        }
        return n;
    };
    bool loaded = waitFor([&] { return loadedCount(nullptr) == 13; }, 60s, 500ms);
    app.report(r);
    int right = 0;
    for (const auto& [name, ch] : wantCh) if (fs::path(widen(r.get("stem" + std::to_string(ch)))).filename() == name) ++right;
    check(loaded && right == 13, "all loaded; " + std::to_string(right) + " of 13 where they belong (Vocals 1, Backing 2, Drums 3, Percussion 4, Bass 5, Guitar 6, Keyboard 7, Synth 8, Strings 9, Brass 10, Woodwinds 11, FX 12, the whole song in EXT 1)");
    for (int k = 1; k <= 14; ++k) log("    channel " + std::to_string(k) + ": " + (r.get("stem" + std::to_string(k)).empty() ? "-" : u8(fs::path(widen(r.get("stem" + std::to_string(k)))).filename())));
    screenshot(out / "uitest-12-stems-loaded.png");

    // =========================================================================================================
    log("\n[3] Change controls with real clicks (switches and selectors on DESK, STEP EQ, TWIN BUS, COLOUR)");
    std::map<std::string, std::string> before, after;
    app.state(before);
    int clicked = 0;
    for (int pg : {0, 2, 3, 4}) {
        app.command("page " + std::to_string(pg));
        std::this_thread::sleep_for(1s);
        Report rp;
        app.report(rp);
        int onPage = 0;
        for (const auto& w : rp.widgets) {
            if (!(w.kind == "switch" || w.kind == "selector") || !w.control) continue;
            if (w.label == "MODEL" || w.label == "RACK IN" || w.label == "BYPASS") continue;   // keep the page as it is drawn
            if (onPage >= 25) break;
            app.clickWidget(w);
            ++onPage;
            ++clicked;
        }
    }
    app.command("page 0");
    std::this_thread::sleep_for(500ms);
    app.state(after);
    int changed = 0;
    for (const auto& [k, v] : after) if (before.count(k) && before[k] != v) ++changed;
    check(changed >= 50, std::to_string(clicked) + " real clicks changed " + std::to_string(changed) + " saved controls (need at least 50)");
    app.report(r);
    check(r.get("title").find(" *") != std::string::npos && r.get("dirty") == "1", "title shows unsaved changes: \"" + r.get("title") + "\"");

    // =========================================================================================================
    log("\n[4] SAVE (Ctrl+S on the keyboard): first save asks for a name");
    const fs::path sessionFile = work / "Reading the Same Letter.hemc";
    waitNoDialog(app, "SAVE");
    bringToFront(app.hwnd);
    chord({VK_CONTROL, 'S'});
    if (!check(fileDialogPick(app, L"", {sessionFile.wstring()}, &dlgTitle), "save dialog (\"" + narrow(dlgTitle) + "\"), name typed, Save pressed")) logTrace(app, 15);
    check(waitFor([&] { return fs::exists(sessionFile, ec); }, 10s), "session file written: " + u8(sessionFile.filename()));
    std::this_thread::sleep_for(1500ms);
    app.report(r);
    check(r.get("title").find(" *") == std::string::npos && r.get("dirty") == "0", "title after save: \"" + r.get("title") + "\" (no *)");
    check(r.get("recent1") == u8(sessionFile), "it is first in RECENT");
    const std::string savedText = readText(sessionFile);
    check(savedText.find("version=2") != std::string::npos && savedText.find(".rel=reading the same letter") != std::string::npos,
          "file holds the format version and stem paths relative to the session");
    // SAVE again (button): saves in place, no dialog
    const auto t0 = fs::last_write_time(sessionFile, ec);
    std::this_thread::sleep_for(1100ms);
    if (const auto sv = button(r, "SAVE")) app.clickWidget(*sv);
    std::this_thread::sleep_for(1500ms);
    check(app.findDialog() == nullptr && fs::last_write_time(sessionFile, ec) != t0, "SAVE button the second time: saved in place, no dialog");

    // =========================================================================================================
    log("\n[5] Close and reopen: every setting must match");
    std::map<std::string, std::string> saved;
    app.state(saved);
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);   // the close box
    check(app.waitExit(20s), "closed without a question (nothing unsaved)");
    app.close();
    if (!app.start({L"\"" + sessionFile.wstring() + L"\""})) { check(false, "app restarted with the session"); return 1; }
    std::this_thread::sleep_for(3s);
    waitFor([&] { return loadedCount(nullptr) == 13; }, 60s, 500ms);
    std::map<std::string, std::string> reopened;
    app.state(reopened);
    int same = 0, diff = 0;
    std::string firstDiff;
    for (const auto& [k, v] : saved) {
        auto it = reopened.find(k);
        if (it != reopened.end() && it->second == v) ++same;
        else { ++diff; if (firstDiff.empty()) firstDiff = k + ": " + v + " -> " + (it == reopened.end() ? "(missing)" : it->second); }
    }
    check(diff == 0 && same == static_cast<int>(saved.size()) && reopened.size() == saved.size(),
          std::to_string(same) + " of " + std::to_string(saved.size()) + " saved settings match after reopening" + (firstDiff.empty() ? "" : " (first difference: " + firstDiff + ")"));
    app.report(r);
    right = 0;
    for (const auto& [name, ch] : wantCh) if (fs::path(widen(r.get("stem" + std::to_string(ch)))).filename() == name) ++right;
    check(right == 13, "13 of 13 files back in the same slots (" + std::to_string(right) + ")");

    // =========================================================================================================
    log("\n[6] NEW with unsaved changes asks \"Save changes?\"");
    app.report(r);
    for (const auto& w : r.widgets) if (w.kind == "switch" && w.control) { app.clickWidget(w); break; }
    std::this_thread::sleep_for(800ms);
    app.report(r);
    std::wstring text;
    if (const auto nw = button(r, "NEW")) {
        app.clickWidget(*nw);
        check(pressDialogButton(app, L"Cancel", &text), "NEW: the question appeared; Cancel keeps everything");
        log("  it says: " + narrow(text.substr(0, text.find(L'\n', 40))));
        app.report(r);
        check(!r.get("stem1").empty(), "after Cancel the stems are still there");
        app.clickWidget(*nw);
        check(pressDialogButton(app, L"Don't save", &text), "NEW again: \"Don't save\" pressed");
        std::this_thread::sleep_for(1s);
        app.report(r);
        check(r.get("stem1").empty() && r.get("dirty") == "0" && r.get("session").empty(), "new session: no stems, nothing unsaved, untitled");
    }

    // =========================================================================================================
    log("\n[7] OPEN: the three file types, start folder, opening a session; RECENT");
    app.report(r);
    if (const auto op = button(r, "OPEN")) {
        app.clickWidget(*op);
        HWND dlg = nullptr;
        waitFor([&] { dlg = app.findDialog(); return dlg != nullptr; }, 15s);
        std::this_thread::sleep_for(800ms);
        const std::wstring all = dialogText(dlg);
        check(all.find(L"Sessions + Audio") != std::wstring::npos && all.find(L"Sessions (*.hemc)") != std::wstring::npos &&
                  all.find(L"Audio (WAV, MP3, FLAC)") != std::wstring::npos,
              "OPEN dialog offers \"Sessions + Audio\", \"Sessions (*.hemc)\" and \"Audio (WAV, MP3, FLAC)\"");
        check(fileDialogPick(app, L"", {sessionFile.wstring()}), "session picked in OPEN");
        std::this_thread::sleep_for(3s);
        app.report(r);
        check(r.get("session") == u8(sessionFile) && !r.get("stem1").empty(), "the session opened with its stems");
    }
    // OPEN with audio files: they load as stems
    app.report(r);
    if (const auto op = button(r, "OPEN")) {
        app.clickWidget(*op);
        check(fileDialogPick(app, stems.wstring(), {L"12 Vocals.wav", L"10 Drums.wav"}), "OPEN with two audio files");
        std::this_thread::sleep_for(3s);
        app.report(r);
        check(fs::path(widen(r.get("stem1"))).filename() == L"12 Vocals.wav" && fs::path(widen(r.get("stem3"))).filename() == L"10 Drums.wav" && r.get("stem2").empty(),
              "audio from OPEN loads as stems (Vocals on 1, Drums on 3)");
    }
    // RECENT: the session is in the menu
    app.report(r);
    if (const auto rc = button(r, "RECENT")) {
        app.clickWidget(*rc);
        HWND menu = nullptr;
        waitFor([&] { menu = ::FindWindowW(L"#32768", nullptr); return menu && ::IsWindowVisible(menu); }, 5s);
        IUIAutomationElement* root = nullptr;
        gUia->GetRootElement(&root);
        IUIAutomationElement* item = nullptr;
        waitFor([&] {
            IUIAutomationCondition* t = nullptr;
            VARIANT v{}; v.vt = VT_I4; v.lVal = UIA_MenuItemControlTypeId;
            gUia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &t);
            IUIAutomationElementArray* arr = nullptr;
            root->FindAll(TreeScope_Descendants, t, &arr);
            int n = 0;
            if (arr) arr->get_Length(&n);
            for (int i = 0; i < n && !item; ++i) {
                IUIAutomationElement* e = nullptr;
                arr->GetElement(i, &e);
                if (e && nameOf(e).find(L"Reading the Same Letter") != std::wstring::npos) item = e; else if (e) e->Release();
            }
            if (arr) arr->Release();
            if (t) t->Release();
            return item != nullptr;
        }, 5s);
        check(item != nullptr, "RECENT lists \"Reading the Same Letter\"");
        // opening it asks about the unsaved stems loaded just before
        if (item) { invoke(item); item->Release(); } else chord({VK_ESCAPE});
        if (root) root->Release();
        pressDialogButton(app, L"Don't save", nullptr, 5s);
        std::this_thread::sleep_for(3s);
        app.report(r);
        check(r.get("session") == u8(sessionFile), "picked from RECENT: the session is open again");
    }

    // =========================================================================================================
    log("\n[8] SAVE AS (Ctrl+Shift+S)");
    const fs::path copyFile = work / "Reading the Same Letter v2.hemc";
    bringToFront(app.hwnd);
    chord({VK_CONTROL, VK_SHIFT, 'S'});
    check(fileDialogPick(app, L"", {copyFile.wstring()}, &dlgTitle), "SAVE AS dialog (\"" + narrow(dlgTitle) + "\"), new name");
    std::this_thread::sleep_for(1500ms);
    app.report(r);
    check(fs::exists(copyFile, ec) && r.get("session") == u8(copyFile) && r.get("title").find("v2") != std::string::npos, "saved as a new file; the title names it");

    // =========================================================================================================
    log("\n[9] Crash in the middle of a save: the last saved session must still open; recovery is offered");
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    app.waitExit(20s);
    app.close();
    std::map<std::string, std::string> beforeCrash;
    if (!app.start({L"--test-slow-save", L"60", L"\"" + copyFile.wstring() + L"\""})) { check(false, "app started (slow save)"); return 1; }
    std::this_thread::sleep_for(3s);
    app.state(beforeCrash);
    app.report(r);
    int flipped = 0;
    for (const auto& w : r.widgets) if (w.kind == "switch" && w.control && flipped < 3) { app.clickWidget(w); ++flipped; }
    std::this_thread::sleep_for(500ms);
    std::map<std::string, std::string> changedState;
    app.state(changedState);
    app.command("write-recovery");
    std::this_thread::sleep_for(1500ms);
    const std::string goodText = readText(copyFile);
    bringToFront(app.hwnd);
    chord({VK_CONTROL, 'S'});
    const fs::path partial = copyFile.wstring() + L".saving";
    const bool midSave = waitFor([&] { return fs::exists(partial, ec) && fs::file_size(partial, ec) > 0; }, 5s, 20ms);
    std::this_thread::sleep_for(150ms);
    const auto partialSize = fs::exists(partial, ec) ? fs::file_size(partial, ec) : 0;
    app.kill();
    app.close();
    check(midSave && partialSize > 0 && partialSize < goodText.size(),
          "killed while saving: the half-written side file had " + std::to_string(partialSize) + " of " + std::to_string(goodText.size()) + " bytes");
    check(readText(copyFile) == goodText, "the session file itself is untouched (byte for byte the last save)");
    // relaunch without a session: the crash is noticed, recovery offered
    if (!app.start({})) { check(false, "app restarted after the crash"); return 1; }
    check(pressDialogButton(app, L"Recover my work", &text), "after the crash: \"Recover my work?\" offered and accepted");
    std::this_thread::sleep_for(3s);
    std::map<std::string, std::string> recovered;
    app.state(recovered);
    int recMatch = 0;
    for (const auto& [k, v] : changedState) if (recovered.count(k) && recovered[k] == v) ++recMatch;
    check(recMatch == static_cast<int>(changedState.size()), "recovered work has the changes made before the crash (" + std::to_string(recMatch) + " of " + std::to_string(changedState.size()) + " settings)");
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    check(pressDialogButton(app, L"Don't save", nullptr, 10s), "closing with recovered (unsaved) work asks first");
    app.waitExit(20s);
    app.close();
    if (!app.start({L"\"" + copyFile.wstring() + L"\""})) { check(false, "app started with the session"); return 1; }
    std::this_thread::sleep_for(3s);
    std::map<std::string, std::string> afterCrash;
    app.state(afterCrash);
    int crashSame = 0;
    for (const auto& [k, v] : beforeCrash) if (afterCrash.count(k) && afterCrash[k] == v) ++crashSame;
    check(crashSame == static_cast<int>(beforeCrash.size()), "the session opens as last saved: " + std::to_string(crashSame) + " of " + std::to_string(beforeCrash.size()) + " settings");
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    app.waitExit(20s);
    app.close();

    // =========================================================================================================
    log("\n[10] Moved stems: reopening asks where they are and relinks them by name");
    const fs::path moved = work / "moved" / "reading the same letter (moved)";
    fs::create_directories(moved.parent_path(), ec);
    fs::rename(stems, moved, ec);
    check(!ec, "stems folder moved to " + u8(moved));
    if (!app.start({L"\"" + sessionFile.wstring() + L"\""})) { check(false, "app started"); return 1; }
    check(pressDialogButton(app, L"Find the folder...", &text), "it asked about the missing stems (not crashed): \"" +
          narrow(text.substr(0, std::min<std::size_t>(text.size(), 60))) + "...\"");
    check(fileDialogPick(app, L"", {moved.wstring()}), "folder picked in the folder dialog");
    loaded = waitFor([&] { return loadedCount("(moved)") == 13; }, 60s, 500ms);
    app.report(r);
    check(loaded, "all 13 files found in the new folder and loaded");
    check(app.running(), "the console is still running");

    // =========================================================================================================
    log("\n[11] Dropping files (sent as a command: the same code as an Explorer drop)");
    app.report(r);
    // channel 5's name strip is the 5th name strip on DESK
    std::optional<Widget> strip5;
    int seen = 0;
    for (const auto& w : r.widgets) if (w.kind == "stem" && ++seen == 5) strip5 = w;
    if (strip5) {
        const long cx = (strip5->r.left + strip5->r.right) / 2 - r.client.left, cy = (strip5->r.top + strip5->r.bottom) / 2 - r.client.top;
        app.command("drop " + std::to_string(cx) + " " + std::to_string(cy) + "|" + u8(moved / "4 Synth.wav"));
        std::this_thread::sleep_for(2500ms);
        Report q;
        app.report(q);
        int unchanged = 0;
        for (int k = 1; k <= 13; ++k) if (k != 5 && q.get("stem" + std::to_string(k)) == r.get("stem" + std::to_string(k))) ++unchanged;
        check(fs::path(widen(q.get("stem5"))).filename() == L"4 Synth.wav" && unchanged == 12, "one file dropped on channel 5: only channel 5 changed (12 others untouched)");
    }
    // 15 files: 13 loaded, the 2 left out are named
    const fs::path many = work / "fifteen";
    fs::create_directories(many, ec);
    for (int k = 1; k <= 15; ++k) writeWav(many / ("Take " + std::to_string(k) + ".wav"), 200.0 + k, 0.5);
    app.command("drop 10 10|" + u8(many));
    check(pressDialogButton(app, L"OK", &text), "15 files: the console says plainly which were left out");
    check(text.find(L"Take 14.wav") != std::wstring::npos && text.find(L"Take 15.wav") != std::wstring::npos, "the message names Take 14.wav and Take 15.wav");

    // =========================================================================================================
    log("\n[12] Window size, position and maximized state are remembered");
    ::ShowWindow(app.hwnd, SW_RESTORE);
    ::SetWindowPos(app.hwnd, nullptr, 60, 50, 1000, 640, SWP_NOZORDER);
    std::this_thread::sleep_for(2500ms);
    RECT wr{};
    ::GetWindowRect(app.hwnd, &wr);
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    pressDialogButton(app, L"Don't save", nullptr, 5s);
    app.waitExit(20s);
    app.close();
    app.start({});
    std::this_thread::sleep_for(3s);
    RECT wr2{};
    ::GetWindowRect(app.hwnd, &wr2);
    check(!::IsZoomed(app.hwnd) && wr2.left == wr.left && wr2.top == wr.top && wr2.right == wr.right && wr2.bottom == wr.bottom,
          "reopened at the same place and size (" + std::to_string(wr2.right - wr2.left) + "x" + std::to_string(wr2.bottom - wr2.top) + " at " +
              std::to_string(wr2.left) + "," + std::to_string(wr2.top) + ")");
    ::ShowWindow(app.hwnd, SW_MAXIMIZE);
    std::this_thread::sleep_for(2500ms);
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    pressDialogButton(app, L"Don't save", nullptr, 5s);
    app.waitExit(20s);
    app.close();
    app.start({});
    std::this_thread::sleep_for(3s);
    check(::IsZoomed(app.hwnd) != FALSE, "closed maximized: opens maximized");
    app.report(r);
    checkOnScreen(app, r, "final start");
    screenshot(out / "uitest-final.png");
    ::PostMessageW(app.hwnd, WM_CLOSE, 0, 0);
    pressDialogButton(app, L"Don't save", nullptr, 5s);
    app.waitExit(20s);
    app.kill();
    app.close();

    char tail[160];
    std::snprintf(tail, sizeof tail, "\n%d checks, %d failed -> %s", gChecks, gFails, gFails ? "FAIL" : "PASS");
    log(tail);
    std::ofstream(out / "uitest-report.txt", std::ios::binary) << gReport;
    gUia->Release();
    ::CoUninitialize();
    return gFails ? 1 : 0;
}

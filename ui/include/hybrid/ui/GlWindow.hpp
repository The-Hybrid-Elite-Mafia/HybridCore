#pragma once
// =============================================================================
//  GlWindow — Win32 window + OpenGL 3.3 core context (WGL), per-monitor DPI aware.
//  Input is queued as plain events for the UI thread to consume each frame.
//  A hidden window is used for off-screen rendering (tests, snapshots).
// =============================================================================
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hc::ui {

struct InputEvent {
    enum class Type : std::uint8_t { MouseDown, MouseUp, MouseMove, Wheel, Resize, Close, RightDown, Drop, Key, OpenFile, MiddleDown, MiddleUp, Command };
    Type  type        = Type::MouseMove;
    float x           = 0.0f;   // client pixels
    float y           = 0.0f;
    float wheel       = 0.0f;   // notches (+ = away from user)
    float wheelX      = 0.0f;   // horizontal notches (tilt wheel / touchpad, + = right)
    bool  doubleClick = false;
    bool  fine        = false;  // Shift or Ctrl held
    bool  ctrl        = false;  // Ctrl held (Key, Wheel)
    bool  shift       = false;  // Shift held (Key, Wheel)
    std::uint32_t key = 0;      // Key: Windows virtual-key code
    std::vector<std::filesystem::path> files;   // Drop: files dragged in from Explorer; OpenFile: path sent by a second instance
    std::string text;                           // Command: UTF-8 text sent with kCommandTag (test automation)
};

class GlWindow {
public:
    GlWindow();
    ~GlWindow();
    GlWindow(const GlWindow&)            = delete;
    GlWindow& operator=(const GlWindow&) = delete;

    // Creates the window and a GL 3.3 core context and makes it current on this thread.
    // clientW/H are in DESIGN-sized logical pixels; they are scaled by the monitor DPI.
    bool create(const std::wstring& title, int clientW, int clientH, bool visible, std::string* error);
    // A child window filling `parentHwnd` (plug-in editors). clientW/H are physical pixels.
    // The host pumps the messages; input arrives through takeEvents() and onTimer runs on the
    // window's thread every `timerMs` (draw there). Uses its own window class, registered with
    // this module and unregistered when the last child closes (safe when a DLL is unloaded).
    bool createChild(void* parentHwnd, int clientW, int clientH, std::string* error);
    void setTimer(unsigned int timerMs, std::function<void()> onTimer);
    void resizeChild(int clientW, int clientH) noexcept;
    // Makes this window's GL context current; restoreContext() puts back whatever was current
    // before (hosts may have their own GL contexts on the same thread).
    void makeCurrent() noexcept;
    void restoreContext() noexcept;
    void destroy() noexcept;

    // Pumps Win32 messages; returns false once the window was closed.
    bool pump();
    [[nodiscard]] std::vector<InputEvent> takeEvents();

    void swap() noexcept;
    void setVsync(bool on) noexcept;
    void setTitle(const std::wstring& title) noexcept;
    // Borderless full screen on the window's monitor (F11 in the apps); false restores the previous window.
    void setFullscreen(bool on) noexcept;
    [[nodiscard]] bool fullscreen() const noexcept;
    // Window class of every GlWindow (used to find a running instance).
    [[nodiscard]] static const wchar_t* className() noexcept;
    // WM_COPYDATA tag: the payload is a UTF-16 file path to open (InputEvent::Type::OpenFile).
    static constexpr unsigned long kOpenFileTag = 0x48435331;   // 'HCS1'
    // WM_COPYDATA tag: a UTF-8 command line for the app (InputEvent::Type::Command). Apps act on it only when they
    // were started for test automation.
    static constexpr unsigned long kCommandTag = 0x48434D44;    // 'HCMD'

    // The close box / Alt+F4 normally ends pump() at once. With this on, they only queue a Close event and the
    // app decides (e.g. "Save changes?"), then calls requestClose().
    void setAppHandlesClose(bool on) noexcept;
    void requestClose() noexcept;
    // Smallest client area the user can drag the window to, in DIPs (scaled by the window's monitor DPI).
    void setMinClientSize(int dipW, int dipH) noexcept;

    [[nodiscard]] int   pixelWidth() const noexcept;
    [[nodiscard]] int   pixelHeight() const noexcept;
    [[nodiscard]] float dpiScale() const noexcept;    // 1.0 = 96 DPI
    [[nodiscard]] bool  minimized() const noexcept;
    [[nodiscard]] void* nativeHandle() const noexcept;   // HWND

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hc::ui

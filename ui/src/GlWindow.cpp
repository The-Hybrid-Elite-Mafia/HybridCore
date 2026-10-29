#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Gl.hpp"

#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>

namespace hc::ui {

struct GlWindow::Impl {
    HWND                    hwnd = nullptr;
    HDC                     hdc  = nullptr;
    HGLRC                   rc   = nullptr;
    bool                    closed = false;
    bool                    appHandlesClose = false;
    int                     minDipW = 0, minDipH = 0;
    bool                    mouseDown = false;
    // While a drag holds the mouse, the cursor that reaches the edge of the screen jumps to the other side and the
    // drag goes on (as in a DAW: a knob never runs out of room); the jump is added back so the app sees one smooth move.
    long                    dragDx = 0, dragDy = 0;

    void wrapAtScreenEdge() {
        POINT p{};
        if (!::GetCursorPos(&p)) return;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(::MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return;
        const RECT& r = mi.rcMonitor;
        const long spanX = (r.right - r.left) - 8, spanY = (r.bottom - r.top) - 8;
        if (spanX < 100 || spanY < 100) return;
        POINT to = p;
        if (p.y <= r.top + 1) { to.y += spanY; dragDy -= spanY; }
        else if (p.y >= r.bottom - 2) { to.y -= spanY; dragDy += spanY; }
        if (p.x <= r.left + 1) { to.x += spanX; dragDx -= spanX; }
        else if (p.x >= r.right - 2) { to.x -= spanX; dragDx += spanX; }
        if (to.x != p.x || to.y != p.y) ::SetCursorPos(to.x, to.y);
    }
    std::vector<InputEvent> events;
    gl::PFN_wglSwapIntervalEXT swapInterval = nullptr;
    bool                    child = false;
    bool                    full = false;
    WINDOWPLACEMENT         placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR                savedStyle = 0;
    std::function<void()>   onTimer;
    HDC                     prevDc = nullptr;
    HGLRC                   prevRc = nullptr;

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return ::DefWindowProcW(h, msg, wp, lp);
        }
        if (!self) return ::DefWindowProcW(h, msg, wp, lp);
        return self->handle(h, msg, wp, lp);
    }

    static bool fineKeys() { return (::GetKeyState(VK_SHIFT) & 0x8000) || (::GetKeyState(VK_CONTROL) & 0x8000); }

    LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        InputEvent e;
        e.fine = fineKeys();
        e.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        e.shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        switch (msg) {
        case WM_CLOSE:
            if (!appHandlesClose) closed = true;
            events.push_back({InputEvent::Type::Close});
            return 0;
        case WM_GETMINMAXINFO: {
            if (minDipW > 0 && minDipH > 0) {
                const UINT dpi = ::GetDpiForWindow(h);
                RECT r{0, 0, ::MulDiv(minDipW, static_cast<int>(dpi), 96), ::MulDiv(minDipH, static_cast<int>(dpi), 96)};
                ::AdjustWindowRectExForDpi(&r, static_cast<DWORD>(::GetWindowLongPtrW(h, GWL_STYLE)), FALSE,
                                           static_cast<DWORD>(::GetWindowLongPtrW(h, GWL_EXSTYLE)), dpi);
                auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
                mmi->ptMinTrackSize.x = r.right - r.left;
                mmi->ptMinTrackSize.y = r.bottom - r.top;
                return 0;
            }
            return ::DefWindowProcW(h, msg, wp, lp);
        }
        case WM_SIZE:
            events.push_back({InputEvent::Type::Resize});
            return 0;
        case WM_ERASEBKGND:
            return 1;   // GL paints everything
        case WM_SYSCOMMAND:
            // ALT (or F10) pressed alone would put the window in "menu mode": a modal loop inside Windows that stops
            // the app's frame loop (nothing redraws or answers) until another key or a click. No menu here: ignore it.
            // Alt+F4, Alt+Space (the window menu) and the title-bar buttons still work.
            if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
            return ::DefWindowProcW(h, msg, wp, lp);
        case WM_TIMER:
            if (onTimer) onTimer();
            return 0;
        case WM_MOUSEACTIVATE:
            if (child) return MA_ACTIVATE;
            return ::DefWindowProcW(h, msg, wp, lp);
        case WM_DPICHANGED: {
            const auto* r = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            events.push_back({InputEvent::Type::Resize});
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            if (child) ::SetFocus(h);   // so the wheel reaches a plug-in view
            ::SetCapture(h);
            mouseDown = true;
            dragDx = dragDy = 0;
            e.type = InputEvent::Type::MouseDown;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            e.doubleClick = (msg == WM_LBUTTONDBLCLK);
            events.push_back(e);
            return 0;
        case WM_LBUTTONUP: {
            // mouseDown goes false BEFORE ReleaseCapture: ReleaseCapture sends WM_CAPTURECHANGED at once, which must not
            // see a press in progress (it used to queue an extra MouseUp at 0,0 first, so a click never reached the
            // control it was on: every push button that fires on release did nothing).
            const bool wasDown = mouseDown;
            mouseDown = false;
            if (wasDown) ::ReleaseCapture();
            e.type = InputEvent::Type::MouseUp;
            e.x = static_cast<float>(GET_X_LPARAM(lp) + dragDx);
            e.y = static_cast<float>(GET_Y_LPARAM(lp) + dragDy);
            dragDx = dragDy = 0;
            events.push_back(e);
            return 0;
        }
        case WM_MOUSEMOVE:
            e.type = InputEvent::Type::MouseMove;
            e.x = static_cast<float>(GET_X_LPARAM(lp) + dragDx);
            e.y = static_cast<float>(GET_Y_LPARAM(lp) + dragDy);
            events.push_back(e);
            if (mouseDown) wrapAtScreenEdge();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ::ScreenToClient(h, &pt);
            e.type  = InputEvent::Type::Wheel;
            e.x     = static_cast<float>(pt.x);
            e.y     = static_cast<float>(pt.y);
            e.wheel = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            events.push_back(e);
            return 0;
        }
        case WM_MOUSEHWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ::ScreenToClient(h, &pt);
            e.type   = InputEvent::Type::Wheel;
            e.x      = static_cast<float>(pt.x);
            e.y      = static_cast<float>(pt.y);
            e.wheelX = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            events.push_back(e);
            return 0;
        }
        case WM_MBUTTONDOWN:
            ::SetCapture(h);
            e.type = InputEvent::Type::MiddleDown;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
            return 0;
        case WM_MBUTTONUP:
            if (!mouseDown) ::ReleaseCapture();
            e.type = InputEvent::Type::MiddleUp;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
            return 0;
        case WM_KEYDOWN:
            e.type = InputEvent::Type::Key;
            e.key  = static_cast<std::uint32_t>(wp);
            events.push_back(e);
            return 0;
        case WM_RBUTTONDOWN:
            e.type = InputEvent::Type::RightDown;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
            return 0;
        case WM_COPYDATA: {   // a second HybridCore.exe hands over a session to open
            const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cd && cd->dwData == GlWindow::kCommandTag && cd->lpData && cd->cbData > 0 && cd->cbData <= 65536) {
                e.type = InputEvent::Type::Command;
                e.text.assign(static_cast<const char*>(cd->lpData), cd->cbData);
                while (!e.text.empty() && e.text.back() == '\0') e.text.pop_back();
                events.push_back(std::move(e));
                return TRUE;
            }
            if (!cd || cd->dwData != GlWindow::kOpenFileTag || !cd->lpData || cd->cbData < sizeof(wchar_t) || cd->cbData > 65536) return FALSE;
            e.type = InputEvent::Type::OpenFile;
            e.files.emplace_back(std::wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)).c_str());
            events.push_back(std::move(e));
            return TRUE;
        }
        case WM_DROPFILES: {
            auto* drop = reinterpret_cast<HDROP>(wp);
            POINT pt{};
            ::DragQueryPoint(drop, &pt);
            e.type = InputEvent::Type::Drop;
            e.x = static_cast<float>(pt.x);
            e.y = static_cast<float>(pt.y);
            const UINT n = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                const UINT len = ::DragQueryFileW(drop, i, nullptr, 0);
                std::wstring name(len, L'\0');
                ::DragQueryFileW(drop, i, name.data(), len + 1);
                e.files.emplace_back(name);
            }
            ::DragFinish(drop);
            events.push_back(std::move(e));
            return 0;
        }
        case WM_CAPTURECHANGED:
            // the press was taken away (another window, a dialog): end it far off any control, so nothing fires
            if (mouseDown) {
                mouseDown = false;
                dragDx = dragDy = 0;
                InputEvent up;
                up.type = InputEvent::Type::MouseUp;
                up.x = up.y = -100000.0f;
                events.push_back(up);
            }
            return 0;
        default:
            return ::DefWindowProcW(h, msg, wp, lp);
        }
    }
};

namespace {
constexpr wchar_t kClassName[] = L"HybridCoreGlWindow";

bool registerClass() {
    static bool done = false;
    if (done) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &GlWindow::Impl::proc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.lpszClassName = kClassName;
    done = ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    return done;
}

constexpr wchar_t kChildClassName[] = L"HEMPluginGlView";
int gChildCount = 0;

HINSTANCE thisModule() {
    HMODULE m = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&thisModule), &m);
    return m;
}

bool registerChildClass() {
    if (gChildCount > 0) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc   = &GlWindow::Impl::proc;
    wc.hInstance     = thisModule();
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kChildClassName;
    return ::RegisterClassExW(&wc) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool setBasicPixelFormat(HDC dc) {
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int fmt = ::ChoosePixelFormat(dc, &pfd);
    return fmt != 0 && ::SetPixelFormat(dc, fmt, &pfd) != FALSE;
}
} // namespace

GlWindow::GlWindow() : impl_(std::make_unique<Impl>()) {}
GlWindow::~GlWindow() { destroy(); }

bool GlWindow::create(const std::wstring& title, int clientW, int clientH, bool visible, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        destroy();
        return false;
    };
    if (!registerClass()) return fail("RegisterClassEx failed");

    // 1) Throw-away context to obtain wglCreateContextAttribsARB.
    HWND dummy = ::CreateWindowExW(0, kClassName, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, ::GetModuleHandleW(nullptr), impl_.get());
    if (!dummy) return fail("dummy window failed");
    HDC   ddc = ::GetDC(dummy);
    HGLRC drc = nullptr;
    gl::PFN_wglCreateContextAttribsARB createAttribs = nullptr;
    if (setBasicPixelFormat(ddc) && (drc = ::wglCreateContext(ddc)) && ::wglMakeCurrent(ddc, drc))
        createAttribs = reinterpret_cast<gl::PFN_wglCreateContextAttribsARB>(::wglGetProcAddress("wglCreateContextAttribsARB"));
    ::wglMakeCurrent(nullptr, nullptr);
    if (drc) ::wglDeleteContext(drc);
    ::ReleaseDC(dummy, ddc);
    ::DestroyWindow(dummy);
    if (!createAttribs) return fail("wglCreateContextAttribsARB unavailable (OpenGL 3.3 driver required)");

    // 2) Real window, sized for the monitor DPI.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    UINT dpi = 96;
    if (HDC sdc = ::GetDC(nullptr)) { dpi = static_cast<UINT>(::GetDeviceCaps(sdc, LOGPIXELSX)); ::ReleaseDC(nullptr, sdc); }
    int cw = ::MulDiv(clientW, static_cast<int>(dpi), 96), ch = ::MulDiv(clientH, static_cast<int>(dpi), 96);
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        // Fit inside 92% of the work area, keeping the aspect ratio.
        const double fit = std::min(0.92 * (work.right - work.left) / cw, 0.92 * (work.bottom - work.top) / ch);
        if (fit < 1.0) { cw = static_cast<int>(cw * fit); ch = static_cast<int>(ch * fit); }
    }
    RECT r{0, 0, cw, ch};
    ::AdjustWindowRectEx(&r, style, FALSE, 0);
    impl_->hwnd = ::CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                    r.right - r.left, r.bottom - r.top, nullptr, nullptr, ::GetModuleHandleW(nullptr), impl_.get());
    if (!impl_->hwnd) return fail("CreateWindowEx failed");
    impl_->hdc = ::GetDC(impl_->hwnd);
    if (!setBasicPixelFormat(impl_->hdc)) return fail("SetPixelFormat failed");

    const int attribs[] = {gl::kWglContextMajorVersion, 3, gl::kWglContextMinorVersion, 3,
                           gl::kWglContextProfileMask, gl::kWglContextCoreProfile, 0};
    impl_->rc = createAttribs(impl_->hdc, nullptr, attribs);
    if (!impl_->rc) return fail("could not create an OpenGL 3.3 core context");
    if (!::wglMakeCurrent(impl_->hdc, impl_->rc)) return fail("wglMakeCurrent failed");
    if (const char* missing = gl::loadFunctions()) {
        static std::string msg;
        msg = std::string("OpenGL function missing: ") + missing;
        return fail(msg.c_str());
    }
    impl_->swapInterval = reinterpret_cast<gl::PFN_wglSwapIntervalEXT>(::wglGetProcAddress("wglSwapIntervalEXT"));
    setVsync(true);
    ::DragAcceptFiles(impl_->hwnd, TRUE);   // stems can be dropped onto the console
    if (visible) { ::ShowWindow(impl_->hwnd, SW_SHOWNORMAL); ::UpdateWindow(impl_->hwnd); }
    return true;
}

void GlWindow::destroy() noexcept {
    if (impl_->hwnd) ::KillTimer(impl_->hwnd, 1);
    impl_->onTimer = nullptr;
    if (impl_->rc) {
        if (::wglGetCurrentContext() == impl_->rc) ::wglMakeCurrent(nullptr, nullptr);
        ::wglDeleteContext(impl_->rc);
        impl_->rc = nullptr;
    }
    if (impl_->hdc && impl_->hwnd) { ::ReleaseDC(impl_->hwnd, impl_->hdc); impl_->hdc = nullptr; }
    if (impl_->hwnd) { ::DestroyWindow(impl_->hwnd); impl_->hwnd = nullptr; }
    if (impl_->child) {
        impl_->child = false;
        if (--gChildCount == 0) ::UnregisterClassW(kChildClassName, thisModule());
    }
}

bool GlWindow::createChild(void* parentHwnd, int clientW, int clientH, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        destroy();
        return false;
    };
    if (!registerChildClass()) { if (error) *error = "RegisterClassEx (plug-in view) failed"; return false; }
    ++gChildCount;
    impl_->child = true;
    const HDC   prevDc = ::wglGetCurrentDC();
    const HGLRC prevRc = ::wglGetCurrentContext();
    auto restore = [&] { ::wglMakeCurrent(prevDc, prevRc); };

    // 1) Throw-away context to obtain wglCreateContextAttribsARB.
    HWND dummy = ::CreateWindowExW(0, kChildClassName, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, thisModule(), impl_.get());
    if (!dummy) return fail("dummy window failed");
    HDC   ddc = ::GetDC(dummy);
    HGLRC drc = nullptr;
    gl::PFN_wglCreateContextAttribsARB createAttribs = nullptr;
    if (setBasicPixelFormat(ddc) && (drc = ::wglCreateContext(ddc)) && ::wglMakeCurrent(ddc, drc))
        createAttribs = reinterpret_cast<gl::PFN_wglCreateContextAttribsARB>(::wglGetProcAddress("wglCreateContextAttribsARB"));
    ::wglMakeCurrent(nullptr, nullptr);
    if (drc) ::wglDeleteContext(drc);
    ::ReleaseDC(dummy, ddc);
    ::DestroyWindow(dummy);
    if (!createAttribs) { restore(); return fail("wglCreateContextAttribsARB unavailable (OpenGL 3.3 driver required)"); }

    impl_->hwnd = ::CreateWindowExW(0, kChildClassName, L"HEM", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0,
                                    clientW, clientH, static_cast<HWND>(parentHwnd), nullptr, thisModule(), impl_.get());
    if (!impl_->hwnd) { restore(); return fail("CreateWindowEx (plug-in view) failed"); }
    impl_->hdc = ::GetDC(impl_->hwnd);
    if (!setBasicPixelFormat(impl_->hdc)) { restore(); return fail("SetPixelFormat failed"); }
    const int attribs[] = {gl::kWglContextMajorVersion, 3, gl::kWglContextMinorVersion, 3,
                           gl::kWglContextProfileMask, gl::kWglContextCoreProfile, 0};
    impl_->rc = createAttribs(impl_->hdc, nullptr, attribs);
    if (!impl_->rc) { restore(); return fail("could not create an OpenGL 3.3 core context"); }
    if (!::wglMakeCurrent(impl_->hdc, impl_->rc)) { restore(); return fail("wglMakeCurrent failed"); }
    if (const char* missing = gl::loadFunctions()) {
        static std::string msg;
        msg = std::string("OpenGL function missing: ") + missing;
        restore();
        return fail(msg.c_str());
    }
    impl_->swapInterval = reinterpret_cast<gl::PFN_wglSwapIntervalEXT>(::wglGetProcAddress("wglSwapIntervalEXT"));
    if (impl_->swapInterval) impl_->swapInterval(0);   // the host's message loop must never wait on our vsync
    // Leave our context current for the caller's set-up (renderer, textures); it calls restoreContext().
    impl_->prevDc = prevDc;
    impl_->prevRc = prevRc;
    return true;
}

void GlWindow::setTimer(unsigned int timerMs, std::function<void()> onTimer) {
    impl_->onTimer = std::move(onTimer);
    if (impl_->hwnd) ::SetTimer(impl_->hwnd, 1, timerMs, nullptr);
}

void GlWindow::resizeChild(int w, int h) noexcept {
    if (impl_->hwnd) ::SetWindowPos(impl_->hwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
}

void GlWindow::makeCurrent() noexcept {
    impl_->prevDc = ::wglGetCurrentDC();
    impl_->prevRc = ::wglGetCurrentContext();
    if (impl_->hdc && impl_->rc) ::wglMakeCurrent(impl_->hdc, impl_->rc);
}

void GlWindow::restoreContext() noexcept {
    ::wglMakeCurrent(impl_->prevDc, impl_->prevRc);
    impl_->prevDc = nullptr;
    impl_->prevRc = nullptr;
}

bool GlWindow::pump() {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    return !impl_->closed;
}

std::vector<InputEvent> GlWindow::takeEvents() {
    std::vector<InputEvent> out;
    out.swap(impl_->events);
    return out;
}

void GlWindow::swap() noexcept { if (impl_->hdc) ::SwapBuffers(impl_->hdc); }
void GlWindow::setVsync(bool on) noexcept { if (impl_->swapInterval) impl_->swapInterval(on ? 1 : 0); }
const wchar_t* GlWindow::className() noexcept { return kClassName; }
void GlWindow::setAppHandlesClose(bool on) noexcept { impl_->appHandlesClose = on; }
void GlWindow::requestClose() noexcept { impl_->closed = true; }
void GlWindow::setMinClientSize(int dipW, int dipH) noexcept { impl_->minDipW = dipW; impl_->minDipH = dipH; }
void GlWindow::setTitle(const std::wstring& t) noexcept { if (impl_->hwnd) ::SetWindowTextW(impl_->hwnd, t.c_str()); }

void GlWindow::setFullscreen(bool on) noexcept {
    Impl& d = *impl_;
    if (!d.hwnd || d.child || on == d.full) return;
    if (on) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetWindowPlacement(d.hwnd, &d.placement) || !::GetMonitorInfoW(::MonitorFromWindow(d.hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        d.savedStyle = ::GetWindowLongPtrW(d.hwnd, GWL_STYLE);
        ::SetWindowLongPtrW(d.hwnd, GWL_STYLE, (d.savedStyle & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW)) | WS_POPUP | WS_VISIBLE);
        const RECT& r = mi.rcMonitor;
        ::SetWindowPos(d.hwnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        ::SetWindowLongPtrW(d.hwnd, GWL_STYLE, d.savedStyle);
        ::SetWindowPlacement(d.hwnd, &d.placement);
        ::SetWindowPos(d.hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    d.full = on;
    d.events.push_back({InputEvent::Type::Resize});
}
bool GlWindow::fullscreen() const noexcept { return impl_->full; }

int GlWindow::pixelWidth() const noexcept {
    RECT r{};
    if (impl_->hwnd) ::GetClientRect(impl_->hwnd, &r);
    return std::max(1L, r.right - r.left);
}
int GlWindow::pixelHeight() const noexcept {
    RECT r{};
    if (impl_->hwnd) ::GetClientRect(impl_->hwnd, &r);
    return std::max(1L, r.bottom - r.top);
}
float GlWindow::dpiScale() const noexcept {
    return impl_->hwnd ? static_cast<float>(::GetDpiForWindow(impl_->hwnd)) / 96.0f : 1.0f;
}
bool  GlWindow::minimized() const noexcept { return impl_->hwnd && ::IsIconic(impl_->hwnd); }
void* GlWindow::nativeHandle() const noexcept { return impl_->hwnd; }

} // namespace hc::ui

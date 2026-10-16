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
    bool                    mouseDown = false;
    std::vector<InputEvent> events;
    gl::PFN_wglSwapIntervalEXT swapInterval = nullptr;

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
        switch (msg) {
        case WM_CLOSE:
            closed = true;
            events.push_back({InputEvent::Type::Close});
            return 0;
        case WM_SIZE:
            events.push_back({InputEvent::Type::Resize});
            return 0;
        case WM_ERASEBKGND:
            return 1;   // GL paints everything
        case WM_DPICHANGED: {
            const auto* r = reinterpret_cast<const RECT*>(lp);
            ::SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            events.push_back({InputEvent::Type::Resize});
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            ::SetCapture(h);
            mouseDown = true;
            e.type = InputEvent::Type::MouseDown;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            e.doubleClick = (msg == WM_LBUTTONDBLCLK);
            events.push_back(e);
            return 0;
        case WM_LBUTTONUP:
            if (mouseDown) ::ReleaseCapture();
            mouseDown = false;
            e.type = InputEvent::Type::MouseUp;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
            return 0;
        case WM_MOUSEMOVE:
            e.type = InputEvent::Type::MouseMove;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
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
        case WM_KEYDOWN:
            e.type = InputEvent::Type::Key;
            e.key  = static_cast<std::uint32_t>(wp);
            e.ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            events.push_back(e);
            return 0;
        case WM_RBUTTONDOWN:
            e.type = InputEvent::Type::RightDown;
            e.x = static_cast<float>(GET_X_LPARAM(lp));
            e.y = static_cast<float>(GET_Y_LPARAM(lp));
            events.push_back(e);
            return 0;
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
            if (mouseDown) { mouseDown = false; events.push_back({InputEvent::Type::MouseUp}); }
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
    if (impl_->rc) { ::wglMakeCurrent(nullptr, nullptr); ::wglDeleteContext(impl_->rc); impl_->rc = nullptr; }
    if (impl_->hdc && impl_->hwnd) { ::ReleaseDC(impl_->hwnd, impl_->hdc); impl_->hdc = nullptr; }
    if (impl_->hwnd) { ::DestroyWindow(impl_->hwnd); impl_->hwnd = nullptr; }
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
void GlWindow::setTitle(const std::wstring& t) noexcept { if (impl_->hwnd) ::SetWindowTextW(impl_->hwnd, t.c_str()); }

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
void GlWindow::setTitle(const std::wstring& title) noexcept { if (impl_->hwnd) ::SetWindowTextW(impl_->hwnd, title.c_str()); }

} // namespace hc::ui

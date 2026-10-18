#include "hybrid/ui/FxPanelWindow.hpp"
#include "hybrid/ui/Faceplate.hpp"
#include "hybrid/ui/Parts.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <optional>

namespace hc::ui {
namespace {

// Built once per process: decoding the 4K faceplate and rendering the knob parts takes a
// moment, and every open plug-in window uses the same pieces.
struct SharedArt {
    std::mutex                mutex;
    std::filesystem::path     dir;
    std::optional<FontSet>    fonts;
    std::optional<PartsAtlas> parts;
};
SharedArt& shared() {
    static SharedArt s;
    return s;
}

bool loadShared(const std::filesystem::path& assetDir, FontSet& fontsOut, PartsAtlas& partsOut, std::string* error) {
    SharedArt& s = shared();
    std::lock_guard lock(s.mutex);
    if (!s.fonts || !s.parts || s.dir != assetDir) {
        auto fonts = FontSet::load(assetDir / "fonts", error);
        if (!fonts) return false;
        auto art = loadFaceplateArt(assetDir / "faceplates" / "Main-Faceplate.png", error);
        if (!art) return false;
        s.parts = buildPartsAtlas(art->master);
        s.fonts = std::move(*fonts);
        s.dir = assetDir;
    }
    fontsOut = *s.fonts;
    partsOut = *s.parts;
    return true;
}

} // namespace

void FxPanelWindow::sizeFor(const PanelSpec& spec, float scale, int& width, int& height) {
    const PanelLayout L = layoutFxPanel(spec);
    width = std::max(320, static_cast<int>(std::lround(L.width * scale)));
    height = std::max(200, static_cast<int>(std::lround(L.height * scale)));
}

float FxPanelWindow::defaultScale(const PanelSpec& spec) noexcept {
    const UINT dpi = ::GetDpiForSystem();
    float scale = 0.62f * static_cast<float>(dpi ? dpi : 96) / 96.0f;
    RECT work{};
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        const PanelLayout L = layoutFxPanel(spec);
        const float fitW = 0.9f * static_cast<float>(work.right - work.left) / L.width;
        const float fitH = 0.85f * static_cast<float>(work.bottom - work.top) / L.height;
        scale = std::max(0.3f, std::min({scale, fitW, fitH}));
    }
    return scale;
}

bool FxPanelWindow::setUp(const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir, int w, int h, std::string* error) {
    spec_ = &spec;
    host_ = &host;
    layout_ = std::make_unique<PanelLayout>(layoutFxPanel(spec));
    view_ = std::make_unique<FxPanelView>(*layout_, spec);
    controller_ = std::make_unique<FxPanelController>(*layout_, spec, host);
    FontSet fonts;
    PartsAtlas parts;
    if (!loadShared(assetDir, fonts, parts, error)) return false;
    if (!renderer_.init(4, error)) return false;
    if (!renderer_.setParts(parts, error)) return false;
    w_ = w;
    h_ = h;
    canvas_ = Canvas{};
    canvas_.scale = std::min(static_cast<float>(w_) / layout_->width, static_cast<float>(h_) / layout_->height);
    canvas_.offsetX = std::round((static_cast<float>(w_) - layout_->width * canvas_.scale) * 0.5f);
    canvas_.offsetY = std::round((static_cast<float>(h_) - layout_->height * canvas_.scale) * 0.5f);
    canvas_.pixelW = static_cast<float>(w_);
    canvas_.pixelH = static_cast<float>(h_);
    if (!font_.bake(fonts, canvas_.scale)) { if (error) *error = "could not bake the Poppins atlas"; return false; }
    if (!renderer_.setFont(font_, error)) return false;
    // The obsidian plate at exactly the window's pixel density (capped at the 4096 px texture limit).
    const float ppu = std::min({canvas_.scale, 4096.0f / layout_->width, 4096.0f / layout_->height});
    if (!renderer_.setPanel(buildMips(premultiply(renderFxPlate(*layout_, FxPalette{}, ppu))), error)) return false;
    if (!renderer_.setTargetSize(w_, h_, error)) return false;
    renderer_.beginStatic(canvas_);
    renderer_.panel({0, 0, layout_->width, layout_->height});
    renderer_.endStatic();
    open_ = true;
    return true;
}

bool FxPanelWindow::openChild(void* parentHwnd, const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir,
                              float scale, std::string* error) {
    close();
    int w = 0, h = 0;
    sizeFor(spec, scale, w, h);
    if (!window_.createChild(parentHwnd, w, h, error)) return false;
    child_ = true;
    const bool ok = setUp(spec, host, assetDir, w, h, error);
    window_.restoreContext();
    if (!ok) { close(); return false; }
    window_.setTimer(33, [this] { frame(); });
    return true;
}

bool FxPanelWindow::openHidden(const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir, float scale, std::string* error) {
    close();
    int w = 0, h = 0;
    sizeFor(spec, scale, w, h);
    // The hidden window only carries the GL context; frames are rendered off-screen at w × h.
    if (!window_.create(L"HEM plug-in snapshot", 64, 64, false, error)) return false;
    child_ = false;
    if (!setUp(spec, host, assetDir, w, h, error)) { close(); return false; }
    return true;
}

void FxPanelWindow::close() noexcept {
    if (!open_ && !window_.nativeHandle()) return;
    if (window_.nativeHandle()) {
        window_.makeCurrent();
        renderer_.shutdown();
        window_.restoreContext();
    }
    window_.destroy();
    view_.reset();
    controller_.reset();
    layout_.reset();
    open_ = false;
}

void FxPanelWindow::handleInput() {
    for (const InputEvent& e : window_.takeEvents()) {
        const float x = canvas_.toDesignX(e.x), y = canvas_.toDesignY(e.y);
        switch (e.type) {
        case InputEvent::Type::MouseDown: controller_->mouseDown(x, y, e.doubleClick, e.fine); break;
        case InputEvent::Type::MouseMove: controller_->mouseMove(x, y, e.fine); break;
        case InputEvent::Type::MouseUp:   controller_->mouseUp(x, y); break;
        case InputEvent::Type::Wheel:     controller_->wheel(x, y, e.wheel, e.fine); break;
        default: break;
        }
    }
}

void FxPanelWindow::draw() {
    renderer_.beginFrame(canvas_);
    view_->draw(renderer_, *host_, controller_->hover(), controller_->active(), 0, 0, false);
}

void FxPanelWindow::frame() {
    if (!open_) return;
    window_.makeCurrent();
    handleInput();
    draw();
    renderer_.endFrame(0);
    window_.swap();
    window_.restoreContext();
}

Image FxPanelWindow::snapshot() {
    if (!open_) return {};
    window_.makeCurrent();
    draw();
    renderer_.endFrameToCapture();
    Image img = renderer_.capture();
    window_.restoreContext();
    return img;
}

} // namespace hc::ui

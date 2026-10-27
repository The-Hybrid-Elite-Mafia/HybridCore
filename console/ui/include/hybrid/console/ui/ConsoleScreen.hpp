#pragma once
// =============================================================================
//  ConsoleScreen — the HEM DIGITAL CONSOLE on the GPU (OpenGL 3.3 through hc::ui::Renderer).
//  Owns the renderer, fonts, knob parts, the six faceplates and their obsidian plates, and keeps
//  them matched to the framebuffer:
//    * the plate of every page is routed on the CPU in the background (renderFxPlate) at the
//      window's pixel density, cached, and uploaded when its page is shown (never wider than 4096 px)
//    * Poppins is re-baked when the scale changes; the static layer (plate + engraving) is
//      re-recorded only when the page, the size or the plate changes
//  Every control is drawn live each frame on top: nothing is painted into the plate.
//  UI thread only (needs the current GL context). Used by the app and by the snapshot tool.
// =============================================================================
#include "hybrid/console/ui/ConsoleUi.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/Parts.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hc::console::ui {

class ConsoleScreen {
public:
    // `rig` and `app` must outlive the screen. `onAction` receives every app action (page changes are handled here
    // and also reported).
    ConsoleScreen(ConsoleRig& rig, AppState& app, std::function<void(const ActionEvent&)> onAction);
    ~ConsoleScreen();
    ConsoleScreen(const ConsoleScreen&) = delete;
    ConsoleScreen& operator=(const ConsoleScreen&) = delete;

    // assetDir holds fonts/ and faceplates/Main-Faceplate.png (the knob parts are cut from it).
    bool init(const std::filesystem::path& assetDir, int msaaSamples, std::string* error);
    bool resize(int framebufferW, int framebufferH, std::string* error);
    void shutdown() noexcept;

    void setPage(PageId page);
    [[nodiscard]] PageId page() const noexcept { return page_; }
    [[nodiscard]] const Page& currentPage() const noexcept { return pages_[static_cast<std::size_t>(page_)]; }

    // Input in framebuffer pixels.
    void mouseDown(float px, float py, bool doubleClick, bool fine);
    void rightDown(float px, float py);
    void mouseMove(float px, float py, bool fine);
    void mouseUp(float px, float py);
    // Wheel: over a control it moves the control; Ctrl + wheel zooms around the mouse; elsewhere (zoomed in) it
    // scrolls the view (Shift + wheel or a sideways wheel: across).
    void wheel(float px, float py, float notches, bool fine, bool ctrl = false, bool shift = false, float notchesX = 0.0f);
    // Middle-button drag moves the view (as does a left drag on bare plate while zoomed in).
    void middleDown(float px, float py);
    void middleUp(float px, float py);
    // Keys the screen handles itself: Ctrl + plus / minus / 0 (zoom), Left / Right on the BIG DESK page.
    // True if the key was used.
    bool key(std::uint32_t virtualKey, bool ctrl);
    // View zoom (1 = fit). Kept in AppState::zoom (the app saves it); anchored at a window pixel.
    void setZoom(float zoom, float anchorPx, float anchorPy);
    [[nodiscard]] float zoom() const noexcept { return zoomApplied_; }
    // Moves the view by window pixels (no effect at zoom 1).
    void scrollBy(float dxPx, float dyPx);

    void render(unsigned int targetFbo);
    void renderToCapture();
    [[nodiscard]] hc::ui::Image capture() const { return renderer_.capture(); }
    // Blocks until the current page's plate is routed (snapshots / tests). False on timeout.
    bool waitForPlate(double seconds);

    [[nodiscard]] const hc::ui::Renderer& renderer() const noexcept { return renderer_; }
    [[nodiscard]] const hc::ui::Canvas& canvas() const noexcept { return canvas_; }
    [[nodiscard]] double lastPlateMs() const noexcept { return plateMs_; }
    [[nodiscard]] double lastFrameCpuMs() const noexcept { return frameMs_; }

private:
    struct Plate {
        std::future<hc::ui::TextureLevels> job;
        std::optional<hc::ui::TextureLevels> tex;
        float ppu = 0;
        double ms = 0;
    };
    void routePlates();
    void layoutCanvas();
    void drawScrollbars();
    bool ensurePlate();
    void drawStatic();
    void drawFrame();
    void handle(const ActionEvent& e);
    [[nodiscard]] float targetSlotCorrelation(const Widget& w) const;
    [[nodiscard]] float toX(float px) const noexcept { return canvas_.toDesignX(px); }
    [[nodiscard]] float toY(float py) const noexcept { return canvas_.toDesignY(py); }

    ConsoleRig& rig_;
    AppState& app_;
    std::function<void(const ActionEvent&)> onAction_;
    std::vector<Page> pages_;
    Controller controller_;
    PageId page_ = PageId::Desk;
    hc::ui::Renderer renderer_;
    hc::ui::FontSet fonts_;
    hc::ui::FontAtlas font_;
    hc::ui::Canvas canvas_;
    std::array<Plate, kPageCount> plates_;
    float plateBucket_ = 0;
    float fontScale_ = 0;
    int uploadedPage_ = -1;
    bool staticDirty_ = true, haveCanvas_ = false, ready_ = false;
    double plateMs_ = 0, frameMs_ = 0;
    int fbW_ = 0, fbH_ = 0;
    float zoomApplied_ = 1.0f;
    float viewX_ = 0, viewY_ = 0;            // design point at the window's top-left while zoomed in
    bool panning_ = false, leftPan_ = false;
    float panLastX_ = 0, panLastY_ = 0;
    struct ViewState;
    std::unique_ptr<ViewState> view_;
};

} // namespace hc::console::ui

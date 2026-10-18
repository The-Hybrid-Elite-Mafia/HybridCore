#pragma once
// =============================================================================
//  FxPanelWindow — a HEM plug-in faceplate in its own OpenGL window:
//    openChild()  : inside a host's window (VST3 editor); redraws itself ~30× a second
//    openHidden() : off-screen, for snapshots (CI screenshots of every faceplate)
//  Fonts and the knob parts are built once per process and shared by every open panel.
//  Message / UI thread only.
// =============================================================================
#include "hybrid/ui/FxPanel.hpp"
#include "hybrid/ui/FxPanelView.hpp"
#include "hybrid/ui/FontAtlas.hpp"
#include "hybrid/ui/GlWindow.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace hc::ui {

class FxPanelWindow {
public:
    FxPanelWindow() = default;
    ~FxPanelWindow() { close(); }
    FxPanelWindow(const FxPanelWindow&)            = delete;
    FxPanelWindow& operator=(const FxPanelWindow&) = delete;

    // Pixel size the panel wants at a given scale (pixels per panel design unit).
    static void sizeFor(const PanelSpec& spec, float scale, int& width, int& height);
    // Scale for an editor on this computer: ~0.62 px per unit at 100 % Windows scaling, smaller if
    // the panel would not fit on the screen (keeps 10 % of the work area free).
    [[nodiscard]] static float defaultScale(const PanelSpec& spec) noexcept;

    // `assetDir` holds fonts/ and faceplates/Main-Faceplate.png. `spec` and `host` must outlive the window.
    bool openChild(void* parentHwnd, const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir,
                   float scale, std::string* error);
    bool openHidden(const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir, float scale, std::string* error);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    void frame();                    // input + draw + present (child windows: called by the timer)
    [[nodiscard]] Image snapshot();  // draw once into the capture buffer and read it back
    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }

private:
    bool setUp(const PanelSpec& spec, PanelHost& host, const std::filesystem::path& assetDir, int w, int h, std::string* error);
    void handleInput();
    void draw();

    GlWindow   window_;
    Renderer   renderer_;
    FontAtlas  font_;
    std::unique_ptr<PanelLayout>       layout_;
    std::unique_ptr<FxPanelView>       view_;
    std::unique_ptr<FxPanelController> controller_;
    const PanelSpec* spec_ = nullptr;
    PanelHost*       host_ = nullptr;
    Canvas canvas_;
    int    w_ = 0, h_ = 0;
    bool   open_ = false, child_ = false;
};

} // namespace hc::ui

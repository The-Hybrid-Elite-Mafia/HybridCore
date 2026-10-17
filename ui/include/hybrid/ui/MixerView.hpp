#pragma once
// =============================================================================
//  MixerView — draws the console on the faceplate with the Renderer.
//    drawStatic : chassis (the PNG), strip glass, sockets, slots, scales, labels.
//                 Rendered once into the renderer's cached layer (on resize / layout change).
//    drawDynamic: every frame — knobs, caps, faders, buttons, meters, live readouts.
//  Reads parameters (atomics) and meter values only; never touches DSP objects.
// =============================================================================
#include "hybrid/core/Config.hpp"
#include "hybrid/params/ParameterTree.hpp"
#include "hybrid/ui/Layout.hpp"
#include "hybrid/ui/Renderer.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hc::analysis { class Analyzer; }

namespace hc::ui {

// How the ANALYZER's 3D view is turned (radians). Drag in the view to turn it.
struct ScopeCamera {
    static constexpr float kYaw = -0.38f, kPitch = 0.52f;
    float yaw   = kYaw;     // turn left / right, -0.9 .. 0.9
    float pitch = kPitch;   // look down, 0.15 .. 1.1
};

// Linear peak levels from the engine plus UI-side ballistics (decay + peak hold).
struct MeterState {
    std::array<float, hc::kStemCount> stem{};
    std::array<float, hc::kBusCount>  busL{}, busR{};
    std::array<float, hc::kStemCount> stemHold{};
    std::array<float, hc::kBusCount>  busHoldL{}, busHoldR{};

    // Feed new linear peaks; dt in seconds. Falls 24 dB/s, holds peaks 1.5 s.
    void update(const float* stemPeaks, const float* busPeaksL, const float* busPeaksR, float dt) noexcept;

private:
    std::array<float, hc::kStemCount> stemHoldAge_{};
    std::array<float, hc::kBusCount>  busHoldAgeL_{}, busHoldAgeR_{};
};

// Live engine readout for the glass display (all measured by the engine / driver).
struct EngineReadout {
    bool          valid        = false;   // false: no stream running
    std::string   driver;                 // "WASAPI", "ASIO", "OFFLINE CLOCK"
    double        sampleRate   = 0.0;
    std::uint32_t bufferFrames = 0;
    double        latencyMs    = -1.0;    // output latency reported by the driver (-1 unknown)
    float         loadPct      = 0.0f;    // average DSP load of the last interval
    float         peakLoadPct  = 0.0f;
    std::uint64_t dropouts     = 0;
};

enum class SourceView : std::uint8_t { Tones, Inputs, Stems };

// What a stem strip shows in its title area.
struct StemSlotView {
    std::string name;              // loaded file name ("" = empty slot)
    std::string info;              // e.g. "MP3 44.1k 3:42"
    float       progress = -1.0f;  // 0..1 while loading
    bool        failed   = false;
    float       glow     = 0.0f;   // 0..1 name flash (Beat Forge note hits)
};

struct TransportView {
    bool   playing  = false;
    bool   loop     = true;
    double position = 0.0;   // seconds
    double length   = 0.0;   // seconds (0 = nothing loaded)
};

struct ViewState {
    std::string   deviceName;      // name panel, line 1
    std::string   deviceFormat;    // name panel, line 2
    std::string   status;          // bottom bar
    bool          alert   = false;
    bool          running = false;
    SourceView    source  = SourceView::Tones;
    bool          loading = false; // any stem loading
    std::array<StemSlotView, hc::kStemCount> slots{};
    TransportView transport;
    const std::vector<float>* overview = nullptr;   // song overview, peak 0..1 per bin (null = nothing loaded)
    std::array<float, hc::kStemCount> gainReduction{};   // compressor gain reduction per stem, dB >= 0
    float         limiterGr = 0.0f;                       // main limiter gain reduction, dB >= 0
    float         exportProgress = -1.0f;                 // 0..1 while a WAV export runs, -1 otherwise
    const std::vector<std::uint8_t>* sections = nullptr;  // Beat Forge song form: forge::Section per bar (null = none)
    double        barSeconds = 0.0;                       // length of one bar at the Beat Forge tempo
    EngineReadout engine;
    // ANALYZER (over the stem strips while `scope` is on).
    bool                      scope    = false;
    const analysis::Analyzer* analyzer = nullptr;
    ScopeCamera               camera;
    std::string               scopeSource;   // what is playing: a file name, or "" for the console's Main out
    int                       resetStage = 0;   // RESET button: 0 = ready, 1 = "SURE?" (press again), 2 = "UNDO" offered
    int           hover   = -1;    // widget indices from Interaction
    int           active  = -1;
    int           pressed = -1;
};

class MixerView {
public:
    MixerView(const Layout& layout, const params::ParameterTree& tree) noexcept : layout_(layout), tree_(tree) {}

    void drawStatic(Renderer& r, const Canvas& canvas) const;
    void drawDynamic(Renderer& r, const ViewState& view, const MeterState& meters) const;

    // Fader-taper position (0..1) for a linear level, so meters line up with fader scales.
    [[nodiscard]] static float levelToNorm(float linear) noexcept;

private:
    void drawChassis(Renderer& r, const Canvas& canvas) const;
    void drawStrip(Renderer& r, const StripLayout& st) const;
    void drawEmissiveStatic(Renderer& r, bool scope) const;
    void drawKnobStatic(Renderer& r, const Widget& w) const;
    void drawKnob(Renderer& r, const Widget& w, Rgba accent, bool hot) const;
    void drawButton(Renderer& r, const Widget& w, bool pressed) const;
    void drawCap(Renderer& r, Rect b, const std::string& caption, bool on, bool pressed, Rgba lit, bool bigText = false) const;
    void drawFader(Renderer& r, const StripLayout& st, Rgba lineColour, const Widget* focusKnob = nullptr) const;
    void drawMeterColumn(Renderer& r, Rect col, float level, float hold, bool lit) const;
    void drawMeterRow(Renderer& r, Rect row, float level, float hold, bool lit) const;
    void drawTransport(Renderer& r, const Widget& w, const ViewState& v) const;
    void drawDisplay(Renderer& r, const ViewState& v) const;
    void drawOverview(Renderer& r, const ViewState& v) const;
    void drawGlassOverlay(Renderer& r, Rect glass) const;

    const Layout&                layout_;
    const params::ParameterTree& tree_;
};

} // namespace hc::ui

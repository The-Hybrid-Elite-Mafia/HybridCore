#pragma once
// HEM front-panel controls: the LOGIC only (no drawing, no Windows). A knob holds a value and moves when dragged.
// Pure C++ so the same code is tested on any machine and drawn by HemUi.cpp on Windows.
#include <algorithm>
#include <cmath>
#include <string>

namespace hem::ui {

struct Knob {
    float cx = 0, cy = 0, r = 60;       // centre and radius on the plate
    double lo = 0, hi = 1, def = 0.5;   // range and the value a double-click returns to
    double step = 0;                    // 0 = smooth; otherwise the value snaps to lo + n * step
    double value = 0.5;
    const wchar_t* legend = L"";        // text under the knob
    const wchar_t* unit = L"";          // text after the number
    bool dragging = false;
    float y0 = 0; double v0 = 0;
    static constexpr double kPixelsFullRange = 220.0;   // pixels of mouse travel for the whole range
    static constexpr double kFineFactor = 4.0;          // Shift held: 4 times finer

    [[nodiscard]] bool hit(float x, float y) const { const float dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= (r * 1.15f) * (r * 1.15f); }
    [[nodiscard]] double t01() const { return hi > lo ? (value - lo) / (hi - lo) : 0.0; }
    void set(double v) {
        v = std::clamp(v, lo, hi);
        if (step > 0) v = std::clamp(lo + std::round((v - lo) / step) * step, lo, hi);
        value = v;
    }
    void begin(float y) { dragging = true; y0 = y; v0 = value; }
    void drag(float y, bool fine) { if (dragging) set(v0 + (static_cast<double>(y0) - static_cast<double>(y)) / (kPixelsFullRange * (fine ? kFineFactor : 1.0)) * (hi - lo)); }
    void end() { dragging = false; }
    void reset() { set(def); }
    void wheel(int notches) { set(value + notches * (step > 0 ? step : (hi - lo) / 100.0)); }
};

struct Button {
    float x = 0, y = 0, w = 100, h = 50;
    const wchar_t* label = L"";
    bool enabled = true;
    bool lit = false;                   // small lamp on the button
    bool down = false;                  // mouse is holding it
    [[nodiscard]] bool hit(float px, float py) const { return px >= x && px <= x + w && py >= y && py <= y + h; }
};

// Plate layout, in plate pixels (1673 x 940). Drawing and mouse tests both use these numbers.
namespace layout {
inline constexpr int kW = 1673, kH = 940;
struct R { float x, y, w, h; };
inline constexpr R kBig    {180, 80, 1360, 340};
inline constexpr R kSmall0 {180, 480, 270, 140};
inline constexpr R kSmall1 {490, 480, 270, 140};
inline constexpr R kSmall2 {800, 480, 300, 140};
inline constexpr R kStatus {750, 760, 790, 76};
inline constexpr float kKnobX = 1335, kKnobY = 548, kKnobR = 70;
inline constexpr R kBtn0 {180, 760, 150, 76};
inline constexpr R kBtn1 {360, 760, 150, 76};
inline constexpr R kBtn2 {540, 760, 190, 76};
} // namespace layout

} // namespace hem::ui

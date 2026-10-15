#pragma once
// =============================================================================
//  Basic 2D types shared by the UI. Coordinates are DESIGN units: the pixel grid of
//  Main-Faceplate.png (3840 × 2160). The Canvas (Faceplate.hpp) maps them to the window.
// =============================================================================
#include <cstdint>

namespace hc::ui {

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] constexpr bool  contains(float px, float py) const noexcept { return px >= x && py >= y && px < x + w && py < y + h; }
    [[nodiscard]] constexpr float cx() const noexcept { return x + w * 0.5f; }
    [[nodiscard]] constexpr float cy() const noexcept { return y + h * 0.5f; }
    [[nodiscard]] constexpr float right() const noexcept { return x + w; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + h; }
    [[nodiscard]] constexpr Rect  inset(float d) const noexcept { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    [[nodiscard]] constexpr Rect  inset(float dx, float dy) const noexcept { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    [[nodiscard]] constexpr Rect  offset(float dx, float dy) const noexcept { return {x + dx, y + dy, w, h}; }
};

struct Circle {
    float cx = 0, cy = 0, r = 0;
};

// Colour as authored: sRGB components 0..1, straight alpha. The renderer converts to
// linear premultiplied.
struct Rgba {
    float r = 1, g = 1, b = 1, a = 1;
    [[nodiscard]] constexpr Rgba withAlpha(float na) const noexcept { return {r, g, b, na}; }
};

[[nodiscard]] constexpr Rgba rgb8(std::uint32_t hex, float a = 1.0f) noexcept {
    return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
            static_cast<float>(hex & 0xFF) / 255.0f, a};
}

} // namespace hc::ui

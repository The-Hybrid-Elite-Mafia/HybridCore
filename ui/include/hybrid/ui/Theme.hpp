#pragma once
// =============================================================================
//  Themes: Pearl White, Obsidian, Titanium — colours for live-drawn widgets plus a
//  procedural faceplate generator used when no custom art is installed.
//  Custom art: put pearl.png / obsidian.png / titanium.png (any size, 8K is fine)
//  in %APPDATA%\HybridCore\skins\ — it is baked down to the screen tier on load.
// =============================================================================
#include "hybrid/ui/Image.hpp"
#include "hybrid/ui/Layout.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace hc::ui {

enum class ThemeId : std::uint8_t { Pearl, Obsidian, Titanium };
inline constexpr std::uint32_t kThemeCount = 3;

struct Rgb { float r, g, b; };   // sRGB 0..1

struct Palette {
    const char* name;
    const char* fileStem;        // skin file name without extension
    Rgb plateTop, plateBottom, recess, groove;
    Rgb knobBody, knobRim, indicator;
    Rgb text, textDim;
    Rgb neon, neonAlt;
    Rgb buttonOff;
    Rgb meterLow, meterMid, meterHigh;
    float brushed;               // streak strength
    float gloss;                 // diagonal specular band
    float sheen;                 // iridescent shimmer (pearl)
};

[[nodiscard]] const Palette& palette(ThemeId id) noexcept;
[[nodiscard]] ThemeId        nextTheme(ThemeId id) noexcept;
[[nodiscard]] ThemeId        themeFromName(const std::string& name) noexcept;   // unknown -> Pearl

// Deterministic procedural faceplate at exactly width×height (sRGB RGBA8),
// drawn from the same Layout the UI uses so panels line up with the controls.
[[nodiscard]] Image generateFaceplate(ThemeId theme, const Layout& layout, std::uint32_t width, std::uint32_t height);

// Resolves the faceplate for a theme and screen tier, ready for upload:
//   1. <dataDir>/skins/<theme>.png or .jpg (any size, 8K fine) -> baked to the tier
//   2. otherwise the procedural faceplate
// Results are cached in <dataDir>/texcache so a master is decoded once per tier.
// `info` receives a one-line description of where the art came from.
[[nodiscard]] CompressedTexture loadFaceplate(ThemeId theme, Tier tier, const Layout& layout,
                                              const std::filesystem::path& dataDir, std::string* info);

} // namespace hc::ui

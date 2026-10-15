#pragma once
// =============================================================================
//  The chassis: Main-Faceplate.png (3840 × 2160) and everything measured on it.
//
//  * The PNG is the screen. It is drawn as a 3-slice in height: the rows above and
//    below the stretch band are copied 1:1, the band (inside the left/main panels,
//    where the art is vertically uniform) stretches when the window is taller than
//    16:9. Wider-than-16:9 windows are pillarboxed and the outer frame edge columns
//    are stretched outward, so no flat colour ever shows.
//  * Every coordinate below was measured on the PNG (pixels). Layout places widgets
//    on these regions; Parts cuts the knob socket and glass window out of it.
//  * Skins: any PNG/JPG with the same layout and a 16:9 aspect (4K or 8K) works; it is
//    scaled to 3840 × 2160 in RAM first, so the measurements stay valid.
// =============================================================================
#include "hybrid/ui/Geometry.hpp"
#include "hybrid/ui/Image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace hc::ui::plate {

inline constexpr float kWidth  = 3840.0f;
inline constexpr float kHeight = 2160.0f;

// Vertical stretch band (rows 900..1800 only contain the uniform left/main panels).
inline constexpr float kStretchY0 = 900.0f;
inline constexpr float kStretchY1 = 1800.0f;
// Pillarbox fill: the outermost frame columns.
inline constexpr float kEdgeColumn = 24.0f;

inline constexpr Rect   kTopBar{65, 36, 3701, 162};                 // brushed chrome bar
inline constexpr Circle kLogoSocket{159, 127, 51};                  // small socket, inner rim radius
inline constexpr Rect   kNamePanel{113, 255, 1076, 178};            // brushed chrome face inside the dark outline
inline constexpr std::array<Circle, 6> kSockets{{{1399, 347, 81}, {1607, 347, 81}, {1814, 347, 81},
                                                  {2023, 347, 81}, {2232, 347, 81}, {2440, 347, 81}}};
inline constexpr float  kSocketOuterRadius = 97.0f;                 // outer chrome edge of a round socket
inline constexpr Rect   kDisplayBezel{2627, 235, 1126, 218};        // glass display incl. chrome bezel
inline constexpr Rect   kDisplayGlass{2656, 259, 1056, 180};        // dark glass inside the bezel
inline constexpr float  kDisplayBezelRadius = 50.0f;
inline constexpr std::array<Rect, 4> kTabFaces{{{117, 519, 855, 122}, {1034, 519, 853, 122},
                                                 {1949, 519, 855, 122}, {2865, 519, 853, 122}}};
inline constexpr float  kTabFaceRadius = 26.0f;
inline constexpr Rect   kLeftPanel{84, 702, 614, 1233};             // grey face (stretches with the band)
inline constexpr Rect   kMainPanel{740, 702, 3016, 1233};
inline constexpr Rect   kBottomBar{72, 1966, 3695, 141};            // brushed chrome bar

} // namespace hc::ui::plate

namespace hc::ui {

// The design canvas fitted to a window: design units -> window pixels.
struct Canvas {
    float scale   = 1.0f;    // window pixels per design unit
    float offsetX = 0.0f;    // window pixel of design x = 0
    float offsetY = 0.0f;
    float extraH  = 0.0f;    // design units added to the stretch band (window taller than 16:9)
    float pixelW  = plate::kWidth;
    float pixelH  = plate::kHeight;

    [[nodiscard]] static Canvas fit(float windowW, float windowH) noexcept;
    [[nodiscard]] float height() const noexcept { return plate::kHeight + extraH; }
    [[nodiscard]] float visibleLeft() const noexcept { return -offsetX / scale; }               // < 0 when pillarboxed
    [[nodiscard]] float visibleRight() const noexcept { return (pixelW - offsetX) / scale; }
    [[nodiscard]] float toDesignX(float px) const noexcept { return (px - offsetX) / scale; }
    [[nodiscard]] float toDesignY(float py) const noexcept { return (py - offsetY) / scale; }
    [[nodiscard]] float toPixelX(float x) const noexcept { return offsetX + x * scale; }
    [[nodiscard]] float toPixelY(float y) const noexcept { return offsetY + y * scale; }
    // Faceplate row -> canvas row (identity above the band, shifted by extraH below it).
    [[nodiscard]] float plateY(float y) const noexcept;
    // A faceplate rectangle that spans the band grows with it.
    [[nodiscard]] Rect plateRect(Rect r) const noexcept;
    [[nodiscard]] bool operator==(const Canvas& o) const noexcept {
        return scale == o.scale && offsetX == o.offsetX && offsetY == o.offsetY && extraH == o.extraH && pixelW == o.pixelW && pixelH == o.pixelH;
    }
};

// The faceplate master in RAM (straight sRGB, exactly 3840 × 2160).
struct FaceplateArt {
    Image       master;
    std::string origin;    // where it came from, for status lines
};

inline constexpr const char* kFaceplateFile = "Main-Faceplate.png";

// The user's skin (<dataDir>/skins/Main-Faceplate.png or .jpg, same layout, any 16:9 size up
// to 8K) when present, else the bundled <assetDir>/faceplates/Main-Faceplate.png.
[[nodiscard]] std::filesystem::path resolveFaceplate(const std::filesystem::path& assetDir, const std::filesystem::path& dataDir);

// Loads and validates a faceplate (16:9 within 1 %, at least 1920 wide); scales to 3840×2160.
[[nodiscard]] std::optional<FaceplateArt> loadFaceplateArt(const std::filesystem::path& file, std::string* error);

// GPU copy of the chassis for a canvas scale: the smallest of 960/1280/1920/2560/3840 px wide
// that is at least the on-screen width, premultiplied sRGB with a full mip chain.
[[nodiscard]] std::uint32_t chassisTextureWidth(float canvasScale) noexcept;
[[nodiscard]] TextureLevels bakeChassis(const FaceplateArt& art, std::uint32_t textureWidth);

} // namespace hc::ui

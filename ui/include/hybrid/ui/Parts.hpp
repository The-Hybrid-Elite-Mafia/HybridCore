#pragma once
// =============================================================================
//  Parts atlas — every moving or repeated piece of hardware, as textures.
//
//  Two sources, one atlas (premultiplied sRGB, full mip chain):
//   * CUT FROM THE FACEPLATE PNG: the round chrome socket (knob/button well) and the
//     glass display window (9-slice: corners pixel-exact, edges stretch).
//   * RENDERED ON THE CPU AT START-UP (deterministic, no files): knob layers, chrome
//     fader / button caps, glass reflections, LED segments, glows, icons. Each is shaded
//     with ONE light from the top-left (kLight) and a studio environment for chrome.
//
//  Knob = 5 layers (see MixerView): shadow (fixed) -> socket (fixed, from the PNG) ->
//  track + value arc (fixed) -> BODY (rotates) -> INDICATOR (rotates) -> LIGHT (fixed:
//  diffuse shading + specular, so the light stays put while the metal turns under it).
// =============================================================================
#include "hybrid/ui/Image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hc::ui {

enum class Sprite : std::uint8_t {
    Socket,          // round chrome well, cut from the faceplate (transport socket)
    KnobShadow,      // soft contact shadow
    KnobTrack,       // dark ring between the socket rim and the body; the value arc lights it
    KnobBody,        // spun dark cap + knurled chrome skirt (rotates)
    KnobIndicator,   // pointer line (rotates)
    KnobLight,       // diffuse darkening (alpha) + specular (additive) — fixed
    CapDark,         // glossy black transport cap, lit
    RingGlow,        // soft ring of light (additive)
    FaderCap,        // brushed chrome fader cap, lit, with centre channel
    FaderShadow,
    FaderLine,       // centre line (tinted, emissive)
    ButtonCap,       // chrome M/S/Ø cap, lit
    ButtonShadow,
    ButtonLens,      // lit-from-inside fill (tinted)
    ButtonHalo,      // glow around a lit button (additive)
    Glass,           // glass window with chrome bezel, cut from the faceplate (9-slice)
    GlassSheen,      // reflection gradient over glass (additive)
    GlassRim,        // thin bright inner rim (9-slice, additive)
    InnerShadow,     // recess shadow (9-slice)
    DropShadow,      // soft shadow a raised part casts down-right (9-slice)
    LedSegment,      // meter segment (tinted)
    GlowDot,         // radial glow (additive)
    Lamp,            // LED dome (tinted)
    IconPrev,
    IconNext,
    IconSine,
    IconInput,
    IconFolder,
    IconWave,
    IconPlay,
    IconPause,
    IconStop,
    IconLoop,
    Count
};
inline constexpr std::size_t kSpriteCount = static_cast<std::size_t>(Sprite::Count);

struct SpriteInfo {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;   // atlas UVs (level 0)
    float width = 0, height = 0;           // sprite pixels in the atlas
    float slice[4] = {0, 0, 0, 0};         // 9-slice insets in sprite pixels: left, top, right, bottom
};

struct PartsAtlas {
    TextureLevels                           texture;   // premultiplied sRGB + mips
    std::array<SpriteInfo, kSpriteCount>    sprites{};
    [[nodiscard]] const SpriteInfo& operator[](Sprite s) const noexcept { return sprites[static_cast<std::size_t>(s)]; }
};

// `faceplate` is the 3840×2160 master (FaceplateArt::master).
[[nodiscard]] PartsAtlas buildPartsAtlas(const Image& faceplate);

// Knob proportions, as fractions of the SOCKET OUTER RADIUS (the knob widget radius).
namespace knob {
inline constexpr float kRimInner   = 0.835f;  // inside edge of the chrome socket rim
inline constexpr float kTrackInner = 0.64f;
inline constexpr float kBody       = 0.665f;  // knob body radius
inline constexpr float kArcRadius  = 0.745f;  // centre line of the value arc
inline constexpr float kArcWidth   = 0.085f;
inline constexpr float kIndicatorFrom = 0.20f;   // along the body radius
inline constexpr float kIndicatorTo   = 0.62f;
} // namespace knob

// Light direction (towards the light): from the top-left. x right, y down, z out of the screen.
inline constexpr float kLight[3] = {-0.48f, -0.62f, 0.62f};

} // namespace hc::ui

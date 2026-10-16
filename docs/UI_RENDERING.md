# UI rendering — how the console is drawn

The screen is a photograph of hardware: `assets/faceplates/Main-Faceplate.png` (3840 × 2160).
Nothing is a flat colour or a filled rectangle; every pixel comes from the PNG, from pieces cut
out of it, or from textures rendered on the CPU at start-up and lit from one direction.

## Coordinates
- Design units = faceplate pixels. `ui::plate` (Faceplate.hpp) holds every region measured on
  the PNG: top bar, logo socket, name panel, 6 round sockets, glass display, 4 tabs, left and main
  panels, bottom bar. `tests/ui_tests.cpp` checks those measurements against the real file.
- `ui::Canvas` fits the design to the window: uniform scale by width. A window taller than 16:9
  stretches only the band of rows 900–1800 (inside the left/main panels, where the art is
  vertically uniform); widgets in those panels get the extra height. A window wider than 16:9 is
  pillarboxed and the outermost frame columns are stretched outward.
  4K, 1440p and a 1389-wide laptop therefore show the same layout at different sizes.

## Pipeline (OpenGL 3.3 core)
- Every texture is `SRGB8_ALPHA8`, premultiplied alpha (premultiplied in linear light), full mip
  chain built in linear light, trilinear filtering. Render targets are sRGB, so blending happens
  in linear light; one blend mode (`ONE, ONE_MINUS_SRC_ALPHA`) covers both normal and additive
  quads, so they batch into a single draw call.
- MSAA: 8× requested; the GPU's maximum is used if lower (Mesa's software driver in CI: 4×).
- Static layer: chassis, strip glass, sockets, slots, scales and labels are rendered once into a
  texture and re-used; each frame only draws moving parts (knobs, caps, buttons, meters, readouts).
- Bloom: emissive quads (value arcs, lit LEDs, lit buttons, neon text, accent lines) are drawn
  again into a half-resolution light buffer, blurred at two radii and added before the final
  sRGB encode.

## Textures and VRAM (all charged to the 64 MB UI texture budget)
| Texture | Size | Notes |
|---|---|---|
| Chassis | 960 / 1280 / 1920 / 2560 / 3840 wide, chosen from the window width | full 3840 only on a 4K window; 1920 on a 1389-wide laptop |
| Parts atlas | 1024 × 1024 + mips (~5.3 MB) | socket + glass cut from the PNG, the rest rendered at start-up |
| Poppins | RG8, 512–2048 square | R = glyph, G = blurred glyph (shadows, engraving, glow) |
Render targets (scene MSAA, static layer, resolve, bloom, capture) are extra and reported by
`hc_ui_snapshot`; at 3840 × 2160 with 8× MSAA the MSAA buffer alone is about 265 MB.

## Parts (Parts.hpp)
- Knob = shadow (fixed) → socket from the PNG (fixed) → dark track + value arc (fixed) → body
  with spun cap and knurled skirt (rotates) → indicator (rotates) → light layer (fixed: diffuse
  darkening in alpha + specular in colour). The light stays put while the metal turns under it.
- Fader cap: brushed chrome with bevel, grip grooves, centre channel; coloured centre line
  (emissive); soft shadow cast down-right onto the slot. Slot = glass + inner shadow.
- Meters: glass window → inner shadow → LED segments (green/yellow/red on the fader taper,
  peak hold) → reflection gradient → thin bright rim.
- M / S / Ø: chrome caps; when on they move 1.5 px down-right, the shadow shrinks, a coloured
  lens lights from inside and a halo glows.
- One light from the top-left (`ui::kLight`) for every shadow, bevel and highlight.

## Text
Poppins Medium / SemiBold / Bold (SIL OFL 1.1, `assets/fonts`). Engraved on chrome (dark text with
a 1 px light edge underneath), light text with a soft shadow on glass, neon with glow in the
display. No kerning: Poppins stores it in a GPOS layout stb_truetype cannot read.

## Skins
`%APPDATA%\HybridCore\skins\Main-Faceplate.png` (or .jpg) replaces the bundled art. It must keep
the same layout and be 16:9 (4K or 8K); it is scaled to 3840 × 2160 in RAM and then to the window.

## Look target
`docs/reference/steven-hd-target-3460x2160.webp` is Steven's AI-enhanced version of a 0.6.0 screenshot: the target for contrast and
crispness (black glass and caps, white labels, near-black engraving, tight glow rings, bright knob rings). It is a reference only — its
controls and text are frozen pixels (and the enhancer invented some letters), so it is never drawn by the app.
Tuning taken from it (0.6.1): sharper minification of the chassis and parts (0.6× texture footprint; 4K still samples 1:1), less bloom,
darker strip glass, brighter bezel rims and labels, brighter knob skirts, visible unlit knob rings, blacker transport caps, no bloom on
the lit cap rings, darker engraving with a fainter highlight.

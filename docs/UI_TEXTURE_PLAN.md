# GPU UI textures (Phase 4, implemented) — no 8K textures on the graphics card

Rule: **8K art never reaches VRAM.** The 8K files are source masters only. The GPU
gets downscaled, mipmapped, block-compressed copies sized to the screen.

## Why
| Texture | Format | VRAM |
|---|---|---|
| One 8K faceplate (7680×4320) | RGBA8 | 132.7 MB (176.9 MB with mips) |
| One 8K faceplate | BC7 | 33.2 MB (44.2 MB with mips) |
| Faceplate at design size 1676×940 (1x) | DXT5 + mips | **2.1 MB** |
| Faceplate at 3348×1880 (2x, for 4K / high-DPI) | DXT5 + mips | **8.4 MB** |
| Knob as 128-frame filmstrip, 256² each | RGBA8 | 33.6 MB |
| Knob drawn as SDF shapes in the shader | — | **0 bytes** |

(1673 is padded to 1676: DXT5 works in 4×4 blocks, so width/height must be multiples of 4.)

## Pipeline
1. **Masters stay on disk**: drop them in `%APPDATA%\HybridCore\skins\` as `pearl.png`, `obsidian.png`,
   `titanium.png` (PNG or JPG, up to 16384 px per side). They are never uploaded as-is.
2. **Bake on first load, then cached** (`ui/src/Image.cpp`, `loadFaceplate()` in `ui/src/Theme.cpp`):
   - sRGB-correct resize (stb_image_resize2) to the screen tier: **1x = 1676×940**, **2x = 3348×1880**
   - full mip chain
   - **DXT5 / BC3** block compression (stb_dxt, high-quality mode) — 1 byte per pixel, same size as BC7.
     BC7 was the original plan; DXT5 was chosen because a small, permissively-licensed encoder exists
     and every desktop GPU decodes it. Measured round-trip error on a test gradient: about 2/255.
   - result cached in `%APPDATA%\HybridCore\texcache\*.hctx`, so an 8K master is decoded once per tier
   - with no custom art, a procedural faceplate (Pearl White / Obsidian / Titanium) is generated instead
3. **Runtime load (render thread only):**
   - pick the tier from the window's real pixel size: ≤1x → 1x tier, otherwise 2x tier; load only that tier
   - the renderer **refuses any texture larger than 4096 px per side**, so an 8K file can't slip in
   - one upload per theme/tier change; nothing re-uploaded per frame
   - total UI texture budget: **64 MB VRAM**, enforced by `TextureBudget` (CI measured 3.0 MB at 1x, 9.0 MB at 2x)
4. **Neon lighting is shader work, not pixels:** knobs, buttons, faders and meters are drawn as
   signed-distance-field shapes with an additive glow quad (`ui/src/Renderer.cpp`). Changing the glow
   changes vertex data, not textures. No bloom pass or emissive masks were needed.
5. **Knobs/faders/meters:** no textures at all — the C++ side sends a few hundred quads per frame
   with the control's angle/level baked into the vertices.

## Thread rules (unchanged from Phase 2)
- The audio thread never touches the GPU, the loader, or any texture memory.
- Parameter values travel audio ↔ render thread through the lock-free rings/atomics only.

## Backend
OpenGL 3.3 core through our own WGL loader (`ui/src/Gl.cpp`) — chosen over Vulkan because it needs no
SDK, no shader compiler at build time, and can be tested in CI with Mesa's software driver. If the
driver lacks S3TC, textures are decoded and uploaded as RGBA8 (4× the VRAM, still inside the budget).
The exact GPU in the rig is not known yet; the app and `hc_ui_snapshot` print the GL renderer name.

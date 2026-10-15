# GPU UI texture plan (Phase 4) — no 8K textures on the graphics card

Rule: **8K art never reaches VRAM.** The 8K files are source masters only. The GPU
gets downscaled, mipmapped, block-compressed copies sized to the screen.

## Why
| Texture | Format | VRAM |
|---|---|---|
| One 8K faceplate (7680×4320) | RGBA8 | 132.7 MB (176.9 MB with mips) |
| One 8K faceplate | BC7 | 33.2 MB (44.2 MB with mips) |
| Faceplate at design size 1676×940 (1x) | BC7 + mips | **2.1 MB** |
| Faceplate at 3348×1880 (2x, for 4K / high-DPI) | BC7 + mips | **8.4 MB** |
| Neon glow mask at 2x | BC4 (1 channel) + mips | 4.2 MB |
| Knob as 128-frame filmstrip, 256² each | RGBA8 | 33.6 MB |
| Knob as one sprite rotated by the shader, 256² | BC7 | **64 KB** |

(1673 is padded to 1676: BC7 works in 4×4 blocks, so width/height must be multiples of 4.)

## Pipeline
1. **Masters stay on disk** in `assets/masters/` (8K PNG). They are never loaded by the app.
   Large binaries will go through Git LFS or stay out of git — decided when the art arrives.
2. **Build-time bake** (`hc_texbake` step in Phase 4) using Microsoft DirectXTex `texconv`:
   - resize in linear light to two tiers: **1x = 1676×940**, **2x = 3348×1880**
   - full mip chain
   - encode **BC7_UNORM_SRGB** for colour art, **BC4_UNORM** for glow/emissive masks
   - output `.dds` into `assets/baked/` (what ships with the exe)
3. **Runtime load (render thread only):**
   - pick the tier from the window's real pixel size: ≤1x → 1x tier, otherwise 2x tier; load only that tier
   - loader **rejects any texture larger than 4096 px per side** (hard assert) so an 8K file can't slip in
   - one staging-buffer upload at startup; nothing re-uploaded per frame
   - total UI texture budget: **64 MB VRAM**, checked and printed at startup
4. **Neon lighting is shader work, not pixels:** emissive mask × colour × intensity, plus a
   half-resolution bloom pass. Animating the glow changes a uniform, not a texture.
5. **Knobs/faders/meters:** single sprites transformed in the vertex shader. The C++ side sends
   only floats (angle, level) per control each frame.

## Thread rules (unchanged from Phase 2)
- The audio thread never touches the GPU, the loader, or any texture memory.
- Parameter values travel audio ↔ render thread through the lock-free rings/atomics only.

## Backend
Vulkan (Windows only). BC7/BC4 are supported by every DirectX 11-class desktop GPU.
The exact GPU in the rig is not known yet. `hc_device_probe` will report it once Phase 4 adds a GPU section.

# The Hybrid Core

Proprietary real-time audio framework for Hybrid Elite Mafia (HEM).
Built for one machine only: Windows x64, MSVC, Intel Core i7-14700HX, 48 kHz.

Rig-wide constants (sample rate, 13 stems, 4 buses, limits) live in ONE file:
`core/include/hybrid/core/Config.hpp`. The version lives in `VERSION`.

## Status
| Phase | What | State |
|---|---|---|
| 1 | Windows/MSVC build, ASIO + WASAPI device probe with measured timing | Built + run in CI (no sound card there). Not yet run on the rig. |
| 2 | Lock-free audio callback, WASAPI exclusive + ASIO streams, MMCSS, P-core pinning | Built, unit-tested in CI. Not yet run on real audio hardware. |
| 3 | AVX2 DSP kernels + 13-stem × 4-bus routing matrix | Built, tested (0 heap allocations on the audio path). Not yet heard on real hardware. |
| 4 | GPU UI (OpenGL 3.3) on `Main-Faceplate.png`: layered knobs, chrome caps, glass meters, bloom | Rendered with the live engine and pixel-checked against the faceplate in CI (software GPU). Not yet seen on the rig's GPU. |
| 5 | Parameter tree, settings, standalone `HybridCore.exe`, optional VST3 | App starts, draws and saves settings in CI; VST3 passes Steinberg's validator (47/47). Not yet used in a DAW. |
| 6 | Stem loading (WAV/RF64, MP3, FLAC) + sample-rate conversion to 48 kHz, background loader, lock-free stem playback | Built, unit-tested (decoders, resampler quality, loader, zero-allocation stem swaps); loaded through the app code in CI. Not yet heard on the rig. |

Planned next (each waits for a "go"): 7 transport/timeline ·
8 DSP modules (EQ, compression…) · 9 session save/open + WAV export · 10 installer polish + full test suite.

## Modules
| Folder | Library | What it is |
|---|---|---|
| `core/` | `hc::core` | Config, wait-free SPSC ring, aligned buffers, real-time thread policy |
| `io/` | `hc::io` | WASAPI exclusive stream, ASIO duplex host, offline clock stream, device/CPU probe, AVX2 sample conversion |
| `dsp/` | `hc::dsp` | AVX2 kernels, `StemMatrix` (13 stems × 4 stereo buses, ramps, meters) |
| `engine/` | `hc::engine` | Lock-free `Engine`, `AudioProcessor`, `MixerProcessor` (tones / inputs / stems), `StemAudio`, `ToneProcessor` |
| `media/` | `hc::media` | WAV/RF64 decoder + writer, MP3/FLAC (dr_libs), `Resampler` (Kaiser windowed sinc, AVX2), `StemLoader` (background) |
| `params/` | `hc::params` | Parameter tree (atomics + dispatch), `Settings` INI, mixer parameter set + bindings |
| `ui/` | `hc::ui` | Faceplate chassis + canvas, parts atlas, Poppins atlas, layout/interaction, GL renderer (sRGB, MSAA, bloom, static cache), `MixerView`, `Console` — see `docs/UI_RENDERING.md` |
| `assets/` | — | `faceplates/Main-Faceplate.png` (HEM art), Poppins fonts (OFL) |
| `app/` | — | `HybridCore.exe` standalone app; `hc_apphost` (AudioHost, StemDeck) |
| `plugin/` | — | Optional VST3 (`-DHC_BUILD_VST3=ON`; SDK fetched at configure time) |
| `tools/` | — | `hc_device_probe`, `hc_engine_smoke`, `hc_ui_snapshot`, CI scripts, ASIO SDK fetcher |
| `tests/` | — | `hc_tests` (ring, conversion, engine, kernels, matrix, zero-allocation, params, settings, UI, media, stems); encoded test vectors in `tests/data` |

Every public header documents its threading rules (which thread may call what).

## CI (every push to main)
GitHub Windows Server 2022 + Visual Studio 2022:
1. WASAPI-only build (no third-party SDKs) + tests + device probe.
2. Full build: ASIO SDK + VST3 SDK fetched (never committed) → tests → Steinberg VST3 validator →
   UI snapshots at 3840×2160 and 1389×868 with the live engine (Mesa software OpenGL) → real app start + window screenshot →
   installer build → silent install / upgrade / uninstall check → GitHub Release `v<VERSION>`.
Screenshots of each run: branch `ci-screenshots`.

## Third-party code
- stb (public domain / MIT) — vendored in `external/stb`.
- Poppins (SIL Open Font License 1.1) — `assets/fonts`, bundled unmodified with its licence.
- dr_mp3 0.7.3 / dr_flac 0.13.3 (public domain or MIT-0) — vendored in `external/dr_libs`.
- Steinberg ASIO SDK 2.3.4 — GPLv3 or Steinberg proprietary licence; downloaded at build time, never committed.
  Fine for this private build; before giving the app to anyone else, sign Steinberg's ASIO agreement or go GPLv3.
- Steinberg VST3 SDK 3.8.1 — MIT licence (checked in its LICENSE.txt); fetched at configure time.
- Inno Setup builds the installer. Its maker asks businesses earning over USD 5,000/year to buy a licence (not strictly required).

## Build on the rig (only if ever needed — the installer is the normal route)
```
powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
cmake --preset rig-full
cmake --build --preset rig-full-release
```

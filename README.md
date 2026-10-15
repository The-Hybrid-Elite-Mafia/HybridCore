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
| 4 | GPU UI (OpenGL 3.3): faceplates, knobs, faders, neon meters, 3 themes | Rendered and pixel-checked in CI with a software GPU driver. Not yet seen on the rig's GPU. |
| 5 | Parameter tree, settings, standalone `HybridCore.exe`, optional VST3 | App starts, draws and saves settings in CI; VST3 passes Steinberg's validator (47/47). Not yet used in a DAW. |

Planned next (each waits for a "go"): 6 stem/WAV/MP3 loading + sample-rate conversion · 7 transport/timeline ·
8 DSP modules (EQ, compression…) · 9 session save/open + WAV export · 10 installer polish + full test suite.

## Modules
| Folder | Library | What it is |
|---|---|---|
| `core/` | `hc::core` | Config, wait-free SPSC ring, aligned buffers, real-time thread policy |
| `io/` | `hc::io` | WASAPI exclusive stream, ASIO duplex host, device/CPU probe, AVX2 sample conversion |
| `dsp/` | `hc::dsp` | AVX2 kernels, `StemMatrix` (13 stems × 4 stereo buses, ramps, meters) |
| `engine/` | `hc::engine` | Lock-free `Engine`, `AudioProcessor`, `MixerProcessor`, `ToneProcessor` |
| `params/` | `hc::params` | Parameter tree (atomics + dispatch), `Settings` INI, mixer parameter set + bindings |
| `ui/` | `hc::ui` | Image pipeline (8K → tier, mips, DXT5, cache), faceplates, layout/interaction, font, GL renderer, `MixerView` |
| `app/` | — | `HybridCore.exe` standalone app |
| `plugin/` | — | Optional VST3 (`-DHC_BUILD_VST3=ON`; SDK fetched at configure time) |
| `tools/` | — | `hc_device_probe`, `hc_engine_smoke`, `hc_ui_snapshot`, CI scripts, ASIO SDK fetcher |
| `tests/` | — | `hc_tests` (ring, conversion, engine, kernels, matrix, zero-allocation, params, settings, UI) |

Every public header documents its threading rules (which thread may call what).

## CI (every push to main)
GitHub Windows Server 2022 + Visual Studio 2022:
1. WASAPI-only build (no third-party SDKs) + tests + device probe.
2. Full build: ASIO SDK + VST3 SDK fetched (never committed) → tests → Steinberg VST3 validator →
   UI snapshot of every theme with Mesa software OpenGL → real app start + window screenshot →
   installer build → silent install / upgrade / uninstall check → GitHub Release `v<VERSION>`.
Screenshots of each run: branch `ci-screenshots`.

## Third-party code
- stb (public domain / MIT) — vendored in `external/stb`.
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

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
| 7 | Transport (play / pause / stop / seek / loop, click-free), song overview with click-to-seek, keyboard | Built, unit-tested (500 random transport changes, 0 allocations); playing in the CI snapshots. Not yet heard on the rig. |
| 8 | Per-stem EQ (low cut, low/high shelf, sweep mid) + compressor, main look-ahead limiter, MIX / EQ / DYNAMICS pages, VST3 parity | Built, unit-tested (EQ curves, compressor curve and timing, limiter ceiling, 0 allocations, full-chain CPU). Not yet heard on the rig. |
| 9 | Sessions (`.hcs`), offline WAV export in the background, OPEN / SAVE / EXPORT | Built, unit-tested (round trip, moved folders, export = sum of stems, byte-identical renders). Dialogs not yet clicked on the rig. |
| 11 | BEAT FORGE (from Hybrid Studio 2.0.9, rebuilt without JUCE): 8 synthesized tracks on strips 1–8, genre composer, song form, FORGE page | Built, unit-tested (composer, form, sounds, timing, zero allocations, export). Not yet heard on the rig. |
| 10 | `.hcs` file association, single instance (double-click hands the song to the open console), version in the title bar | Installer check in CI (association added and removed). Hand-over not yet tried on the rig. |

The roadmap's 10 phases are built. Next steps depend on what Steven hears and sees on the rig.

## Modules
| Folder | Library | What it is |
|---|---|---|
| `core/` | `hc::core` | Config, wait-free SPSC ring, aligned buffers, real-time thread policy |
| `io/` | `hc::io` | WASAPI exclusive stream, ASIO duplex host, offline clock stream, device/CPU probe, AVX2 sample conversion |
| `dsp/` | `hc::dsp` | AVX2 kernels, `StemMatrix` (13 stems × 4 stereo buses, ramps, meters), `ChannelStrip` (EQ + compressor), `BusLimiter` (look-ahead) |
| `engine/` | `hc::engine` | Lock-free `Engine`, `AudioProcessor`, `MixerProcessor` (tones / inputs / stems), `StemAudio`, `ToneProcessor` |
| `media/` | `hc::media` | WAV/RF64 decoder + writer, MP3/FLAC (dr_libs), `Resampler` (Kaiser windowed sinc, AVX2), `StemLoader` (background), `Session` (.hcs), `MixExporter` (offline render) |
| `forge/` | `hc::forge` | BEAT FORGE: synth voices, `Composer` (genres, chords, bass, melody, song form), `ForgeEngine` (lock-free playback, reverb, delay) |
| `params/` | `hc::params` | Parameter tree (atomics + dispatch), `Settings` INI, mixer parameter set + bindings |
| `ui/` | `hc::ui` | Faceplate chassis + canvas, parts atlas, Poppins atlas, layout/interaction, GL renderer (sRGB, MSAA, bloom, static cache), `MixerView`, `Console` — see `docs/UI_RENDERING.md` |
| `assets/` | — | `faceplates/Main-Faceplate.png` (HEM art), Poppins fonts (OFL) |
| `app/` | — | `HybridCore.exe` standalone app; `hc_apphost` (AudioHost, StemDeck) |
| `plugin/` | — | Optional VST3 (`-DHC_BUILD_VST3=ON`; SDK fetched at configure time) |
| `tools/` | — | `hc_device_probe`, `hc_engine_smoke`, `hc_ui_snapshot`, CI scripts, ASIO SDK fetcher |
| `tests/` | — | `hc_tests` (ring, conversion, engine, kernels, matrix, zero-allocation, params, settings, UI, media, stems, transport, EQ/compressor/limiter, sessions, export); encoded test vectors in `tests/data` |

Every public header documents its threading rules (which thread may call what).

## CI (every push to main)
GitHub Windows Server 2022 + Visual Studio 2022:
1. WASAPI-only build (no third-party SDKs) + tests + device probe.
2. Public build (`rig-public`: WASAPI + VST3 SDK fetched, **no ASIO code**) → tests → Steinberg VST3 validator →
   UI snapshots at 3840×2160 and 1389×868 with the live engine (Mesa software OpenGL) → real app start + window screenshot →
   installer build → silent install / upgrade / uninstall check → GitHub Release `v<VERSION>`.
Screenshots of each run: branch `ci-screenshots`.

## Licence
The Hybrid Core is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License as published by the Free Software Foundation, version 3 of the License, or (at your option) any
later version (`LICENSE`). Copyright (C) 2026 Steven Reliford / Hybrid Elite Mafia. It comes with NO WARRANTY.
GPLv3 is what lets HEM apps built on it ship Steinberg's ASIO SDK under the SDK's GPLv3 option.

## Third-party code
- stb (public domain / MIT) — vendored in `external/stb`.
- Poppins (SIL Open Font License 1.1) — `assets/fonts`, bundled unmodified with its licence.
- dr_mp3 0.7.3 / dr_flac 0.13.3 (public domain or MIT-0) — vendored in `external/dr_libs`.
- Steinberg ASIO SDK 2.3.4 — dual licence, GPLv3 or Steinberg proprietary (its LICENSE.txt); never committed,
  fetched from download.steinberg.net and checked by SHA-512 (`tools/fetch-asio-sdk.ps1`). HEM apps that ship
  ASIO in public installers (HEM-Elite-Rack) use it under its GPLv3 option. This repository's own installer is
  still the WASAPI-only `rig-public` build. ASIO is a trademark of Steinberg Media Technologies GmbH.
- Steinberg VST3 SDK 3.8.1 — MIT licence (checked in its LICENSE.txt); fetched at configure time.
- Inno Setup builds the installer. Its maker asks businesses earning over USD 5,000/year to buy a licence (not strictly required).

## Build on the rig (only if ever needed — the installer is the normal route)
Public (shareable) build, no ASIO:
```
cmake --preset rig-public
cmake --build --preset rig-public-release
```
ASIO build (under the SDK's GPLv3 option):
```
powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1
cmake --preset rig-full
cmake --build --preset rig-full-release
```

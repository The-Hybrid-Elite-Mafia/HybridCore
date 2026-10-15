# The Hybrid Core

Proprietary real-time audio framework for Hybrid Elite Mafia (HEM).
Built for one machine only: Windows x64, MSVC, Intel Core i7-14700HX, 48 kHz.

Rig-wide constants (sample rate, 13 stems, limits) live in ONE file:
`core/include/hybrid/core/Config.hpp`.

## Status
| Phase | What | State |
|---|---|---|
| 1 | Windows/MSVC build, ASIO + WASAPI device probe | Builds + runs in CI. Not yet run on the rig. |
| 2 | Lock-free audio callback, WASAPI exclusive + ASIO streams | Builds in CI, unit tests pass. Not yet run on real audio hardware. |
| 3 | AVX2 13-stem DSP matrix | Not started |
| 4 | GPU UI canvas (see `docs/UI_TEXTURE_PLAN.md`) | Planned |
| 5 | Parameter tree, standalone .exe + .vst3 switch | Not started |

CI: every push builds on GitHub's Windows Server 2022 + Visual Studio 2022 runner,
runs `hc_tests` and `hc_device_probe --no-audio-ok` (the runner has no sound card).

## Build on the rig
1. Install Visual Studio 2022 (Desktop development with C++) and CMake 3.28+.
2. ASIO (optional): `powershell -ExecutionPolicy Bypass -File tools\fetch-asio-sdk.ps1`
3. Build:
   ```
   cmake --preset rig            (or rig-asio after step 2)
   cmake --build --preset rig-release   (or rig-asio-release)
   ```

## Tools
- `hc_device_probe.exe` — CPU check, then opens every 48 kHz-capable output (and ASIO driver),
  plays 1 s of silence, and reports the real buffer size, latency and measured callback timing.
- `hc_engine_smoke.exe` — plays a -18 dBFS 440 Hz tone through the engine.
- `hc_tests.exe` — hardware-free unit tests.

# The Hybrid Core

Proprietary real-time audio framework for Hybrid Elite Mafia (HEM).
Built for one machine only: Windows x64, MSVC, Intel Core i7-14700HX, 48 kHz.

## Status
- Phase 1 — Windows/MSVC build foundation, ASIO + WASAPI device probe: **done (untested on rig)**
- Phase 2 — Lock-free audio callback, WASAPI exclusive + ASIO streams: **done (untested on rig)**
- Phase 3 — AVX2 13-stem DSP matrix: next
- Phase 4 — GPU UI canvas
- Phase 5 — Parameter tree, standalone .exe + .vst3 switch

## Build (on the rig)
1. Install Visual Studio 2022 (Desktop development with C++) and CMake 3.28+.
2. Optional for ASIO: download the ASIO SDK from https://www.steinberg.net/developers/ and extract it into `external/asiosdk/`.
3. From this folder:
   ```
   cmake --preset rig
   cmake --build --preset rig-release
   ```

## Test tools
- `build\rig\tools\device_probe\Release\hc_device_probe.exe` — checks CPU, WASAPI 48 kHz support, ASIO drivers
- `build\rig\tools\engine_smoke\Release\hc_engine_smoke.exe` — plays a -18 dBFS 440 Hz tone through the engine
  - ASIO: `hc_engine_smoke.exe --asio "Driver Name" --frames 64 --seconds 30`

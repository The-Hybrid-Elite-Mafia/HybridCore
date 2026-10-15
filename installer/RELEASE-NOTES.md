## The Hybrid Core — Phases 1–5 test build

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**What's inside**
- **The Hybrid Core** (`HybridCore.exe`, desktop shortcut): GPU-drawn 13-stem console — 13 stem strips (trim, pan, 3 aux sends, mute/solo/phase, fader, meter) and 4 buses (Main + Aux 1–3), Pearl White / Obsidian / Titanium themes, WASAPI exclusive or ASIO, settings saved automatically.
- **VST3 plug-in** (optional, installed to Common Files\VST3): the same 125-parameter 13-stem matrix for use inside a DAW (generic host controls, no custom screen yet).
- Diagnostics: Rig Check (measured buffer/latency per device), Mixer Test (WASAPI / ASIO), Engine Self-Test.

**How to test:** see `HOW-TO-TEST.txt` (Start menu → The Hybrid Core → How to test).

**Verified in CI** (GitHub Windows Server 2022 runner — no sound card, no GPU): builds with 0 compiler warnings; all unit tests pass; Steinberg's VST3 validator passes; the UI renders every theme correctly with a software OpenGL driver; the real app starts, draws and saves settings; the installer installs / upgrades / uninstalls silently and keeps settings.

**Not tested yet:** sound through real speakers, real latency, your graphics card, your ASIO driver, the plug-in inside a real DAW. That is what this build is for.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

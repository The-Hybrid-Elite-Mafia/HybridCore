## The Hybrid Core — Phase 3 test build

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**What's inside**
- Rig Check (`hc_device_probe`): CPU check + real measured buffer size / latency / callback timing per audio device
- Mixer Test (`hc_engine_smoke --mixer`): 13-stem AVX2 routing matrix with 13 test tones, WASAPI or ASIO
- Engine Self-Test (`hc_tests`): hardware-free unit tests

**How to test:** see `HOW-TO-TEST.txt` (opens after install, also in the Start menu).

**Verified in CI (GitHub Windows Server 2022 runner, no sound card):** builds with 0 warnings, all unit tests pass,
installer installs / upgrades / uninstalls silently and keeps settings. Audio playback through real devices is **not** tested yet — that is what this build is for.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

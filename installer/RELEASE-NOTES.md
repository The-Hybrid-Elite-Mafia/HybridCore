## The Hybrid Core — 0.7.0: Phase 7 transport (play / pause / stop / seek / loop) + song overview

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**New**
- **Transport** on the round chrome buttons: LOAD · START · PLAY/PAUSE · STOP · LOOP · SOURCE. Every start, stop and jump fades in or out over a few milliseconds so it never clicks.
- **Song overview** in the bottom bar: the whole song as a waveform with a moving playhead. Click or drag it to jump.
- **Glass display** shows song position and length (m:ss.t / m:ss).
- **Keys:** Space play/pause, Home start, End stop, L loop, ←/→ 5 s (Ctrl: 30 s).
- Device switching moved to the name panel (click = next, right-click = previous). Right-click any S or stem M clears all solos / mutes.
- Loop on/off is remembered.

**Verified in CI** (GitHub Windows Server 2022 runner — no sound card, no GPU): builds, unit tests (including 500 random transport changes on the audio thread with 0 memory allocations), VST3 validator, UI snapshots with real stems playing, installer install / upgrade / uninstall.

**Not tested yet:** your stems on your speakers, your graphics card, your ASIO driver, the plug-in in a DAW.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

## The Hybrid Core — 0.6.1: sharper look (from Steven's HD reference) + Phase 6 stems

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**New**
- **Load your stems:** LOAD button (pick up to 13 files) or drag files / a folder onto the console. WAV (8/16/24/32-bit, 32/64-bit float, RF64), MP3 (gapless) and FLAC.
- **Sample-rate conversion:** anything that isn't 48 kHz (44.1k, 88.2k, 96k, 192k...) is converted on load with a high-quality filter (flat to 20 kHz, aliasing below −100 dB in the tests).
- Loading runs in the background; each strip shows LOADING %, then the stem name, format and length. Your stems are remembered and reloaded next time.
- SOURCE button cycles TONES → INPUTS → STEMS. Stems loop together from the start (play / stop / seek are Phase 7).
- The audio thread swaps stems without locks and without allocating memory (tested).

Also in this build: the photoreal console from 0.5.1.

**Verified in CI** (GitHub Windows Server 2022 runner — no sound card, no GPU): builds, unit tests, VST3 validator, UI snapshots including one with real MP3 + FLAC stems loaded through the app's own code, the app starts and draws, installer install / upgrade / uninstall.

**Not tested yet:** your stems on your speakers, your graphics card, your ASIO driver, the plug-in in a DAW.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

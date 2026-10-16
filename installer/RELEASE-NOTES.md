## The Hybrid Core — 0.9.0: Phase 9 sessions (save / open) + WAV export

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**New**
- **OPEN / SAVE / EXPORT** buttons in the top bar (Ctrl+O / Ctrl+S / Ctrl+Shift+S / Ctrl+E).
- **Sessions (.hcs):** stems, every control, loop and song position in one small text file. Stems are found relative to the session first, so a song folder that is moved or copied to another drive still opens. Missing stems are named in the status line.
- **Export:** the mix to a 48 kHz WAV (24-bit), the exact length of the song, from its first sample, with every setting (EQ, compressors, faders, mutes, limiter). Runs in the background while you keep listening; press EXPORT again to cancel. It reads the stems from disk again, so it needs free RAM about the size of the loaded stems.
- Window title shows the version and the session name.

**Measured in the tests** (CI machine): an export of two stems with default settings matches the plain sum of the stems to within 6e-8 (32-bit float check); two exports of the same session are byte-identical; with the limiter on, a mix pushed 12 dB hot stays at the −0.3 dB ceiling; a moved song folder reopens with all stems.

**Not tested yet:** the Windows open / save dialogs and exporting on your PC (CI has no one to click them), your speakers, graphics card and ASIO driver.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

## The Hybrid Core — 0.8.0: Phase 8 EQ + compressor on every stem, main limiter

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**New**
- **EQ on every stem:** low cut, low shelf, mid (sweepable 150 Hz – 8 kHz), high shelf. Tab **EQ**.
- **Compressor on every stem:** threshold, ratio, attack, release, makeup. Tab **DYNAMICS**. An orange bar on each stem meter shows the gain reduction.
- **Main limiter (LIM):** look-ahead brick-wall limiter on the main output, ceiling −0.3 dB, on by default. Gain reduction shown next to the button. Adds 1 ms of delay to every output (same on all buses, so they stay lined up).
- **FADERS tab** cycles the stem faders between MAIN / AUX 1 / AUX 2 / AUX 3 sends.
- The VST3 plug-in has the same EQ, compressor and limiter, and reports its 1 ms latency to the DAW.
- The main meters now show the signal after the limiter.

**Measured in the tests** (CI machine, not your rig): EQ curves within 0.1 dB of design at the checked points; compressor −20 dB 4:1 on a full-scale tone turns it down 14.5 dB (15 dB on paper — the detector follows the waveform); limiter never let a sample past the ceiling with bursts 15 dB over it; all 13 stems with every band and the compressor plus the limiter cost about 4 % of one CPU core at 480 frames; 0 memory allocations on the audio thread.

**Not tested yet:** how it sounds on your speakers, your graphics card, your ASIO driver, the plug-in in a DAW.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

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

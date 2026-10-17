## The Hybrid Core — 0.12.4: RESET button

New **RESET** button in the top bar (after SCOPE):
1. Press it once: it turns red and asks **SURE?**
2. Press it again within 4 seconds: every knob and fader goes back to its start position — faders, trims, pans, EQ, compressors, sends, mutes / solos, bus levels, and Beat Forge's sound knobs and effects.
3. Changed your mind? For 15 seconds the button says **UNDO**: press it and everything comes back the way you had it.

RESET never touches your loaded stems or files, or the beat itself (Beat Forge on / off, tempo, genre, key, song form, the sounds it picked, locks).

When the app opens with controls still set from last time, the status line now says so ("Welcome back: 12 controls are where you left them · RESET puts them back").

Also in this version: every ANALYZER check always fits on screen (0.12.3).

---

## The Hybrid Core — 0.12.3: every ANALYZER check fits

On a shorter window a long check (e.g. the peak warning, two lines) pushed the last check (stereo) out of the CHECKS box. Now the checks switch to smaller text when they would not fit, so all of them always show. Everything else is the same as 0.12.2.

Tested on Steven's PC with 0.12.2: a new trap beat now reads stereo +0.94 (was +0.99), and "Almost mono" is gone.

---

## The Hybrid Core — 0.12.2: a wider Beat Forge

The ANALYZER showed Beat Forge beats were almost mono (stereo +0.99: left and right nearly the same). Now hats, keys, pad, lead, percussion (and a touch of snare) spread out to the sides; kick and 808 stay dead centre.

The width is made so it can't hurt in mono: the extra side sound cancels out when left and right are added together, so on a phone speaker or in a club the beat is exactly as before.

Measured on a trap beat (1 = mono): kick 1.000, 808 1.000, hats 0.54, keys 0.53, pad 0.26, whole beat 0.96 (was 0.985); the whole beat in mono loses only 0.1 dB. Everything else is the same as 0.12.1.

---

## The Hybrid Core — 0.12.1: the ANALYZER (3D)

0.12.1 = 0.12.0 with cleaner labels in the ANALYZER (no "?" marks, BASS / HIGHS labels moved clear of the numbers).

New **SCOPE** button in the top bar (next to EXPORT). It covers the stem strips with the ANALYZER; press it again to get the strips back.

- **3D spectrum landscape**, rebuilt from Hybrid Studio's waterfall and drawn on the graphics card: the newest sound in front, about 2.7 seconds going back; bass on the left, highs on the right; height and colour show how loud each part is. Drag it to turn it, double-click to put it back.
- **Loudness** (LUFS, the number streaming services use): last 3 seconds, and the whole play so far, with "finished songs: about -14" marked.
- **Peak** with hold, highest peak and clipped samples.
- **Tone** (bass / mids / highs) and **stereo** (how wide, and whether it holds up on one speaker).
- **Checks in plain words**, e.g. "Song level: -14.2 LUFS", "Peaks safe", "Clipping: 3 samples (last at 1:32)".
- **OPEN** now also opens WAV / MP3 / FLAC: the file goes on strip 1 and the ANALYZER comes up; press PLAY. You can also drag a file onto the ANALYZER.
- It measures what comes out of MAIN, so it works for Beat Forge, your stems and opened files alike. Measuring stops while it is hidden.

Measured in the tests: an EBU reference tone reads -23.0 LUFS (as the standard requires); the analyzer and the export meter agree within 0.15 LU; a full-scale sine reads 0 dB at its frequency in the 3D view; the audio-thread feed never allocates and never loses order.

Not tested yet on your PC: how smooth the 3D view runs on your graphics card.

---

## The Hybrid Core — 0.11.6: OPEN plays audio files

OPEN (and Ctrl+O) now shows WAV, MP3 and FLAC files as well as sessions. Open an audio file (an export, a Suno song) and it goes on strip 1 so you can play it through the console; BEAT FORGE switches off for it (your beat is kept — press ON on the BEAT FORGE page to bring it back). Sessions open as before. Everything else is the same as 0.11.5.

---

## The Hybrid Core — 0.11.5: choose mastered or clean on every export

In the EXPORT window, **"Save as type"** now has two choices:
- **WAV, mastered to song loudness (-14 LUFS)** — the default; ready to play next to finished songs.
- **WAV, clean, no mastering (for mastering later)** — the mix exactly as it plays, with all its headroom, for mastering in the Playground or uploading to Suno.

The app remembers the one you picked last. Everything else is the same as 0.11.4.

---

## The Hybrid Core — 0.11.4: exports at song loudness, hooks that hit harder

- **EXPORT now makes the WAV as loud as a finished song** (-14 LUFS, the level Spotify / YouTube play at), with a limiter keeping peaks at -1 dB so nothing distorts or clips. A mix that is already that loud or louder is left exactly as it is. The message after export shows the loudness and how much it was raised. Playback in the app is not changed. (To turn it off: settings.ini, section [export], master=0.)
- **Hooks hit harder:** in the verses, kick / snare / 808 play a little softer, hats softer still, and the percussion waits for the hook (trap, pop trap, drill, boom bap). Measured on 6 trap beats: hooks are about 2 dB louder than verses (was 0.2 dB).

Measured on the beat exported on your PC in 0.11.3: -17.8 LUFS before; the new export step brings that same mix to -14.3 LUFS, peak -1.0 dB (limiter working up to about 6 dB on the loudest hits).

---

## The Hybrid Core — 0.11.3: selected page tab, second try

0.11.2's tab fix didn't work: the tab's glow also filled its inside, so it still showed bright blue on your PC. In 0.11.3 the selected tab is a dark face with a coloured rim and coloured letters (blue for MIX / EQ / DYNAMICS, orange for BEAT FORGE). Everything else is the same as 0.11.2.

---

## The Hybrid Core — 0.11.2: fixes from the first test on Steven's PC

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX). Fixes what showed up when 0.11.1 ran on the real PC (Realtek speakers, WASAPI exclusive, 144 frames = 3.0 ms, DSP load 2.5-3.8 %, 0 dropouts).

- **"Default output" now works:** when Windows' default device cannot run 48 kHz exclusive, the app uses the first output that can and shows "Default → <device>". Error messages say what is wrong in plain words.
- **Selected page tab is readable on every graphics card:** dark glass with a coloured rim and coloured letters (it washed out to near-white on the PC).
- **Beat Forge is 6 dB quieter going into the console:** a trap beat measured +4.7 to +5.6 dBFS before the console, so the Main limiter was always working. Now it peaks around -1 dBFS and the limiter is only a safety net. Turn Main up if you want it louder.
- **Verses are thinner, hooks lift:** verses drop LEAD and PAD (except cinematic); the hook brings them in.
- Re-roll / CLEAR messages say BACK (the button's name), not UNDO.
- **EXPORT names a beat after itself** ("Beat Forge - TRAP 140 BPM.wav") and saves to Music\The Hybrid Core, then remembers the last folder you used.

**Checked, not a bug:** the sharp sample-to-sample jumps in the PC export are the snare's noise, the kick's click layer and the hats (all by design); no glitch was found when each track was rendered on its own.

**No ASIO devices on this PC:** the build has ASIO, but no ASIO driver is installed (Realtek doesn't ship one). An audio interface's driver will show up in the DEVICE list.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

## The Hybrid Core — 0.11.1: BEAT FORGE (your instrumental maker) rebuilt inside The Hybrid Core

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX). 0.11.1 = 0.11.0 with the OPEN / SAVE / EXPORT labels back to full size.

Beat Forge comes from Hybrid Studio 2.0.9 (that code is untouched in its own repo). It was rebuilt here without JUCE and improved:

**New / better than in Hybrid Studio**
- **Each Beat Forge track is a full console strip:** KICK, SNARE, HATS, PERC, BASS, KEYS, LEAD, PAD play through strips 1-8 with their own EQ, compressor, fader, pan, mute / solo and meters (in Hybrid Studio they were mixed down inside Beat Forge).
- **Whole songs, not just loops:** FULL builds a song form for the genre (intro, verses, hooks, a drop before each hook, outro). The bottom bar shows the sections.
- **Plays with your vocals:** stems on strips 9-13 run on the same clock; one EXPORT renders everything, including the reverb / delay tails (no PRINT TO LANES step needed).
- Reverb / delay sends now follow each strip's fader, mute and solo.
- The beat is saved in sessions (.hcs) and restored when the app opens.

**Same as Hybrid Studio:** 8 genres (trap, pop trap, drill, boom bap, outlaw country, folk, gospel, cinematic), key / scale / bars, density / variation / humanize / swing, LOCK, re-roll one track, 4 sounds each for bass / keys / lead / pad, all synthesized (nothing to license).

**Not in this build yet:** the note grid / piano roll for editing single notes, ASK CLAUDE, and humming / beatboxing into the mic. They come next if you want them.

**Measured in the tests** (CI machine): 192 genre / scale / length combinations composed and arranged with every note in range and in key; all 48 sounds render; notes land on the exact sample with swing; 0 memory allocations on the audio thread; a beat exported twice is byte-identical.

**Not tested yet:** how it sounds on your speakers, your graphics card, your ASIO driver.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

## The Hybrid Core — 0.10.0: Phase 10 — all ten roadmap phases built (1.0 after it is proven on the rig)

Private test build for Steven's rig (Windows 10/11 x64, Intel i7-14700HX).

**New in 0.10.0**
- Double-click a **.hcs** session in Explorer to open it. If the console is already open, the song opens in that window (only one console runs at a time, so two never fight over the sound card).
- Version in the title bar.

**Everything since 0.6** (one place): transport + song overview (0.7) · EQ, compressor on every stem + main limiter (0.8) · sessions + WAV export (0.9).

**Verified in CI** (GitHub Windows Server 2022 — no sound card, software GPU): build with 0 compiler warnings, all unit tests, VST3 validator, UI snapshots with real stems playing, the real app starting, installer install / upgrade / uninstall including the .hcs association.

**Not tested yet:** everything that needs your hardware or your hands — sound on your speakers and ASIO driver, your graphics card, the open / save / export dialogs, double-clicking a session, the plug-in in a DAW.

Not code-signed: Windows SmartScreen will warn. Click "More info" → "Run anyway".

---

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

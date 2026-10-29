# HEM Digital Console

Your engineer's rig as one app, built on The Hybrid Core (no JUCE): the 13-channel desk, the stepped mastering EQ,
the twin bus compressor with dynamic EQ, the colour stage, the loudness meter, and three front-end tools
(RESO TAMER, SHUFFLE, TAP ECHO) in the slots where the desk has its mic preamps.

Install: download `HEMDigitalConsole-Setup-<version>.exe` below and run it. It installs next to The Hybrid Core
(its own program, its own desktop icon); your settings and last session are kept across updates.

Sound: ASIO (any ASIO driver on your PC) or Windows WASAPI as the fallback. ASIO uses Steinberg's ASIO SDK
under its GPLv3 option; the HEM Digital Console is free software under the GPLv3 (LICENSE.txt), and the full
source code of this build is attached below (HEMDigitalConsole-<version>-source.zip).

New in 0.4.0 (screen fit, OPEN, SAVE, stems):
- The whole console always fits the window: no scrolling, every button on screen, at any window size and Windows
  scale (Per-Monitor DPI aware). ZOOM is gone (it pushed SAVE off the screen); BIG DESK is the way to see it big.
  The window opens where you left it (size, place, maximized).
- OPEN opens sessions or audio files ("Sessions + Audio", "Sessions", "Audio (WAV, MP3, FLAC)"), starting in
  Downloads or the last folder. RECENT lists the last 10 sessions. SAVE AS and Ctrl+Shift+S are new.
- Stems go to fixed channels by their Suno names (Vocals 1, Backing Vocals 2, Drums 3, Percussion 4, Bass 5,
  Guitar 6, Keyboard 7, Synth 8, Strings 9, Brass 10, Woodwinds 11, FX 12), whatever the spelling ("0 Lead Vocals",
  "9 Backing_Vocals", "Song (Vocals)"); others in name order. The whole-song file Suno adds (or a master) goes to
  EXT 1 as a reference, not into the mix. More than 13: the console tells you which files were left out.
  Dropping one file on a channel strip loads only that channel.
- Saving is safe: the file is written beside the old one, flushed, then swapped in. A recovery copy is kept every
  3 minutes and offered if the console ever closes by surprise. "Save changes?" before NEW, OPEN and closing.
  Moved stems: the console asks where they are and finds them by name.
- Tested in 0.4.0: everything fits with every control on screen and not covered at 1366x768 and 1920x1080 (100 %,
  125 % where offered, 150 %), Suno stems on the right channels, a crash in the middle of a save leaves the last save
  untouched and recovery is offered, window size and place remembered (automatic UI test on a Windows machine).
  NOT yet passed by the automatic test: SAVE / SAVE AS / OPEN typed into the Windows file dialogs and the moved-stems
  folder picker - the test robot itself is still being fixed, so treat those as untested in this build.

In 0.3.0: the METER page has the reference meter's own controls from its manual: RUN / PAUSE, RESET, the
RADAR / RTA / SCOPE views, AUTO, PEAK and LOUD lamps, the whole EDIT menu (radar, RTA, scope, general: layouts,
units, downmix, alerts, peak-meter scales), a preset LIBRARY (factory + 8 of your own) and FOLLOW PLAY / RESET ON PLAY.

In 0.2.0: ZOOM (removed in 0.4.0), the BIG DESK page (5 channels at a time, bigger
knobs, move across all 13), F11 full screen, text never drawn smaller than 13 screen pixels (except words that
cannot fit a control at all on a small window - the BIG DESK page or ZOOM shows them big), ASIO, and the STEP EQ
SPECIAL EDITION model (MODEL on the STEP EQ page; it is the default): 8 frequencies per band (20 Hz LO, 40 kHz HI),
21 boost / cut steps on every band (0.5 dB steps near zero), a 40-600 Hz high-pass filter per channel and M/S.
The standard model is still there with its own settings.

Every control on every faceplate is wired to the DSP and checked by automated tests on a Windows machine (numbers in
the test log). The sound follows the makers' manuals and published graphs ("in the spirit of" the real gear); it has
not yet been compared by ear or by recording against the real units. Guesses are marked ASSUMED in one file
(console/common/include/hybrid/console/ConsoleSpecs.hpp).

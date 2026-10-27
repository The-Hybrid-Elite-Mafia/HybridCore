# HEM Digital Console

Your engineer's rig as one app, built on The Hybrid Core (no JUCE): the 13-channel desk, the stepped mastering EQ,
the twin bus compressor with dynamic EQ, the colour stage, the loudness meter, and three front-end tools
(RESO TAMER, SHUFFLE, TAP ECHO) in the slots where the desk has its mic preamps.

Install: download `HEMDigitalConsole-Setup-<version>.exe` below and run it. It installs next to The Hybrid Core
(its own program, its own desktop icon); your settings and last session are kept across updates.

Sound: ASIO (any ASIO driver on your PC) or Windows WASAPI as the fallback. ASIO uses Steinberg's ASIO SDK
under its GPLv3 option; the HEM Digital Console is free software under the GPLv3 (LICENSE.txt), and the full
source code of this build is attached below (HEMDigitalConsole-<version>-source.zip).

New in 0.2.0: ZOOM (top bar, or CTRL + mouse wheel; remembered), the BIG DESK page (5 channels at a time, bigger
knobs, move across all 13), F11 full screen, text never drawn smaller than 13 screen pixels (except words that
cannot fit a control at all on a small window - the BIG DESK page or ZOOM shows them big), ASIO.

Every control on every faceplate is wired to the DSP and checked by automated tests on a Windows machine (numbers in
the test log). The sound follows the makers' manuals and published graphs ("in the spirit of" the real gear); it has
not yet been compared by ear or by recording against the real units. Guesses are marked ASSUMED in one file
(console/common/include/hybrid/console/ConsoleSpecs.hpp).

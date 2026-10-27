# HEM Digital Console

Your engineer's rig as one app, built on The Hybrid Core (no JUCE): the 13-channel desk, the stepped mastering EQ,
the twin bus compressor with dynamic EQ, the colour stage, the loudness meter, and three front-end tools
(RESO TAMER, SHUFFLE, TAP ECHO) in the slots where the desk has its mic preamps.

Install: download `HEMDigitalConsole-Setup-<version>.exe` below and run it. It installs next to The Hybrid Core
(its own program, its own desktop icon); your settings and last session are kept across updates.

Public build: WASAPI only (no ASIO code).

Every control on every faceplate is wired to the DSP and checked by automated tests on a Windows machine (numbers in
the test log). The sound follows the makers' manuals and published graphs ("in the spirit of" the real gear); it has
not yet been compared by ear or by recording against the real units. Guesses are marked ASSUMED in one file
(console/common/include/hybrid/console/ConsoleSpecs.hpp).

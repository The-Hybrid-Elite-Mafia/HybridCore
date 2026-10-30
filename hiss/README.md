# HEM Hiss Tamer + Hiss Auto (Phase 1)

Two pieces, pure C++, no JUCE, nothing from outside The Hybrid Core:

* `hem_hiss_tamer_engine` (`HissTamerEngine`): the cutter. Watches one band (default 11.25 to 12.25 kHz), learns what is
  *always there*, and turns only that down. Cymbal hits and other sudden sounds are protected by a hit guard.
  Controls: Sensitivity, Reduction (capped 18 dB), Focus, Width, Mix, Bypass (level-matched, click-free),
  Listen to Removed, Mid/Side, Method, Steady-time, Smoothing. Every one is wired to real maths.
* `hem_hiss_auto` (`autoFix`, `autoFixStems`): the brain. Load a file (or 13 stems) and it scans, finds the problem,
  picks the settings, fixes, checks its own work, and gives the file back **untouched, bit for bit** when it is not sure.

All working guesses live in `include/hybrid/hiss/HissConfig.hpp` (tagged `// ASSUMED`, with an `UNKNOWN` list at the bottom).

Try it (done by CI, not by you): `hc_hiss_cli auto in.wav outdir`, `hc_hiss_cli stems folder outdir`, `hc_hiss_cli demo outdir`.

# HEM Hiss Meters: wiring table (Step 5 of the HEM Real Build rules)

The meters have no knobs on a screen yet. Their "controls" are the numbers in `HissConfig.hpp` (section METERS). Their "meters" are the values the engine returns. Both are listed. There is no screen, so there is nothing drawn that could be fake.

Status words: WORKING = run on known-answer signals and real files, result shown. UNTESTED = exists, no test moves it.

## Readings (what the engine returns)

| Reading | Function (`HissMeters.cpp`) | Test that proves it | Real result | Status |
|---|---|---|---|---|
| Loudness LUFS | `integratedLufs` | 997 Hz sine, peak −20 dBFS, expect −23.01. libebur128 on 14 real files | −23.0103. libebur128 gap 0.0000 (check 7) | WORKING |
| True peak | `truePeakDbfs` | fs/4 sine, samples at 0.707, expect 0 dBFS. libebur128 on 14 files | 0.001. libebur128 gap at most 0.008 dB | WORKING |
| RMS | `rmsDbfs` | −20 dBFS-peak sine, expect −23.01 | −23.010 | WORKING |
| Band RMS | `bandRmsDbfs` | same sine in a 0.5–2 kHz band. ffmpeg on 14 files | −23.010. ffmpeg within 0.109 dB (check 6) | WORKING |
| Band levels A, B, C | `ratioMetrics` | cut 5–10 kHz by 10 dB, add 6 dB air | B moved 0.000 and 6.000 dB. A, B, C match Python to 0.0000 on 14 files | WORKING |
| Ratio 16–20 over 5–10 | `ratioMetrics` | same cut, expect +10 dB | +10.0000 | WORKING |
| Bandwidth audit | `cutoffAudit` | brick wall at 16 kHz; full band | found at 16 025 Hz; full band not called a wall | WORKING |
| 25 Hz comb line level | `combLines` | −100 dBFS per line injected into noise | read −100.02; 100 % lines beat neighbours; 48.5 % with no comb | WORKING |
| Flutter depth (v5, self-calibrating) | `flutterDepth` | 0.30 on steady noise; none; 19.5 s at 44.1 kHz (the half-bin case); 10 s clip; empty band. Real: 9 never-used human clips x 3 depths | 0.328; 0.004; 0.319; refused; refused. Real: 27 of 27 ok, mean 0.300 for 0.30 | WORKING. Weak: strong flutter on a clip that already wobbles at 0.2; below 0.15 UNKNOWN |
| Loud-frame mask | `loudMask` | used inside the ratio test | frames identical in cut and base | PARTLY: flips on peaky clips if air is added; fixed-mask option exists |

## Dials (numbers in `HissConfig.hpp`, section METERS)

All are ASSUMED or POST-HOC. **None has a test that moves it and checks the reading changes by the right amount.** So each is UNTESTED as a dial, even though each is used in the code and the readings above depend on it.

`kMeterFft, kMeterHop, kBandMidLoHz, kBandMidHiHz, kBandAirLoHz, kBandAirHiHz, kBandWideLoHz, kBandMatchedTopHz, kLoudBandLoHz, kLoudBandHiHz, kLoudEnterPct, kLoudLeavePct, kSilenceDb, kMinLoudFrames, kCutoffFft, kCutoffSmoothHz, kWallDropDb, kWallSpanHz, kWallStartDropDb, kGentleDropDb, kCombFundamentalHz, kCombLoHz, kCombHiHz, kFlutterBandLoHz, kFlutterBandHiHz, kFlutterEnvLowpassHz, kFlutterMinSeconds, kFlutterMinBandDb, kFlutterPresentThreshold`

## Fake hunt (Step 6), on `HissMeters.hpp`, `HissMeters.cpp`, the `meters` command, the meter tests

- Searched for: demo, sample value, example, placeholder, mock, fake, todo, stub. **Found 1 hit: the word "fake" in a code comment ("so the ends do not fake a fall").** It is a comment, not fake data.
- Numbers typed straight into the code: **about 25 working guesses were typed in. Moved to `HissConfig.hpp` as ASSUMED. Fixed.** The BS.1770 loudness constants stay in the code on purpose: they are the published values and are checked against libebur128.
- Meters that never change, curves made by formula, buttons with nothing attached: none (there are no screens or buttons).

## Not done

- Smear meter and moving-notch meter exist only in Python. Not in C++ yet.
- No screen exists for these meters. Phase 2 of the console (GPU faceplate) is a separate gate.
- Bypass check does not apply: the meters only read audio (`const float*`) and never change it.

# HEM Hiss tools: the five tools (Lab report, 2026-10-30)

Branch: `hiss-tamer-2` (made from `hiss-tamer-1`). Nothing is on main.
Run on two machines: a Linux build machine with GCC 13, and GitHub's Windows runner with MSVC (Visual Studio 2022, Release). **Windows result: run 41027592840, commit af4d1ce, all steps green**: build with no failure, old tests, lab tests 55 of 55, tool tests 40 of 40, the command-line smoke run (scanner, combclick, airlab, inject, hash, pack, and the Tamer refusing with no proof file), Python check. The Windows numbers matched the Linux numbers in the lab test log (same readings to the printed digits, except tails read 88 / 90 / 250 / 264 ms against 87 / 92 / 244 / 262 ms on Linux, because the two compilers round the hit times differently; both are inside the +/-15 % range).

## Top line
- **Engines: BUILT and TESTED on made-up signals.** 8, 9, 11, 12 (ASSUMED model), 13, 14, 15, 16, 17, 18, 19, 20 are new, pure C++, no JUCE. The older engines 1 to 7, 10 were re-run: 132 of 132 checks pass.
- **The four measuring tools: BUILT as command-line tools with reports. No screens.** The faceplate screens are Phase 2 of the console. They are NOT built, and no screen was faked.
- **Hiss Tamer: the old fixer plus a proof gate that refuses.** The gate is tested. The fixer's own results stay as they were in Phase 1.
- **Nothing here was run on a real Suno file with a known cause.** Real files: UNKNOWN. Every "TESTED" means a made-up signal where the right answer was known, or (for the old meters) 14 real files checked against ffmpeg / libebur128.

## Test results (this run, Linux GCC 13, -O2)
| Test program | Checks | Failed |
|---|---|---|
| hc_hiss_tests (old engines, unchanged) | 132 | 0 |
| hc_hiss_lab_tests (new engines 8-9, 11-17, 19) | 55 | 0 |
| hc_hiss_tools_tests (engines 18, 20, gate, four tools) | 40 | 0 |

Every failure found along the way is in FAILURES.md (F22 to F31). No limit was loosened to make a test pass. Three tests were wrong, not the engine, and were corrected: a click count (25, not 24), a band-pass test tone, and one text check.

## The five tools and their engines
| Tool | Engines | State |
|---|---|---|
| Hiss Scanner | 2 loudness / peaks, 4 bandwidth audit, 5 band levels + 16-20 ratio, 6 loud-part detector | WORKING (command line). Engines were TESTED in Phase 1C against ffmpeg and libebur128 on 14 real files. |
| Comb and Click Finder | 7 comb (25 Hz), 8 seam clicks (40 ms) | WORKING (command line). Click finder TESTED on made-up clicks only. |
| Air and Smear Lab | 9 notch, 10 flutter, 11 smear, 12 masking, 13 hit average, 14 echo, 15 wobble, 16 phase | WORKING (command line) on made-up signals. Masking is ASSUMED. |
| Proof Bench | 17 signal maker, 18 blind plan + pack, 19 fingerprint + name strip, 20 report maker | WORKING (command line). |
| Hiss Tamer | 1 (existing) + gate | Gate WORKING. The fixer is untouched by this work. It refuses until a proof file with `cause_proven: yes`, `blind_result:` and `evidence:` exists. |

## Wiring table (the numbers that act as dials; there is no screen yet)
Dials are numbers in `HissConfig.hpp` section LAB and the command-line numbers. **None has a test that moves the dial and checks the reading changes by the right amount (same open item as F18).** So each dial is UNTESTED as a dial, even though the readings that use it are tested.

| Engine / reading | Function | Test that proves the reading | Real result (made-up signal) | Status |
|---|---|---|---|---|
| 8 Click places | `seamClicks` | 250 clicks every 40 ms in noise | 241 of 250 found within 0.25 ms, 0 invented; gap 40.000 ms; clean noise 0 clicks | WORKING |
| 8 Pattern call | `seamClicks.pattern` | 33 ms train is not "40 ms" | share 0.0, not called | WORKING |
| 9 Notch frequency | `trackNotch` | 15 dB dip sliding 11 to 15 kHz | mean error 12 Hz, worst 40 Hz; drift 284.4 vs 285.7 Hz/s | WORKING |
| 9 Notch depth | `trackNotch` | same | 11.9 dB for a true 15 | PARTLY: reads about 3 dB under; range +/-4 dB printed |
| 11 Tail time | `hitsAndTails` | tau 40 ms and 120 ms | 87.0 vs 92.1; 244 vs 276.3 ms | WORKING with a 5 to 12 % short bias |
| 13 Averaged tail | `averageHits` | same | 92.0 vs 92.1; 262 vs 276.3 ms | WORKING |
| 14 Echo delay | `echoCheck` | 80 ms at -10 dB; 200 ms at -6 dB | 80.0 and 200.0 ms | WORKING |
| 14 Echo strength | `echoCheck` | same | 0.249 for 0.316; 0.500 for 0.501 | PARTLY (first case 21 % low) |
| 15 Wobble rate | `highBandWobble` | 7 Hz depth 0.20 | 6.998 Hz | WORKING |
| 15 Wobble depth | `highBandWobble` | 0.20 and 0.05 | 0.207 and 0.050. Under a beat: 0.117 for 0.20 | WORKING alone; UNKNOWN under a beat (marked NOT trusted) |
| 16 Phase / agreement | `phaseConsistency` | same / delayed 4 samples / two noises | 1.000 and 0.00 deg; 35.8 deg for 36, agreement 0.993; noises 0.028 | WORKING |
| 12 Masking margin | `maskingMarginDb`, `maskingShare` | hand-worked numbers | -15.5, +34.5, +9.5 dB exact; loud noise covers a -120 artifact 100 %, a +10 artifact 0 % | WORKING as maths. The model is ASSUMED. NOT a hearing test |
| 17 Maker | `maker::*` | checked by OTHER meters | comb -80 reads -79.99; flutter 0.30 reads 0.321; click train reads -82.66 against -82.66 from maths | WORKING |
| 18 Blind plan | `blindPlan`, `makePack` | 400 pairs; files | 54.5 % A as X; commitment = SHA-256 of key; X files bit-equal to source; no source name in any file | WORKING |
| 19 Fingerprint, strip | `sha256Hex`, `stripName` | 3 published SHA-256 vectors; file with a title chunk | all match; chunk and title gone, audio bit for bit | WORKING |
| 20 Report | `makeReport` | text checks | ranges + status on every line; bad news first; deterministic | WORKING |
| Tamer gate | `tamerGate` + CLI | 6 proof files; CLI with no proof | refused 5 of 5 bad files; CLI exits 3 and writes nothing | WORKING |
| Bypass | all tools | buffers compared before and after | 0 samples differ (the tools only read) | WORKING |

## Fake hunt (Step 6)
Searched all new files for: demo, sample, example, placeholder, mock, fake, TODO, stub. Hits: the word "sample" meaning an audio sample, and none else. Numbers typed in: all working guesses are in `HissConfig.hpp` section LAB, tagged ASSUMED. Screens: none exist, so nothing painted. Dead controls: the command-line numbers all reach the engine (the 5 inject kinds are run by the tests through the same functions).

## ASSUMED
All numbers in `HissConfig.hpp` section LAB (click size rule 8x, 40 ms tolerance 0.5 ms, notch 8 dB rule, hit rule 10 dB rise, tail 20 dB, echo 0.12 / 4x, wobble 9 dB rule and 10 % harmonic rule, phase 0.30 agreement, masking offsets 14.5 dB and slopes 25 / 10 dB per Bark). The report ranges are the limits used in the tests.

## UNKNOWN
- Everything on real Suno files (cause, size, whether any of these meters sees it). The tools can only say what they measured.
- Whether music with sharp high-frequency drums looks like clicks to the click finder.
- Wobble depth under a strong beat.
- CPU use on the i7-14700HX (the CI runner is a different machine).
- Whether the five-tool faceplate layouts (control counts) are right. They were a proposal. No screen was built.
- Masking: whether the assumed model matches ears.

## Not done
- Faceplate screens for all five tools (Phase 2 gate; needs your GO and a real engine behind every control).
- A real-file run (needs a real human reference set; Phase 2 of the hiss plan is blocked on that, see PHASE2-STATUS.md).
- Engines 18 and 19 existed in Python; the C++ versions are new and are the ones the tools use.

## How to run (done by CI, not by you)
`hc_hiss_tools scanner|combclick|airlab <in.wav> <out.md>`; `hc_hiss_tools proof inject|pack|hash|strip ...`; `hc_hiss_tools tamer <in.wav> <outdir> <proof.txt>`.

## Don't trust yet
- The other agent's earlier report that these engines were TESTED: this run re-ran the C++ tests (132 of 132) on Linux. The Python twins were not re-run here.
- Anything here about how the sound will change. Nothing was listened to.

# HEM Hiss tools: the five tools (Lab report, 2026-10-30, round 2: NO GUESSED LIMITS)

**Windows result for round 2: run 41094654347, commit 70192d0, all steps green** (MSVC Release: lab tests 68 of 68, tool tests 41 of 41, command-line smoke run, Python check).

## Round 2 (this branch, after Steven said "we don't guess")
The lab used to carry working-guess limits that decided yes or no (click 8x, notch 8 dB, hit 10 dB / 15 dB, echo 0.12 and 4x, wobble 9 dB and 10 % harmonic share, phase 0.30). **All of those are gone.** Each decision is now worked out from the file being checked:
- The ONE chosen number is `kAlpha = 0.01`: a clean file may be wrongly accused at most 1 time in 100 per decision. Somebody has to pick it; it is not a property of audio.
- Everything else is a rank test against copies of the same file (other frame lengths, time-shifted channels), or a tail fitted to the file's own noise, or a count that the maths gives (how many parts must agree = the number where agreeing by luck is rarer than kAlpha).
- Config numbers are now in four boxes: CHOSEN (1), DEFINITION (what is measured, becomes a faceplate knob), PUBLISHED (3, masking only, never a decision), ASSUMED (**0 left that change a yes/no**).

**Click finder limit: fixed.** Before: a click quieter than about 8 times the local typical size was never found. Now a 40 ms fold (energy fold for clicks in any direction, matched fold for clicks pointing the same way) shows a pattern far below that. Measured in made-up noise at -40 dBFS rms: one-by-one finding found **0 of about 1200** clicks even at -30 dBFS peak, while the fold showed the pattern in **6 of 6 clips down to -45 dBFS peak** (about 5 dB under the noise), and in 1 of 6 at -50. Every file prints its OWN smallest showable click (line "Smallest click this file can show"). In the test noise it reported -48.4 dBFS and nothing was shown more than 3 dB under it. For clicks that flip direction every other one, the limit is higher (-32.3 dBFS in that noise) and is printed separately.

**Wobble reliability: fixed in what it claims, not in what it can see.** Depth is leakage-corrected and every line carries a RANGE (1 in 100) that includes the meter's own gain uncertainty. In the tests the true depth was inside the range in 12 of 12 clips (depth 0.10) and for the clean 0.20 case. A strong beat raises the clip's noise level and can HIDE a wobble: the 7 Hz, 0.20 line was not listed under the made-up beat. That is not hidden any more: the report prints "Smallest wobble depth this clip could show" for a typical rate and for the worst rate. The old "depth read 0.117 under a beat" problem is gone because that case now says "hidden, here is how deep it must be" instead of giving a wrong number.

**Measured false alarms on clean made-up files (target 1 in 100):** click fold 0 of 140; notch 0 of 100 files (1 of 4000 readings); wobble 0 of 200; hits 0 of 40; phase 7 of 640 bands (1.1 %); echo 2 of 80 clips (2.5 %, lag 50-60 ms, strength about 0.013: not proven to be 1 %, would need more runs). Smallest detectable sizes (made-up noise, also measured): notch 100-Hz-wide steady dip: shown from 5 dB, not at 4 dB (the old "8 dB rule" was too strict, the old test also called a 6 dB dip "not a notch"; it is one); wobble: shown at depth 0.02, not at 0.016 in noise whose own limit read 0.015-0.019; echo (200 ms): shown down to -18 dB, 3 of 10 at -22 dB.
**Echo search now starts at 40 ms**, not 20 ms: below that, a hit's own onset looks like a repeat (5 of 80 clean clips were accused at 20-24 ms before the change).

**Still true: a steady beat that divides evenly by 40 ms can also fold strongly, and any steady rhythm also "repeats" (echo).** The tools say so in their text. Real files: UNKNOWN.

---

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
| hc_hiss_lab_tests (new engines 8-9, 11-17, 19 + calibration over many clean files) | 68 | 0 |
| hc_hiss_tools_tests (engines 18, 20, gate, four tools) | 41 | 0 |

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
| 8 Click places (one by one) | `seamClicks.clicks` | 250 clicks every 40 ms in noise | about 93 % found within 0.25 ms, 0 invented; gap 40.000 ms; rule now 8.7x worked out from clip length; clean noise 0 clicks | WORKING for loud clicks. Quiet ones: use the fold |
| 8 Pattern call (fold) | `seamClicks.pattern`, `foldLevelDb`, `detectLimitDb` | -20 dBFS train; 33 ms train; sweeps to -75 dBFS; either-direction trains; 140 clean noises | called at -20 (level read -20 within 3 dB); 33 ms not called; shown down to -45 dBFS peak in -40 dBFS noise; 0 of 140 clean noises called | WORKING |
| 9 Notch frequency | `trackNotch` | 15 dB dip sliding 11 to 15 kHz | mean error 12 Hz, worst 40 Hz; drift 284.4 vs 285.7 Hz/s | WORKING |
| 9 Notch depth | `trackNotch` | same | 11.6 dB for a true 15 | PARTLY: reads about 3 dB under; range +/-4 dB printed |
| 9 Notch call | `trackNotch.found` | 100 clean noises; dips 2 to 15 dB | 0 of 100 clean accused; steady dip found from 5 dB (not at 4) | WORKING |
| 11 Tail time | `hitsAndTails` | tau 40 ms and 120 ms | 87.0 vs 92.1; 244 vs 276.3 ms | WORKING with a 5 to 12 % short bias |
| 13 Averaged tail | `averageHits` | same | 92.0 vs 92.1; 262 vs 276.3 ms | WORKING |
| 14 Echo delay | `echoCheck` | 80 ms at -10 dB; 200 ms at -6 dB | 80.0 and 200.0 ms | WORKING |
| 14 Echo strength | `echoCheck` | same | 0.237 for 0.316; 0.486 for 0.501 | PARTLY (first case 25 % low) |
| 14 Echo call | `echoCheck.found` | 80 clean hit trains; echoes -6 to -22 dB at 200 ms | 2 of 80 clean accused (2.5 %); found 10 of 10 down to -18 dB, 3 of 10 at -22 | PARTLY: false-alarm rate not shown to be 1 % |
| 15 Wobble rate | `highBandWobble` | 7 Hz depth 0.20 | 6.998 Hz | WORKING |
| 15 Wobble depth | `highBandWobble` | 0.20, 0.10, 0.05 | 0.207, range holds the truth 12 of 12 (0.10); 0.050 | WORKING alone; under a strong beat the line can be hidden and the clip says how deep a wobble must be to show |
| 15 Wobble call | `highBandWobble.found` | 200 clean noises; depths 0.004 to 0.03 | 0 of 200 clean accused; shown at 0.02, not at 0.016 | WORKING |
| 16 Phase / agreement | `phaseConsistency` | same / delayed 4 samples / two noises; 80 unrelated pairs | 1.000 and 0.00 deg; 35.8 deg for 36, agreement 0.993; noises 0.028; unrelated bands called related 7 of 640 (1.1 %) | WORKING |
| 12 Masking margin | `maskingMarginDb`, `maskingShare` | hand-worked numbers | -15.5, +34.5, +9.5 dB exact; loud noise covers a -120 artifact 100 %, a +10 artifact 0 % | WORKING as maths. The model is ASSUMED. NOT a hearing test |
| 17 Maker | `maker::*` | checked by OTHER meters | comb -80 reads -79.99; flutter 0.30 reads 0.321; click train reads -82.66 against -82.66 from maths | WORKING |
| 18 Blind plan | `blindPlan`, `makePack` | 400 pairs; files | 54.5 % A as X; commitment = SHA-256 of key; X files bit-equal to source; no source name in any file | WORKING |
| 19 Fingerprint, strip | `sha256Hex`, `stripName` | 3 published SHA-256 vectors; file with a title chunk | all match; chunk and title gone, audio bit for bit | WORKING |
| 20 Report | `makeReport` | text checks | ranges + status on every line; bad news first; deterministic | WORKING |
| Tamer gate | `tamerGate` + CLI | 6 proof files; CLI with no proof | refused 5 of 5 bad files; CLI exits 3 and writes nothing | WORKING |
| Bypass | all tools | buffers compared before and after | 0 samples differ (the tools only read) | WORKING |

## Fake hunt (Step 6)
Searched all new files for: demo, sample, example, placeholder, mock, fake, TODO, stub. Hits: the word "sample" meaning an audio sample, and none else. Numbers typed in: all working guesses are in `HissConfig.hpp` section LAB, tagged ASSUMED. Screens: none exist, so nothing painted. Dead controls: the command-line numbers all reach the engine (the 5 inject kinds are run by the tests through the same functions).

## ASSUMED (what is still a guess)
- **Count of guessed numbers that change a yes/no call: 0.** The one chosen number is kAlpha = 0.01 (not a guess: a choice of how many false accusations are tolerable).
- Model assumptions inside the tests, not numbers: clean-file noise is bell-shaped and steady; made-up hits and clicks look like real ones; the dip null (see tailBar) assumes a notch fills under 10 % of the band. These are written in the code beside each test.
- Masking: three PUBLISHED textbook constants (14.5 dB, 25 and 10 dB per Bark). Not derived or measured here. The masking line is labelled ASSUMED and never accepts or rejects a finding.
- Config numbers that are DEFINITIONS (bands, windows, search ranges, the 20 dB tail definition, 60 ms between hits). They are what is measured, and they become faceplate knobs. Moving one changes the question; tests do NOT yet move each one and check the reading (F18, still open).

## UNKNOWN
- Everything on real Suno files (cause, size, whether any of these meters sees it). The tools can only say what they measured.
- Whether music with sharp high-frequency drums looks like clicks to the click finder.
- Wobble depth under a strong beat (now: hidden, with the limit stated).
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

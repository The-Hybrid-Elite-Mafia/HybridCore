# Hiss Tamer 1C. Phase 1 report: can I trust my own ruler?

Branch `hiss-tamer-1`. Draft pull request only. Never merged to main.
Every number comes from `RESULTS.json` and the `results/check*.json` files next to it. The plan was committed first, in `SPLIT.json` (commit dafe2ef), before any measurement.

## The first paragraph (tool status)

**One of my measuring tools is broken. The other five work.**
The broken one is the 25 Hz flutter meter. I injected a known 30 % flutter into human cymbals. The meter found it within 20 % in only 4 of 10 tries. I changed the meter twice. The first change had a plain bug (3 of 10 and 4 of 10). The bug-fixed one got 2 of 10 on 10 clips it had never seen. Your rule says all six checks must pass, so **Phase 1 FAILS and I stopped.** I did not start Phase 2.

## The six checks

| # | Check | Result | The numbers |
|---|---|---|---|
| 1 | Bandwidth audit | **PASS** (TESTED) | Tool found known cutoffs within 15 Hz on 16 made-up tests. 171 real files measured. 140 reach 19 kHz or more. **31 are disqualified** (below 19 kHz). |
| 2 | Metric sanity | **PASS** (TESTED) | I cut the 5–10 kHz band by 10 dB. The 16–20 over 5–10 ratio rose 10.01 dB. The real 16–20 kHz level moved 0.02 dB. So the ratio can rise only because the bottom fell. |
| 3 | Line-level sanity | **PASS** (TESTED) | Both ways of measuring a 25 Hz line agree within 1 dB when the line is clearly above the noise. Details below. |
| 4 | Synthetic injection | **FAIL** (TESTED) | Air 9/10. Post-hit smear 9/10. Moving notch 9/10. **Flutter 4/10, then 3/10 and 4/10 (bug), then 2/10.** |
| 5 | Sensitivity floor | **PASS** (TESTED) | Table below. |
| 6 | Independent tool check | **PASS** (TESTED), with two gaps | Python and ffmpeg agree on 14 of 14 files. Worst gap 0.109 dB. `sox` could not be installed here (UNKNOWN). Sonic Visualiser by hand cannot be run here (UNKNOWN). |

## What each check tells you

**1. Bandwidth.** Many files stop before 19 kHz.
- All 7 MP3 stems end in a hard wall near 18 kHz. That is a codec signature. They are out.
- 4 of the 8 June 24 stems end below 19 kHz (walls or slopes at about 12, 15.4, 15.7 and 16.1 kHz). The other 4 reach the top. I cannot say which are drums without opening the key, and I did not.
- 6 of the 12 stems of the 12-stem song, 1 of 10 ending-swoosh stems, 1 of 8 No Brakes stems and the Osiris file are also below 19 kHz.
- 11 of the 51 human files are below 19 kHz too (dull instruments). So human files must be bandwidth-matched as well.
- Most Suno stems are 48 kHz. All human files are 44.1 kHz. The band used must be inside both.

**2. Metric.** The ratio alone can fool you. I will always show the absolute 16–20 kHz level next to it. The group medians I computed mix drums, voice and bass, with huge intervals (for example the human median is −22.4 dB, interval −29.0 to −20.5, 32 files). They show nothing about drums versus drums. I make **no claim** from them. Role-matched comparison is Phase 2.

**3. Line levels.** TESTED on injected lines at −110, −100 and −90 dBFS per line.
- Quiet background: both methods land within 0.02 dB of the truth.
- Loud background: the averaged-spectrum method misses badly (+13 dB wrong at −110, because noise fills its wide window). The narrow-band method stays within 0.4 dB. **Use the narrow-band method for the comb.** (Window 0.5 Hz for a 20 s clip.)
- A catch I found: music that happens to sit on the 25 Hz grid looks like comb. So I now subtract the level halfway between the lines. Only what repeats every 1/25 s counts.
- On 120 real 48 kHz stems and songs: the typical comb line is about −110 to −160 dBFS depending on group (No Brakes about −110, Days about −128, 12-stem about −128, Inspired by Sound about −162). The loudest single "line" reading is −59 dBFS, but that is music on the grid, not comb.
- Against my masking guess (ASSUMED, see below): **0 of 120 files have comb above the masking level in loud passages.** 23 of 120 have it above the level in quiet passages (No Brakes 4/8, Inspired by Sound 4/13, June 24 3/8, mp3 3/7, 12-stem 2/12, Days 2/7, swoosh 2/10, others 1 each). 87 of 120 are above the plain 16-bit noise floor in at least one band.
- **Whether you can actually hear any of this is UNKNOWN.** That is Phase 3.

**4. Injection.** What worked and what did not.
- Air (+6 dB at 16–20 kHz): 9/10 as first defined. The one miss was 1.04 dB off a 1.0 dB limit. When I hold the "loud frames" fixed, it is 10/10.
- Smear (tails of 8–20 kHz after hits): 9/10. A catch: the injected tails were themselves counted as new hits (12 hits became 26 in one clip).
- Moving notch: 9/10. One clip failed badly (776 Hz off).
- **Flutter: broken.** Real cymbals already wobble in loudness, and a hit is a big loudness swing. The meter reads that as 25 Hz movement. Measured values ran low (about 0.23 for a true 0.30) and the baseline was not zero. Three versions, three fails. Version 2 had a plain bug (divided by near zero in gaps) and gave nonsense. Version 2.1 fixed that and was judged only on 10 clips it had never seen: 2 of 10. All three runs are saved in the folder.
- What the flutter meter can still do: at 0.20 depth or more it said "flutter is there" in 10 of 10 tries. It cannot tell 20 % from 30 %.

**5. Smallest change each meter can see** (TESTED, 8-second human cymbal clips, 10 tries):

| Meter | Smallest change seen in 9 of 10 tries |
|---|---|
| Air, 16–20 kHz level, as first defined | 3 dB |
| Air, with the loud frames held fixed | 0.25 dB (the lowest I tried) |
| Post-hit smear | 3 dB (tail energy equal to the hit's own) |
| Flutter | 0.2 depth, detect only |
| Moving notch | 14 dB deep at its centre (full strength = 1.0, 0.8 passed, 0.5 did not) |
| Comb, in silence at the 16-bit floor (20 s) | at or below −155 dBFS per line (the lowest I tried) |
| Comb, under loud human cymbals (8 s) | **−105 dBFS per line** |

Read the last row carefully. The real combs I found are −110 to −130 per line. **An 8-second look under loud cymbals cannot see them.** So "I found no comb in the loud part" never meant "none there". Longer clips do better (about 5 dB per ten times longer, ASSUMED, not measured).

The "loud part" rule has a flaw I found: on a peaky clip with few loud frames, adding a little air changes which frames count as loud, and the 1 dB shows up as −3 dB. For a fair test the same frames must be used before and after. Phase 2 must do that.

## What is ASSUMED and what is UNKNOWN

- ASSUMED: playback level (−20 dBFS rms is 83 dB SPL, so dB SPL = dBFS rms + 103); Terhardt hearing threshold; a noise hides a tone when the tone is 6 dB or more below it; Bark bands. All sit in `check3.py`. One note: `SPLIT.json` wrote the playback rule as "full-scale sine = 103". That was a slip. A full-scale sine is −3 dBFS rms, so 100 dB SPL under the same rule. The code uses the right rule.
- ASSUMED: dates and model names. They are what you told me or the file dates on your PC.
- UNKNOWN: whether the comb is audible; its cause; what the true cutoff of the MP3s' encoder was (header gave none); sox results; Sonic Visualiser by hand; 16-bit-or-better limits for MP3/AAC delivery floors.
- Limit: the human yardstick is 3 songs (Cambridge-MT). That is thin. Phase 2 needs 20 to 40 references from other sources.

## What I could not do here

- CI on GitHub's Windows runner can only re-run the made-up (synthetic) parts of checks 1, 2 and 3. The real files cannot go on GitHub (yours, and the human songs are test-use only). Real-file numbers are regenerable on a machine that holds the files, keyed by the SHA-256 list in `assets.json`.
- The key file (hash to file name) is still closed. It is not in the repo. I never used it.

## Your decision

I stopped as you asked. The flutter meter failed under the rule I wrote before I looked. Options:
1. **Drop the flutter meter** and move to Phase 2 without it. Flutter becomes "not measurable with this tool".
2. **Keep it as detect-only** (is flutter of 0.2 or more present, yes or no). I would write that as a new dated rule file, not by editing `SPLIT.json`.
3. **Build a better flutter meter** and test it on fresh clips first.

I recommend 2 plus Phase 2 now: it costs nothing and the earlier question (is there 25 Hz flutter in loud parts) only needs detect-only.

Reply **GO** with a number (1, 2 or 3), or just GO and I will take 2.

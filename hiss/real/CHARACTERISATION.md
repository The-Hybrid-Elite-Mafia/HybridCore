# Phase 1B, step 2 - what the artifact really is (tuning set only)

Looked at: "No Brakes on This Trip" (full mix + 7 stems) and "Welcome to the Cosmic Abyss" (WAV + MP3). Hold-out song not opened.
All files: 48 kHz, 16-bit stereo. MP3: 194 kb/s, 48 kHz, starts 23 ms late (encoder delay).

## What I expected (from the brief)  vs  what the files show
| Question | Answer from the real files |
|---|---|
| A steady hiss band at 11.25-12.25 kHz? | **No.** In quiet parts the level is smooth and broad (about -70 dB per bin in the mix, -85 to -95 in stems). No raised band at 11-12 kHz. Biggest broad "bump" anywhere is only 5-10 dB and moves around from file to file. |
| Single tones? | Not single ones. **Many** narrow lines (see next row). |
| Periodic flutter? | **Yes, at 25 Hz**, faint and on and off. Quiet parts of the Drums stem: loudness of the 8-16 kHz band repeats every 1920 samples (exactly 40 ms = 25.0 Hz), line +17 dB over the modulation floor. Lead Vocals stem shows 75 Hz (3 x 25). Slow rates (2.3, 4.5, 6.4 Hz) are the song's beat, not the artifact. |
| Codec swirl or holes? | A **comb of lines spaced 25 Hz apart**, from about 3 kHz to 20 kHz, in the quiet floor of the stems. |
| Real frequency range | 3-20 kHz (hard low-pass at 20 kHz). Strongest lines: 14.4 kHz, 16 kHz, 10 kHz, 6 kHz, 18 kHz, 11.6 kHz, 12 kHz (+15 to +29 dB over their neighbours). 11.25-12.25 kHz holds lines too, but nothing stronger than the rest. |
| Every stem, some stems, or only the mix? | **Stems clearly. Mix: masked.** Lines on the 25 Hz grid: Lead Vocals 275 of 303, Drums 429 of 494, Percussion 331 of 336, Bass 96 of 96, Guitar 27 of 27 (a random set would put about 12% on the grid). Full mix: only the side channel at 16 kHz (+11.8 dB). Cosmic Abyss WAV mix: 18 of 18 lines on the grid, all in the side channel; none in mid. |
| MP3 vs WAV: does the codec cause it? | **Not the MP3.** The comb is already in the WAV. The MP3 keeps fewer of the lines (4 vs 18 in Cosmic Abyss side; 3 of 4 on the grid) and cuts everything above about 18.4 kHz. |

## Meaning
The "shimmer" in these files is not a steady noise band. It is a faint 25 Hz repeating pattern spread over many frequencies. My guess (NOT tested): it comes from how Suno builds the audio, in 40 ms pieces. It is loudest in stems and in the side channel, and a loud mix hides most of it.

## What this means for the engine (not changed yet)
- The Floor / Percentile methods look for a steady band. This is a comb. They would find little.
- The Tonal method handles one line, not hundreds spaced exactly 25 Hz apart.
- A new **Comb** method is needed: find the lines on the 25 Hz grid, cut only those lines (and their 25 Hz ripple), keep everything between them. This is a larger change than a setting.

## Not yet known
- Whether anyone can HEAR this (lines sit at -70 to -100 dB per bin; the mix hides them). Only listening answers that.
- Whether the comb is in every Suno song (one song's stems plus one mix so far).
- Whether the hold-out song (240 Days) shows the same thing. Not opened on purpose.

Reproduce: `hiss_characterise.py`, `hiss_comb.py` in `hiss/python`. Pictures and numbers: `characterisation/`.

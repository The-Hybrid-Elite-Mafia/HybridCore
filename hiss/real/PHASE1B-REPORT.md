# Hiss Tamer, Phase 1B report (plain words)

Branch: hiss-tamer-1. Not merged to main. Windows build and tests: passed on commit 7133e91.

## What the artifact really is (MEASURED)
- Not a steady 11-12 kHz hiss. It is a comb: thin lines every 25 Hz, from about 3 kHz to 20 kHz, repeating exactly every 1/25 second.
- It is in the WAV files, so the MP3 codec does not cause it.
- It shows up clearly in quiet stems and is mostly buried by loud music in full mixes.
- The line pattern is almost the same across songs (match 0.66-0.98; unrelated signals score about 0.04). It looks like one shared template.
- Cause: UNKNOWN. Whether anyone can hear it: UNKNOWN (Steven says he hears it in some songs).
- Not every song has it. Of the extra files tested after tuning: only 1 of 18 stems/mixes showed a clear comb, plus Room Tone.

## What changed in the engine
New comb remover (HissCombEngine + HissCombAuto). It learns the pattern only from the quietest parts of the file, then subtracts it. It refuses to act if it cannot prove it helped. Touched files are saved as 24-bit (a 16-bit save rounds the fix away). Untouched files keep their exact bits.

## Tests
- Synthetic: removes 12.95 dB (48 kHz), 14.36 dB (44.1 kHz), 14.75 dB (96 kHz).
- Known-answer on 12 clean human stems (Cambridge-MT, test only, not shipped), comb added at -90 dB: 0 false alarms on the clean originals; 10 of 12 fixed, removing 10.2 to 14.5 dB of the added comb; 2 (Overheads, Room drums) refused. Taken-out music not measured separately beyond the "wrong_taken" figures in /tmp results (UNKNOWN for cymbal/air at other levels).
- Hold-out (240 days, never tuned on): Drums, Guitar, Keyboard fixed (15 dB cut, grid lines 343->101, 141->21, 197->26). Bass, Percussion, Synth, Vocals correctly left alone. Removed part is one repeating pattern (share 1.0), no flags, loudness change under 0.0001 LU, true peak unchanged, timing lag 0.
- noisereduce (free baseline): removes far more (-25 to -60 dBFS rms), follows the music (envelope match 0.74-1.0), changes loudness by up to 8.6 LU on Guitar, and leaves the comb in Drums (359 lines) and Keyboard (197). It did lower the comb in Guitar. No "better" claim either way beyond these numbers.
- PEAQ (peaqb-fast, ITU BS.1387 Basic): tool ODG equals the untouched reference within 0.07 on Drums and Guitar and exactly on Keyboard; noisereduce lower on Guitar (-0.7 / -1.4). PEAQ is unreliable on near-silent excerpts (quiet Drums and Keyboard excerpts score -2.37 even against themselves), so those are not evidence.
- ViSQOL: NOT RUN (UNKNOWN). Needs a build and a model that cannot be fetched here.
- Blind A/B pack: built, 11 pairs, 2 catch pairs, level-matched, key hidden until Finish.

## Limits (TESTED)
- Needs several seconds near the file's quiet floor. Loud full mixes hide the comb; the tool refuses.
- Lead/backing vocals, guitar, synth on the tuning song were refused. "Inspired by Sound only" Keyboard has a comb but no quiet floor, so refused.
- "Circle Jerk Boys": its 12 stems are all zeros, mix only, nothing could be tested.
- Forced runs on music with no quiet floor can make things worse.

## ASSUMED / UNKNOWN
ASSUMED: all numbers in HissConfig.hpp. UNKNOWN: cause; audibility; whether every Suno song has it; ViSQOL.

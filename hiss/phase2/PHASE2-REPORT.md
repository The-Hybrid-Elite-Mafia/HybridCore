# Phase 2 report: human air table (SMALL version)

**Read this first.** This is **not** the table you asked for. Phase 2 asked for 20 to 40 human references from several sources. I have **17 stems from 4 songs, all from one source (Cambridge-MT).** Your PC holds nothing else made by real players, and you chose not to download the 40 GB set. So this is a thin first look. It cannot support a "Suno has too much or too little air" claim. I make **no Suno-versus-human claim here.** The closed key stays closed.

The plan, the file list and every file's SHA-256 were committed **before** I measured (commit 93455a6, `phase2/PHASE2-PLAN.json`). Numbers come from `phase2/RESULTS-air-table.json`. Regenerate: `python3 -I python/phase2/air_table.py phase2/PHASE2-PLAN.json <folder of the unpacked songs> out.json`.

## What the numbers are
- **5-10 kHz level / 16-20 kHz level:** loudness of each band in the loud frames of that stem (dB; 0 dB = a full-size sine). Stem levels, not mix levels.
- **Ratio:** the 16-20 kHz level minus the 5-10 kHz level. Higher means more "air" for the same body of sound. Remember the Phase 1 lesson: the ratio can rise only because the lower band fell, so look at the two levels beside it.

## All 17 stems
| Role | Song | 5-10 kHz level (dB) | 16-20 kHz level (dB) | Ratio 16-20 over 5-10 (dB) | Loud frames | Status |
|---|---|---|---|---|---|---|
| chimes | KarlHungus 12 | - | - | - | - of - | EXCLUDED: sound ends at 18556 Hz (WALL) |
| claps | KarlHungus 07 | -28.2 | -42.2 | -14.0 | 244 of 2366 | OK |
| cymbal_crash | Kangoro 06 | -29.4 | -50.3 | -20.8 | 606 of 1719 | OK |
| drum_loop | Kangoro 09 | - | - | - | - of - | EXCLUDED: sound ends at 16838 Hz (GENTLE) |
| drum_room | Kangoro 07 | -38.7 | -61.6 | -22.9 | 614 of 1719 | OK |
| drum_room | TheLongWait 04 | -43.7 | -63.9 | -20.2 | 648 of 1854 | OK |
| hihat | KarlHungus 09 | - | - | - | - of - | EXCLUDED: sound ends at 16941 Hz (WALL) |
| overheads | AngelaThomasWade 04 | -32.9 | -49.5 | -16.6 | 978 of 2499 | OK |
| overheads | TheLongWait 03 | -48.4 | -61.5 | -13.1 | 647 of 1854 | OK |
| snare | AngelaThomasWade 02 | - | - | - | - of - | EXCLUDED: sound ends at 18647 Hz (GENTLE) |
| snare | Kangoro 04 | -29.3 | -50.3 | -21.0 | 214 of 1719 | OK |
| snare | Kangoro 05 | -37.3 | -47.3 | -10.0 | 73 of 1719 | OK |
| snare | KarlHungus 03 | -27.4 | -41.8 | -14.5 | 218 of 2436 | OK |
| snare | KarlHungus 04 | -26.7 | -38.1 | -11.4 | 299 of 2466 | OK |
| snare | TheLongWait 02 | -55.5 | -85.0 | -29.5 | 602 of 1854 | OK |
| tambourine | Kangoro 08 | -34.8 | -52.0 | -17.2 | 298 of 1719 | OK |
| toms | AngelaThomasWade 03 | - | - | - | - of - | EXCLUDED: sound ends at 15654 Hz (GENTLE) |

## By role
| Role | Files used | Ratio median (dB) | Lowest | Highest |
|---|---|---|---|---|
| chimes | 0 of 1 | - | - | - |
| claps | 1 of 1 | -14.0 | -14.0 | -14.0 |
| cymbal_crash | 1 of 1 | -20.8 | -20.8 | -20.8 |
| drum_loop | 0 of 1 | - | - | - |
| drum_room | 2 of 2 | -21.5 | -22.9 | -20.2 |
| hihat | 0 of 1 | - | - | - |
| overheads | 2 of 2 | -14.9 | -16.6 | -13.1 |
| snare | 5 of 6 | -14.5 | -29.5 | -10.0 |
| tambourine | 1 of 1 | -17.2 | -17.2 | -17.2 |
| toms | 0 of 1 | - | - | - |

## What this shows (TESTED, on these files only)
- **Real human stems vary a lot.** Across the files that pass, the ratio runs from about -29 dB to about -10 dB. That is a spread of about 19 dB. Any Suno number inside that spread would not be "wrong" by this table.
- **Snare is the widest** (one snare at about -29, another at about -10). Different mics, different samples and different processing explain a spread like that. That is a guess (ASSUMED), not tested.
- **5 of 17 stems were excluded** because the sound ends below 19 kHz (Phase 1 rule). That includes the only hi-hat (it stops at about 16.9 kHz), so **there is no human hi-hat reference in this table at all.** The drum loop, a tom, a snare and the chimes also stop early. Part of that may be MP3-style processing in the source or the way the samples were made. Cause UNKNOWN.
- **Only the snare has 5 usable files.** Every other role has 0 to 2. The "95 % interval" printed for snare is just the lowest and highest file; with 5 files it says almost nothing.
- **Crash cymbal: 1 file. Tambourine: 1 file. Drum room: 2. Overheads: 2.** One file is an example, not a yardstick.

## What this does NOT show
- Anything about Suno. No Suno stem was compared.
- Anything about what you hear. Audibility is Phase 3.
- Anything for rock/pop/rap/country full mixes. These are single tracks from four songs.
- The Karl Hungus song is programmed (808s, samples), not played drums. A zip on your PC is cut off, so only 15 of its 18 files could be recovered (all CRC-checked); the last 3 are lost.

## What is still needed (Phase 2 stays OPEN until this is fixed)
20 to 40 references from several sources. The missing piece is hi-hat and cymbal examples with real air. The cheapest fixes, your choice:
1. Download 8 to 10 more Cambridge-MT songs (each is a small download) and re-download Karl Hungus. Different genres.
2. Give me your own recordings made by real players.
3. Accept the thin table and move on, knowing every later comparison is weak.

I stopped here. I did not start Phase 3.

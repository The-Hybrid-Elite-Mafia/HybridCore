# FAILURES.md  (kept up to date; every failure is added here, then fixed)

Format: what failed, how I know (TESTED), what I did, state now.

| # | What failed | How I know | Fix | State |
|---|---|---|---|---|
| F1 | 25 Hz flutter meter v1 recovered a known 30 % flutter within 20 % in only 4 of 10 clips | check4.json `flutter` | New meter being built (v3) and judged on clips never used before | OPEN |
| F2 | Flutter meter v2 divided by almost zero in quiet gaps and gave nonsense (values of 65 000) | check4_run2_flutter_v2_numeric_bug.json | v2.1 floor and clip. Still failed (2 of 10 fresh) | superseded by F1 |
| F3 | "Loud part" rule flips on peaky clips when air is added: 1 dB read as -3 dB | check4.json `air`, 3 of 10 at small sizes | Held the loud frames fixed in the test. Phase 2 must pair frames | FIXED for tests, OPEN for Phase 2 |
| F4 | First cutoff tool called a clean low-pass edge "a wall" at Nyquist and read the end of a fall, not the start | synthetic check 1 first run: 0 of 6 | Mirror the spectrum edges, report the start of the fall | FIXED (16 of 16 pass) |
| F5 | MP3 true cutoff cannot be read from the file header | ffmpeg writes zeros there | Test only "a wall under 20 kHz". True cutoff UNKNOWN | ACCEPTED, labelled UNKNOWN |
| F6 | Grid music looked like comb (max line -59 dBFS) | check 3 first real run | Subtract the level halfway between lines | FIXED |
| F7 | `sox` could not be installed | apt failed | ffmpeg and (new) libebur128 and my own C++ meters used as other tools | ACCEPTED, labelled UNKNOWN for sox |
| F8 | Sonic Visualiser by hand cannot run here | no screen | none possible | ACCEPTED, UNKNOWN |
| F9 | SPLIT.json wrote the playback rule as "full-scale sine = 103 dB SPL" | re-read | Code uses dB SPL = dBFS rms + 103. Note in the report. SPLIT.json is never edited | NOTED |
| F10 | Smear meter counts its own injected tails as new hits (12 hits became 26) | check4.json `smear` | Still passes 9 of 10; floor is 3 dB. Phase 2 must freeze hit times from the clean clip | OPEN for Phase 2 |
| F11 | Flutter meter v4 (rule committed first, see SPLIT-ADDENDUM) recovered the known 0.30 depth within 20 % in only 4 of 10 never-used clips. Readings were steadily low: mean 0.218, spread 0.049 | check4b.json | Rule says: do not change the meter on these clips. Flutter becomes detect-only. The error is mostly a steady bias (about -27 %), so smoother averaging (multitaper) would not fix it | OPEN: gate 4 stays FAIL for quantified flutter |
| F12 | The "smallest flutter seen" table in check4b.json used a threshold of 0.0001, because the shifted clips read almost the same. It claims 0.05 is seen 10 of 10 times. That is wrong: human clips with nothing injected read up to 0.16 | check4b.json `repeatability_threshold`, baseline readings | Redone after the fact against the highest no-injection reading (check4b_posthoc.json, labelled POST-HOC) | FIXED in posthoc file |
| F13 | A first full run timed out in the foreground (2 minute limit); a loop used np.convolve with a huge window | var.py run | Moving averages use scipy uniform_filter1d. Long jobs run in the background and are polled | FIXED |
| F14 | Advice pasted in on 2026-10-29 said to port to "C++23 using JUCE" | message | Not used. Your rule is pure C++, no JUCE | NOTED |
| F16 | CI branding check failed on the plain computing word "API" inside a rule file | CI run on f747b2a | Reworded the line (no company involved). Green after | FIXED |

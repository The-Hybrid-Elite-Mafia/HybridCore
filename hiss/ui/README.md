# HEM Hiss tool windows (Windows programs)

Pure C++ (Win32 + GDI+), no JUCE, no web pages. Everything on screen is drawn by code and every control is wired to an engine.

## hem_comb_click.exe  (Comb and Click Finder)
| Control | What it does | Status |
|---|---|---|
| Knob CLICK PERIOD (10-100 ms, double-click = 40) | sets the frame length the click fold test uses (`seamClicks(periodMs)`) | WORKING: window self-test turns it with real mouse messages, then checks the engine result changes (55 ms found, 40 ms not) |
| OPEN WAV / drop a file | loads PCM 16/24/32 or float WAV | WORKING (a made-up WAV loaded in the self-test; the open dialog itself was not driven) |
| SCAN | runs `combLines` + `seamClicks` on a worker thread | WORKING (self-test) |
| SAVE REPORT | writes the same plain-words report as the command-line tool, next to the WAV | WORKING (self-test reads the file back) |
| Big window | the fold: click band added up inside one frame, with the 1-in-100 bar | WORKING, real data |
| Verdict / smallest click / comb windows | real engine numbers | WORKING, real data |

TESTED on made-up clips only. Real songs: UNKNOWN. Looks: checked from pictures made by the Windows build machine, not by a person yet.
Non-ASCII file names: UNKNOWN (files are opened with the ANSI code page).

Command line used by the build machine:
- `hem_comb_click.exe --snapshot out.png [--made-up <clickMs>] [--period <ms>] [--idle]`
- `hem_comb_click.exe --gui-selftest log.txt [after.png]`

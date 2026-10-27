# HEM Digital Console — engines (Phase 1)

Original HEM designs, built from the reference hardware's own manuals. Sound is "in the spirit of" the
reference rig; nothing here claims to be identical to it. Every number the engines use lives in ONE file,
[`common/include/hybrid/console/ConsoleSpecs.hpp`](common/include/hybrid/console/ConsoleSpecs.hpp), tagged
VERIFIED (printed in the manual), FITTED (measured off the maker's own graph), SOURCED, ASSUMED or UNKNOWN.
Change a guess there and nowhere else. The stem count comes from `hybrid/core/Config.hpp`.

| Library | Engine | What it is |
|---|---|---|
| `hc::console_desk` | **HEM STUDIO DESK** (`StudioDesk`) | 13 full stereo channel strips, main bus, Bus B, two cue buses, EXT returns, monitor section, bus compressor |
| `hc::console_stepeq` | **HEM STEP EQ** (`makeStepEq()`) | 2-channel, 4-band stepped EQ with proportional Q and RANGE |
| `hc::console_twinbus` | **HEM TWIN BUS** (`makeTwinBus()`) | 2-channel bus compressor (4 modes, colours, negative ratios) + 2-band dynamic EQ + transient expander |
| `hc::console_colour` | **HEM COLOUR STAGE** (`makeColourStage()`) | stereo colour processor: HPF, drive, tone EQ, HF compressor / crush, M/S image, transformer, insert, trims, bypass, meter |
| `hc::console_tools` | **HEM RESO TAMER / SHUFFLE / TAP ECHO** (`makeResoTamer()` ...) + `ToolSlot` | front-end tools in the slots where the reference desk has its mic preamps (our own designs) |
| `hc::console_rig` | **HEM CONSOLE RIG** (`ConsoleRig`) | the whole rig as one signal path + saved state |
| `hc::console_player` | `ConsoleProcessor`, `renderExport`, sessions | the engine processor (15 file slots, transport, LISTEN), offline export / PRINT STEMS, `.hemc` files |
| `hc::console_ui` | `ConsoleScreen` | the six GPU faceplates (DESK, STEP EQ, TWIN BUS, COLOUR, METER, TOOLS) |
| `HEMDigitalConsole.exe` | the app (`console/app`) | window, devices, stems, sessions, export; installer `installer/HEMDigitalConsole.iss` |
| `hc::console_meter` | **HEM METER BRIDGE** (`MeterBridge`) | stereo meter: loudness (momentary / short-term / integrated / range / maxima), true peak, PLR, correlation, balance, mono check, 31-band RTA, loudness history, vectorscope |

Real-time rules: `process()` never allocates, locks or waits (tested). Controls are lock-free atomics
that any thread may set. Step EQ and Twin Bus implement `hc::fx::Effect`, so they plug into the same
hosts as the HEM effects. Nothing in `fx/` was changed.

## Panel checklists

"Built" = the control exists in the engine and changes the sound (or, for lights and meters, reports a
real measured value). "Left out" = it only makes sense with a physical plug, device or power supply.

### HEM STUDIO DESK — every channel (all 13 carry the union of the reference's mono and stereo strips)

| Reference panel control | Engine control (id) | Status |
|---|---|---|
| Mic XLR, 48V (+LED), HI-Z, LINE, mic/line GAIN | — | Left out: microphone preamp (stems arrive after it, as the reference's USB returns do) |
| FROM USB switch | — | Left out: stems always arrive this way |
| TRIM −10..+20 dB (stereo strip) | `trim` | Built |
| MONO (stereo strip) | `mono` | Built |
| Ø polarity | `polarity` | Built |
| HPF 75 Hz | `hpf` | Built (12 dB/oct) |
| ST CUE 1: ON (+LED), level ∞..0, PAN, POST | `cue1_on`, `cue1_lvl`, `cue1_pan`, `cue1_post` | Built |
| ST CUE 2: ON (+LED), level ∞..0, PAN, POST | `cue2_on`, `cue2_lvl`, `cue2_pan`, `cue2_post` | Built |
| EQ HF knob, HF BELL | `hf`, `hf_bell` | Built (shapes fitted to the maker's graphs) |
| EQ MF knob | `mf` | Built |
| EQ LF knob, LF BELL | `lf`, `lf_bell` | Built |
| EQ IN (+LED) | `eq_in` | Built |
| COMP THRESHOLD +10..−20 dBu | `comp_thr` | Built |
| COMP IN (+LED) | `comp_in` | Built |
| COMP 3 gain-reduction LEDs | `channelCompGrDb()`, `channelCompLeds()` | Built (LED points ASSUMED) |
| INSERT (+LED) | `insert` + `setChannelInsert()` | Built: plug-in slot for later tools (empty slot passes audio) |
| PAN (mono strip) / BALANCE (stereo strip) | `pan` | Built: pan law when MONO is on, balance law on stereo |
| USB OUT POST FADER | `direct_post` + `Outputs::directL/R` | Built as the direct out (pre/post fader) for printing stems |
| 8-LED meter (2×8 on stereo) | `channelPeakDbu()`, `channelMeterLeds()` | Built |
| MUTE/BUS B (+LED) | `mute_busb` | Built |
| PFL (+LED) | `pfl` | Built |
| 100 mm fader +10..∞ | `fader` | Built |

### HEM STUDIO DESK — master section

| Reference panel control | Engine control (id) | Status |
|---|---|---|
| EXT 1 FROM USB 13/14, EXT 2 FROM USB 15/16 | — | Left out: hardware input select (EXT returns come straight in) |
| TALK LEVEL, talkback XLR, 48V, talkback compressor | — | Left out: needs a talkback microphone |
| POWER LED, USB LED | — | Left out: hardware status |
| Bus comp THRESHOLD −20..+20 | `bc_thr` | Built |
| Bus comp AUTO RELEASE | `bc_auto` | Built (dual rate) |
| Bus comp MAKE UP 0..+20 | `bc_makeup` | Built |
| Bus comp IN (+LED) | `bc_in` | Built |
| Bus comp 5 LEDs (1, 3, 6, 9, 15) | `busCompGrDb()`, `busCompLeds()` | Built |
| CUE 1 TO USB 9/10, CUE 2 TO USB 11/12 (+LED) | — | Left out: USB routing |
| ST CUE 1/2 MASTER level ∞..+10 | `cue1_master`, `cue2_master` | Built |
| ST CUE 1/2: EXT 1, EXT 2 switches | `cue1_ext1`, `cue1_ext2`, `cue2_ext1`, `cue2_ext2` | Built |
| ST CUE 1/2: TALK switch | — | Left out: talkback |
| PHONES 1/2: ST CUE 1, ST CUE 2 switches, level knobs | — | Left out: headphone jacks (ALT feeds a second output instead) |
| EXT LEVEL 1/2 ∞..+20 | `ext1_lvl`, `ext2_lvl` | Built |
| MONITOR SOURCE: MAIN, BUS B, EXT 1, EXT 2 (they sum) | `mon_main`, `mon_busb`, `mon_ext1`, `mon_ext2` | Built |
| 12-LED main meter, PFL LED | `mainMeterDbu()`, `mainMeterLeds()`, `pflActive()` | Built |
| ALT (+LED) | `alt` + `Outputs::altL/R` | Built: monitor goes to a second output |
| DIM level −30..−3, DIM (+LED) | `dim_lvl`, `dim` | Built |
| MONO | `mon_mono` | Built |
| Monitor level 0..11 | `mon_lvl` | Built (knob law ASSUMED) |
| CUT (+LED) | `cut` | Built |
| BUS B MASTER level ∞..+10, MUTE | `busb_lvl`, `busb_mute` | Built |
| MAIN: EXT 1, EXT 2 (+LEDs) | `ext1_main`, `ext2_main` | Built |
| MAIN: INSERT | `main_ins` + `setMainInsert()` | Built: plug-in slot |
| ST CUE 1 TO BUS COMP & SUM TO MAIN BUS (+LED) | `cue1_comp` | Built |
| Main fader +10..∞ | `main_fader` | Built |
| Rear: SUM I/P TO MAIN, inserts, outputs | — | Left out: rear jacks |

### HEM STEP EQ

| Reference panel control | Engine control (id) | Status |
|---|---|---|
| BYPASS (+LED) | `bypass` | Built (bit-exact) |
| Ch 1/2 IN (+LED) | `in1`, `in2` | Built |
| LO freq (7), gain (11), LO SHELF (+LED) | `lo_f1/2`, `lo_g1/2`, `lo_shelf1/2` | Built |
| LO-MID freq (7), gain (11) | `lm_f1/2`, `lm_g1/2` | Built |
| HI-MID freq (7), gain (11) | `hm_f1/2`, `hm_g1/2` | Built |
| HI freq (7), gain (11), HI SHELF (+LED) | `hi_f1/2`, `hi_g1/2`, `hi_shelf1/2` | Built |
| RANGE x1 / x.5 / x.25 | `range1`, `range2` | Built |
| Power, XLR/TRS jacks | — | Left out: hardware |

### HEM STEP EQ SE (special-edition model; the rack's EQ slot holds this or the standard model: `rack.eq_model`)

| Reference panel control | Engine control (id) | Status |
|---|---|---|
| BYPASS (red) | `bypass` | Built (bit-exact) |
| M/S (blue): ch 1 = mid, ch 2 = side | `ms` | Built (matrix scaling ASSUMED, unity when flat) |
| Ch 1/2 IN (blue) | `in1`, `in2` | Built |
| HP-FILT (blue) + HPF 40..600 Hz, 12 dB/oct | `hpf1/2`, `hpf_hz1/2` | Built (Butterworth shape ASSUMED) |
| LO freq (8: 20 Hz .. 400 Hz), gain (21), LO SHELF | `lo_f1/2`, `lo_g1/2`, `lo_shelf1/2` | Built |
| LO-MID freq (8: 50 Hz .. 1 kHz), gain (21) | `lm_f1/2`, `lm_g1/2` | Built |
| HI-MID freq (8: 800 Hz .. 15 kHz), gain (21) | `hm_f1/2`, `hm_g1/2` | Built |
| HI freq (8: 2.5 kHz .. 40 kHz), gain (21), HI SHELF | `hi_f1/2`, `hi_g1/2`, `hi_shelf1/2` | Built (bands near/above half the sample rate fitted to the analogue curve) |
| Power, XLR jacks, voltage switch | — | Left out: hardware |

### HEM TWIN BUS (each side; side 2 = right-hand controls)

| Reference panel control | Engine control (id, `1`/`2` = side) | Status |
|---|---|---|
| LOW THD | `low_thd` | Built (circuit UNKNOWN; model ASSUMED, measured) |
| F/B | `fb` | Built |
| Colour switch + SET DIST (9 levels) | `grit`, `grit_lvl` | Built (amounts ASSUMED) |
| THRESHOLD (31) | `thr` | Built |
| ATTACK (11) | `att` | Built |
| MAKE-UP (31) + FINE | `makeup`, `fine` | Built |
| POST D-EQ | `post_deq` | Built |
| S/C HPF (OUT, 10..300 Hz) + EXT S/C select (COMP / D-EQ / both / off) | `sc_hpf`, `ext_sc`, `ext_sc_bands` | Built (key = any signal via `twinBusSetKey()`) |
| RATIO (11, incl. −2.5, −1.5, −0.5) | `ratio` | Built |
| RELEASE (9 + AUTO + AUTO 2) | `rel` | Built |
| MIX (31) + classic/parallel setting | `mix`, `mix_mode` | Built |
| LF GAIN (31) / right side LF/HF GAIN + G push | `lf_gain`, `reso` | Built |
| D-EQ LF: COMP..EXP, push ON, FREQ (16), RANGE (23) | `lf_ce`, `lf_on`, `lf_freq`, `lf_range` | Built |
| D-EQ HF: COMP..EXP, push ON, FREQ (16), RANGE (23) | `hf_ce`, `hf_on`, `hf_freq`, `hf_range` | Built |
| HF BELL | `hf_bell` | Built |
| HF > FAST (+AUTO), LF > FAST (+AUTO) | `hf_time`, `lf_time` | Built |
| D-EQ activity LEDs (green/orange/red) | meters `D-EQ LF/HF L/R` | Built |
| Moving-coil GR meters (also show D-EQ freq/range) | meters `GR L/R` + the freq/range params | Built |
| CH 1 IN, CH 2 IN (+overload flash) | `in1`, `in2`, meters `OVER L/R` | Built |
| MODE (Classic Stereo, Σ S/C Stereo, Dual Mono, Mid-Side) | `mode` | Built |
| MID SOLO, SIDE SOLO (+side-solo setting) | `mid_solo`, `side_solo`, `side_solo_mode` | Built |
| Settings: input trim 0/+10/−5, 26 kHz LPF, right meter in stereo | `in_trim`, `lpf`, `right_meter` | Built |
| Transient expander (bonus mode) | `tex` | Built |
| Settings: brightness, relay click, sleep / auto-sleep, meter calibration, UID, soak test, factory reset, code game | — | Left out: no effect on sound |
| EXT SEND jacks, power | — | Left out: hardware |

### HEM COLOUR STAGE

| Reference panel control | Engine control (id) | Status |
|---|---|---|
| O/L LEDs (L, R) | meters `O/L L/R` | Built (+27 dBu) |
| INPUT TRIM ±12 dB | `in_trim` | Built |
| HPF OFF / 30 / 40 / 50 Hz | `hpf` | Built (3rd order) |
| Drive section: DRIVE 1..11, DENSITY MIN..MAX, IN, tri-colour LED | `drive`, `density`, `vd_in`, meter `DRIVE THD` + `colourDriveLedColour()` | Built (harmonics fitted to the maker's spectra) |
| Tone EQ: LOW 30/50/70/90 Hz, HIGH 8/12/16/20 kHz, LOW / HIGH gain ±9 dB, IN | `low_freq`, `high_freq`, `low_gain`, `high_gain`, `eq_in` | Built (shelves fitted to the maker's curves) |
| HF COMPRESSOR: THRESHOLD +10..−10, X-OVER 3..20 kHz, IN, LED | `hfc_thr`, `hfc_xover`, `hfc_in`, meter `HF COMP GR` | Built (fixed timing ASSUMED) |
| Full-band crush mode (hold IN 5 s; X-OVER becomes WET/DRY) | `hfc_crush` | Built (curves ASSUMED) |
| STEREO IMAGE: SPACE ±12 dB, WIDTH ±6 dB, IN | `space`, `width`, `si_in` | Built |
| TRANSFORMER switch | `transformer` | Built (low-end curve fitted to the maker's graph) |
| INSERT (+ hold for M/S mode), PRE EQ | `insert`, `insert_ms`, `pre_eq` + `colourStageSetInsert()` | Built: plug-in slot (Mid on left / Side on right in M/S) |
| OUTPUT TRIM ±12 dB | `out_trim` | Built |
| BYPASS (+ hold for POST I/P TRIM) | `bypass`, `bypass_post` | Built (timing unchanged) |
| Master meter L/R, +24 dBu top, 3 s peak hold, input in BYPASS | meters `METER L/R`, `HOLD L/R` | Built |
| Settings: brightness, relay click, factory reset, memory game, UID, soak test | — | Left out: no effect on sound |
| Rear: XLR inputs/outputs, insert send/return jacks, fuse/voltage, power | — | Left out: hardware |

### HEM METER BRIDGE

Built from the reference desktop meter's product sheet plus the published standards it follows
(ITU-R BS.1770-4, EBU R128, EBU Tech 3341 and 3342). The reference's full user manual is not in hand yet,
so its own buttons and display settings (presets, radar speed, scales) are not built; they wait for the manual.

| Reference feature | Engine reading | Status |
|---|---|---|
| Momentary / short-term / integrated loudness | `momentaryLufs()`, `shortTermLufs()`, `integratedLufs()` | Built (standard; checked with the standard's test signals) |
| Loudness range (LRA) | `loudnessRangeLu()` | Built (standard) |
| Max momentary / max short-term | `maxMomentaryLufs()`, `maxShortTermLufs()` | Built |
| True peak per channel, sample peak | `truePeakDbtp()`, `truePeakNowDbtp()`, `samplePeakDbfs()` | Built (4x oversampled) |
| PLR | `plrDb()` | Built |
| Loudness radar history | `historyCount()`, `historyLufs()` | Built (short-term every 100 ms; radar drawing is Phase 2) |
| Correlation, L/R balance, M/S balance, mono deviation | `correlation()`, `balanceLrLu()`, `balanceMsDb()`, `monoDeviationLu()` | Built (averaging times ASSUMED) |
| 1/3-octave spectrum (RTA) with peak hold | `bandHz()`, `bandDb()`, `bandPeakDb()` | Built (averaging and hold ASSUMED) |
| Vectorscope / stereo position | `scopeWritten()`, `scopeFrame()` | Built (data; drawing is Phase 2) |
| RESET | `reset()` | Built |
| AUTO, SCOPE, cursor/frequency knob, LIB, SYS, EDIT, presets, radar settings | — | UNKNOWN: waits for the reference user manual |
| Inputs (digital/analog/HDMI), network/USB, power | — | Left out: hardware |

### Front-end tools (our own designs, no reference unit)

| Tool | Controls | Status |
|---|---|---|
| HEM RESO TAMER | per band: ON, FREQ 40 Hz-16 kHz, WIDTH (Q 1-12), THRESHOLD 0-24 dB, DEPTH 0-18 dB, SPEED (FAST / MEDIUM / SLOW), LISTEN; meters: cut per band | Built, tested |
| HEM SHUFFLE | SHUFFLE FREQ 100-1000 Hz, SHUFFLE 0-12 dB, MUD FREQ 100-800 Hz, MUD CUT 0-12 dB, WIDTH 0-200 %, MONO BASS, OUTPUT ±6 dB; meter: correlation | Built, tested |
| HEM TAP ECHO | TAP, TEMPO 40-240, NOTE (FREE, 1/1 .. 1/16, dotted, triplet), TIME 1-4000 ms, FEEDBACK 0-95 %, MIX, LOW CUT, HIGH CUT, MODE (STEREO / PING-PONG / MONO) | Built, tested |

Every desk channel's INSERT switch runs that channel's TOOL (OFF / RESO / SHUFFLE / ECHO); the COLOUR STAGE's INSERT has one too.

## The app

`HEMDigitalConsole.exe`: 13 desk channels plus EXT 1 / EXT 2 file slots (EXT 2 is also the TWIN BUS external side-chain key),
one transport, LISTEN (MONITOR, CUE 1 or CUE 2 to the output device), EXPORT (MAIN OUT to WAV, offline, rack latency
removed), PRINT STEMS (every channel's DIRECT OUT to its own WAV), sessions (`.hemc`: stems with absolute and relative
paths + every control), autosave of everything on exit, RESET (twice) with UNDO.
Screens: `hem_console_snapshot` renders every page off-screen (CI publishes them to the `console-screenshots` branch).
Views: seven tabs (DESK, BIG DESK, STEP EQ, TWIN BUS, COLOUR, METER, TOOLS). BIG DESK shows `kBigChannels` (5) strips
bound to whichever channels are on screen. ZOOM 1x..3x (never above 1 window pixel per design unit), Ctrl + wheel
around the mouse, saved in `settings.ini`; text is baked at no less than `kMinTextPx` (13) screen pixels.
Sound: ASIO (Steinberg ASIO SDK under its GPLv3 option, fetched and SHA-512 checked by `tools/fetch-asio-sdk.ps1`, never
committed) with WASAPI exclusive as the fallback; build with `--preset rig-asio`. The installer ships LICENSE (GPLv3),
the SDK's licence and THIRD-PARTY-NOTICES; every console release carries its source zip.

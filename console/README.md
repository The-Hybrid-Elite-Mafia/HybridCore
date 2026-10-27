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
| TALK LEVEL, talkback XLR, 48V, LMC | — | Left out: needs a talkback microphone |
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

#pragma once
// =============================================================================
//  HEM DIGITAL CONSOLE — every number taken from the reference hardware, in ONE place.
//
//  Each value carries a tag:
//    VERIFIED  printed in the reference unit's own manual / panel drawing (source given)
//    FITTED    measured off the manufacturer's own published graph (fit error given)
//    SOURCED   stated by the same manufacturer in another of its manuals
//    ASSUMED   our working guess — the manual is silent. Change it HERE, nowhere else.
//    UNKNOWN   not in any manual we have; a safe, documented default is used (also ASSUMED)
//
//  Source keys (the documents themselves are named in the private project doc
//  claude/HEM-DIGITAL-CONSOLE-VERIFIED-SPECS.md, not in code):
//    DESK-UG  p.N   reference desk user guide V1.6, printed page N
//    DESK-RS        reference desk recall sheet (DESK-UG p.45, panel drawing)
//    DESK-GR        reference desk EQ response graphs (DESK-UG p.8)
//    EQ-OM    §N    reference 4-band EQ operator's manual (2024-01-17), section N
//    BUS-UG   p.N   reference bus processor user guide rev 1.1, printed page N
//    COL-UG   p.N   reference stereo colour processor user guide V1.4.0, printed page N
//    COL-RS         its recall sheet (COL-UG p.29, panel drawing)
//    COL-GR   p.N   its published response / spectrum graphs (traced; fit error given)
//    MTR-PID  p.N   reference desktop meter product information document (11 pages)
//    BS1770 / R128 / T3341 / T3342  ITU-R BS.1770-4, EBU R128, EBU Tech 3341 and 3342 (the standards it follows)
//
//  Rig constants (sample rate, stem count) are NOT here: they live in hybrid/core/Config.hpp.
//  Every engine takes its sample rate from prepare(); nothing below assumes 48 kHz.
// =============================================================================
#include <array>
#include <cstdint>

namespace hc::console::spec {

// ---- shared ---------------------------------------------------------------------------------
// Converter alignment used to turn the panels' dBu markings into digital full scale.
inline constexpr float kDbuAtFullScale = 24.0f;       // VERIFIED DESK-UG intro "Converter alignment levels" + p.43: "0 dBFS = +24 dBu"
inline constexpr float kBusDbuAtFullScale = 24.0f;    // ASSUMED: bus processor is analogue; we align it like the desk

// =============================================================================================
//  STUDIO DESK (13 channels; each one carries the union of the mono and stereo strip controls)
// =============================================================================================
namespace desk {
// Channel input
inline constexpr float kTrimMinDb = -10.0f, kTrimMaxDb = 20.0f;      // VERIFIED DESK-UG p.11, DESK-RS
inline constexpr float kMonoSumGain = 0.5f;                            // ASSUMED: MONO = (L+R)/2 (level law not stated)
// High-pass filter
inline constexpr float kHpfHz = 75.0f;                                 // VERIFIED DESK-UG p.7 "switched 12 dB/oct, 75 Hz"
inline constexpr float kHpfQ  = 0.7071f;                               // ASSUMED: Butterworth (12 dB/oct order VERIFIED, shape not)
// Channel EQ: knob range ±15 dB VERIFIED DESK-UG p.8/p.11 (shelves); MF reaches the same ±15 on DESK-GR.
inline constexpr float kEqKnobMaxDb = 15.0f;
// Shapes FITTED to DESK-GR with the analogue prototypes in hybrid/console/Filters.hpp.
// Knob -> gain law between the end stops is ASSUMED linear in dB (the curves only show full boost/cut).
struct Band { float hz, q, gainAtFullKnobDb; };
inline constexpr Band kHfShelf{1679.24f, 0.4884f, 15.4981f};   // FITTED rms 0.26 dB, max 0.69 dB (panel label 3.5 kHz)
inline constexpr Band kLfShelf{ 168.12f, 0.5283f, 15.1791f};   // FITTED rms 0.41 dB, max 0.97 dB (panel label 60 Hz)
inline constexpr Band kMfBell { 708.13f, 0.1437f, 14.7419f};   // FITTED rms 0.05 dB, max 0.14 dB (panel label 700 Hz)
inline constexpr Band kHfBell {5720.56f, 0.1962f, 14.9856f};   // FITTED rms 0.49 dB, max 1.22 dB (BELL moves HF to "5 kHz", DESK-UG p.8)
inline constexpr Band kLfBell { 163.70f, 0.1915f, 14.1189f};   // FITTED rms 0.28 dB, max 0.65 dB (BELL moves LF to "200 Hz", DESK-UG p.8)
// Cuts: DESK-GR shows the cut curves as mirror images of the boosts -> reciprocal (exact inverse) filters.
// Stereo channels have no BELL switches on the reference (DESK-RS); our 13 strips all carry them by owner decision.

// Channel compressor
inline constexpr float kChThreshMaxDbu = 10.0f, kChThreshMinDbu = -20.0f;   // VERIFIED DESK-UG p.9, DESK-RS
inline constexpr float kChRatio = 2.0f;                                       // VERIFIED DESK-UG p.9
inline constexpr float kChAttackFastMs = 8.0f, kChAttackSlowMs = 30.0f;      // VERIFIED DESK-UG p.9 "program dependent ~8..30 ms"
inline constexpr float kChAttackSpanDb = 12.0f;   // ASSUMED: attack moves from 30 ms (just over) to 8 ms (12 dB+ over threshold)
inline constexpr float kChReleaseMs = 300.0f;                                 // VERIFIED DESK-UG p.9
inline constexpr float kChKneeDb = 6.0f;          // UNKNOWN ("slightly softened", DESK-UG p.9) -> ASSUMED 6 dB
inline constexpr float kChAutoMakeupRefDbu = 10.0f;   // UNKNOWN amount of auto make-up -> ASSUMED: a signal at +10 dBu
                                                      // (top of the threshold range) keeps its level at any threshold
inline constexpr std::array<float, 3> kChGrLedDb{3.0f, 6.0f, 10.0f};   // UNKNOWN (3 LEDs, values not printed) -> ASSUMED
inline constexpr bool kChStereoLinked = true;     // ASSUMED: reference channels are mono; our stereo stems use one linked detector

// Pan / balance
inline constexpr float kPanCentreDb = -4.5f;      // VERIFIED DESK-UG p.10 (pan and cue pan centre law)
// Balance law on a stereo signal: UNKNOWN -> ASSUMED: centre = both sides unity, turning one way fades
// the other side with the same curve as the pan law (normalised so centre is 0 dB).

// Faders / levels
inline constexpr float kFaderMaxDb = 10.0f;       // VERIFIED DESK-RS fader scale (+10 .. ∞)
inline constexpr float kOffDb = -80.0f;           // our "∞" (fully down) — treated as silence
inline constexpr float kCueSendMaxDb = 0.0f;      // VERIFIED DESK-UG p.10 "unity when fully clockwise"
inline constexpr float kCueMasterMaxDb = 10.0f;   // VERIFIED DESK-RS cue master scale (∞ .. +10)
inline constexpr float kBusBMasterMaxDb = 10.0f;  // VERIFIED DESK-RS (∞ .. +10)
inline constexpr float kExtLevelMaxDb = 20.0f;    // VERIFIED DESK-UG p.13 "off .. +20 dB, detent at unity"

// Meters (LED marks in dBu, VERIFIED DESK-RS)
inline constexpr std::array<float, 8>  kChannelMeterDbu{24, 18, 12, 6, 0, -6, -15, -21};
inline constexpr std::array<float, 12> kMainMeterDbu{24, 21, 18, 15, 12, 9, 6, 0, -4, -9, -15, -21};
inline constexpr float kMeterReleaseDbPerSec = 26.0f;   // UNKNOWN ("slower release") -> ASSUMED

// Monitor
inline constexpr float kDimMinDb = -30.0f, kDimMaxDb = -3.0f;   // VERIFIED DESK-UG p.12, DESK-RS
inline constexpr float kMonoMonitorGain = 0.70795f;             // VERIFIED "-3 dB level drop" DESK-UG p.12; applied to (L+R) — ASSUMED reading
inline constexpr float kMonitorKnobMax = 11.0f;                 // VERIFIED DESK-UG p.12 "0 = off, 11 = unity"
inline constexpr float kMonitorTaperDb = 40.0f;                 // UNKNOWN knob law -> ASSUMED gain dB = 40*log10(v/11)

// Bus compressor
inline constexpr float kBusThreshMinDb = -20.0f, kBusThreshMaxDb = 20.0f;   // VERIFIED DESK-RS
inline constexpr float kBusThreshRefDbu = 0.0f;   // UNKNOWN reference of the threshold scale -> ASSUMED knob dB = dBu
inline constexpr float kBusRatio = 4.0f;                                     // VERIFIED DESK-UG p.19
inline constexpr float kBusAttackMs = 30.0f, kBusReleaseMs = 100.0f;         // VERIFIED DESK-UG p.19
inline constexpr float kBusAutoFastMs = 100.0f, kBusAutoSlowMs = 12000.0f;   // SOURCED BUS-UG p.10: AUTO "same as all other" bus comps
inline constexpr float kBusAutoChargeMs = 1500.0f; // UNKNOWN how fast the slow stage charges -> ASSUMED
inline constexpr float kBusMakeupMaxDb = 20.0f;    // VERIFIED DESK-RS (0 .. +20)
inline constexpr float kBusScHpfHz = 50.0f;        // VERIFIED DESK-UG p.19 "1st order HPF at approx 50 Hz"
inline constexpr float kBusKneeDb = 2.0f;          // UNKNOWN -> ASSUMED
inline constexpr std::array<float, 5> kBusGrLedDb{1, 3, 6, 9, 15};   // VERIFIED DESK-UG p.19, DESK-RS
} // namespace desk

// =============================================================================================
//  STEP EQ (2 channels × 4 stepped bands)
// =============================================================================================
namespace stepeq {
inline constexpr std::array<float, 7> kLoHz  {30, 40, 50, 100, 200, 300, 400};                  // VERIFIED EQ-OM §2.2.3
inline constexpr std::array<float, 7> kLoMidHz{75, 150, 180, 240, 500, 700, 1000};              // VERIFIED
inline constexpr std::array<float, 7> kHiMidHz{800, 1500, 3000, 5000, 8000, 10000, 12500};      // VERIFIED
inline constexpr std::array<float, 7> kHiHz  {2500, 5000, 7000, 10000, 12500, 15000, 20000};    // VERIFIED
inline constexpr std::array<float, 11> kGainDb{-12, -9, -6, -4, -2, 0, 2, 4, 6, 9, 12};         // VERIFIED EQ-OM §2.2.4
inline constexpr std::array<float, 3> kRange{1.0f, 0.5f, 0.25f};                                 // VERIFIED EQ-OM §2.2.4.1
// Proportional Q: VERIFIED behaviour (EQ-OM §1.3: broad at small boosts, narrower at big ones; the Q follows the
// EFFECTIVE gain, so 6 dB at x1 = 12 dB at x.5, EQ-OM §2.2.4.1). Numbers UNKNOWN -> ASSUMED Q = a + b·|effective dB|.
inline constexpr float kQAtZero = 0.45f, kQPerDb = 0.08f;                                        // ASSUMED (Q 0.61 @2 dB .. 1.41 @12 dB)
// Shelves "reach the indicated amplitude at the indicated frequency" (VERIFIED EQ-OM §2.2.3.1).
// ASSUMED shape: 2nd-order shelf (Q 0.7071) whose half-gain point sits kShelfOctaves inside the marked frequency.
inline constexpr float kShelfQ = 0.7071f, kShelfOctaves = 1.5f;
// Output stage colour (discrete op-amps + transformer): UNKNOWN, not modelled (no control on the panel).
} // namespace stepeq

// =============================================================================================
//  TWIN BUS (2-channel bus compressor + 2-band dynamic EQ)
// =============================================================================================
namespace twinbus {
// Compressor (VERIFIED BUS-UG p.10 + front-panel drawing BUS-UG p.7)
inline constexpr float kThreshMinDb = -20.0f, kThreshMaxDb = 20.0f;   // 31 positions
inline constexpr int   kThreshSteps = 31;
inline constexpr std::array<float, 11> kAttackMs{0.1f, 0.3f, 0.6f, 1, 3, 6, 10, 15, 20, 30, 40};
inline constexpr std::array<float, 11> kRatio{1.3f, 1.5f, 2, 3, 4, 5, 10, 20, -2.5f, -1.5f, -0.5f};
inline constexpr std::array<float, 9>  kReleaseS{0.05f, 0.1f, 0.15f, 0.2f, 0.3f, 0.4f, 0.6f, 0.8f, 1.2f};   // + AUTO, AUTO 2
inline constexpr float kAuto1FastMs = 100.0f, kAuto1SlowMs = 12000.0f;   // VERIFIED BUS-UG p.10
inline constexpr float kAuto2FastMs = 50.0f,  kAuto2SlowMs = 6000.0f;    // VERIFIED BUS-UG p.10
inline constexpr float kAutoChargeMs = 1500.0f;                           // UNKNOWN -> ASSUMED (same as the desk)
inline constexpr float kMakeupMinDb = -10.0f, kMakeupMaxDb = 20.0f;       // VERIFIED 31 positions, 1 dB
inline constexpr float kMakeupFineMinDb = -5.0f, kMakeupFineMaxDb = 10.0f;// VERIFIED FINE: 0.5 dB steps
inline constexpr float kScHpfMaxHz = 300.0f;                              // VERIFIED OUT, then 10..300 Hz in 10 Hz steps, 12 dB/oct
inline constexpr float kScHpfQ = 0.7071f;                                 // ASSUMED Butterworth
inline constexpr float kKneeDb = 3.0f;                                    // UNKNOWN -> ASSUMED
inline constexpr float kSumScGain = 0.5f;                                 // ASSUMED: Σ S/C detector = |L+R|/2
// LOW THD: VERIFIED purpose (side-chain change that limits LF distortion, BUS-UG p.11); circuit UNKNOWN.
// ASSUMED model: detector peak-hold long enough to ride over one low-frequency cycle, so the gain stops
// following the waveform.
inline constexpr float kLowThdHoldMs = 25.0f;
// GRIT (the colour switch): VERIFIED 9 levels, default 5, "variable harmonic distortion via the VCA" (BUS-UG p.12).
// Amount per level UNKNOWN -> ASSUMED polynomial (even + odd) growing linearly with the level.
inline constexpr int   kGritLevels = 9, kGritDefault = 5;
inline constexpr float kGrit2ndPerLevel = 0.012f, kGrit3rdPerLevel = 0.006f;
// Input trim (settings): VERIFIED 0 dB (default), +10 dB, -5 dB (BUS-UG p.21)
inline constexpr std::array<float, 3> kInputTrimDb{0.0f, 10.0f, -5.0f};
// Input low-pass (settings): VERIFIED 26 kHz, 18 dB/oct (3rd order). Only acts when it is below Nyquist.
inline constexpr float kLpfHz = 26000.0f;
// Overload: VERIFIED flashes above +26.5 dBu (BUS-UG p.9) -> in our alignment:
inline constexpr float kOverloadDbfs = 26.5f - kBusDbuAtFullScale;

// D-EQ (VERIFIED BUS-UG p.14-19 and the dial drawings p.18)
inline constexpr std::array<float, 16> kLfHz{20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160, 170};
inline constexpr std::array<float, 16> kHfShelfHz{2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 11000, 12000, 13000, 14000, 15000, 16000, 17000};
inline constexpr std::array<float, 16> kHfBellHz{700, 1200, 1600, 2000, 2500, 3000, 3500, 4000, 4500, 6000, 7000, 8000, 10000, 12000, 14000, 16000};
inline constexpr int kLfDefault = 4, kHfShelfDefault = 4, kHfBellDefault = 7;   // 60 Hz, 6 kHz, 4 kHz VERIFIED
inline constexpr int kRangeSteps = 23;    // 0.5..8 dB by 0.5, then 9..15 by 1 (VERIFIED p.19)
inline constexpr float kStaticGainMaxDb = 10.0f;   // LF/HF GAIN ±10 dB, 31 positions VERIFIED p.15
// The LF GAIN knob is printed 0, ±1.5, ±4.5, ±10 (VERIFIED, front-panel drawing p.7): finer near 0.
// Which of the 15 steps each side sit on those marks is UNKNOWN -> ASSUMED steps 4, 9, 15 (straight lines between).
inline constexpr std::array<float, 4> kStaticGainMarkSteps{0, 4, 9, 15};
inline constexpr std::array<float, 4> kStaticGainMarkDb{0.0f, 1.5f, 4.5f, 10.0f};
inline constexpr int kCompExpSteps = 31;           // VERIFIED p.14: COMP .. 0 .. EXP
inline constexpr float kHfAttackMs = 3, kHfReleaseMs = 50, kHfFastAttackMs = 1, kHfFastReleaseMs = 50;   // VERIFIED p.15
inline constexpr float kLfAttackMs = 30, kLfReleaseMs = 100, kLfFastAttackMs = 10, kLfFastReleaseMs = 50;  // VERIFIED p.15
inline constexpr float kAutoAttackMs = 10;         // VERIFIED p.15 ("10 ms attack, AUTO release"; which AUTO: UNKNOWN -> AUTO 1)
// Bell Q: VERIFIED proportional, 1.04 at 5 dB, 1.87 at 10 dB (p.18). Below 5 dB: ASSUMED same line, floor kBellQMin.
inline constexpr float kBellQAt5 = 1.04f, kBellQAt10 = 1.87f, kBellQMin = 0.5f;
// Shelves are 1st order (VERIFIED p.15); the dial frequency is taken as the filter corner (ASSUMED meaning).
// RESO (2nd-order LF with overshoot/undershoot, VERIFIED p.16): amount UNKNOWN -> ASSUMED resonance Q.
inline constexpr float kResoQ = 1.0f;
// Dynamic law: the COMP/EXP knob lowers the threshold (VERIFIED p.14). Threshold travel and slope UNKNOWN -> ASSUMED:
inline constexpr float kDeqThreshTopDbu = 20.0f;      // knob at the first step off centre
inline constexpr float kDeqThreshTravelDb = 40.0f;    // knob fully over: 40 dB lower
inline constexpr float kDeqSlope = 0.5f;              // dB of band change per dB over threshold (2:1 comp / 1:1.5 exp)
// Transient expander (hidden bonus mode, VERIFIED exists and replaces the HF band, p.27). Algorithm UNKNOWN -> ASSUMED:
inline constexpr float kTeFastAttackMs = 1, kTeFastReleaseMs = 20, kTeSlowAttackMs = 20, kTeSlowReleaseMs = 200;
inline constexpr float kTeMaxBoostDb = 12.0f;
} // namespace twinbus

// =============================================================================================
//  COLOUR STAGE (stereo colour processor: drive, shelving EQ, HF compressor / crush, M/S image,
//  transformer, insert, trims, bypass, meter)
// =============================================================================================
namespace colour {
inline constexpr float kTrimDb = 12.0f;                         // VERIFIED ±12 dB input and output trim (COL-UG p.8, p.14)
inline constexpr std::array<float, 3> kHpfHz{30, 40, 50};       // VERIFIED OFF/30/40/50, 3rd order 18 dB/oct (COL-UG p.8)
// FITTED COL-GR p.8: the 50 Hz curve is a 3rd-order Butterworth with its -3 dB point at 53.1 Hz (rms 0.12 dB).
// The 30/40 Hz traces were too noisy to fit; all three use the marked value as the Butterworth -3 dB point.
inline constexpr float kOverloadDbu = 27.0f;                    // VERIFIED O/L LEDs above +27 dBu (COL-UG p.8)
inline constexpr float kDbuAtFullScale = 24.0f;                 // ASSUMED converter alignment (as the desk)
// Master meter: VERIFIED dBu scale, +24 at the top, 3 s peak hold, shows the input in BYPASS (COL-UG p.15);
// segment marks VERIFIED COL-RS.
inline constexpr std::array<float, 15> kMeterDbu{24, 21, 18, 15, 12, 9, 6, 4, 0, -4, -6, -12, -15, -18, -21};
inline constexpr float kMeterHoldSeconds = 3.0f;

// DRIVE: DRIVE 1..11 (VERIFIED COL-RS), DENSITY MIN..MAX (VERIFIED, printed MIN 2 4 6 8 MAX = 0..10 here),
// auto level compensation for the drive (VERIFIED COL-UG p.9), density raises level (VERIFIED, amount UNKNOWN).
inline constexpr float kDriveDbPerStep = 2.0f;        // ASSUMED: drive 1 = +0 dB into the saturator .. 11 = +20 dB
inline constexpr float kDensityLevelDb = 3.0f;        // ASSUMED: MIN -> MAX adds up to +3 dB
// Saturator y = v + e·v², v = soft-knee(u, T): FITTED to COL-GR p.10 (1 kHz spectra) with the test conditions
// ASSUMED as drive 5 and a -10 dBFS tone: "low" density -> H2 -27.5 dBc, H3 -38.0 dBc; "high" -> H2 -64.2, H3 -54.8.
inline constexpr float kDensityLowAt = 2.0f, kDensityHighAt = 8.0f;   // ASSUMED: which knob values the plots used
inline constexpr float kKneeLow = 0.5072f, kEvenLow = 0.11086f;       // FITTED (exact match to the two harmonics)
inline constexpr float kKneeHigh = 0.6739f, kEvenHigh = 0.00156f;     // FITTED (exact match)
inline constexpr float kKneeMin = 0.30f, kKneeMax = 0.80f, kEvenMax = 0.16f;   // ASSUMED clamps outside the anchors
// MIN density: "dynamic expansion effect when driven hard" (VERIFIED p.9). Amount UNKNOWN -> ASSUMED.
inline constexpr float kMinExpansionDb = 4.0f;        // up to +4 dB on peaks at MIN (fades out by density 1)
// LED: VERIFIED THD bands (COL-UG Appendix B): green 0.2-0.5 %, orange 0.5-2 %, red > 2 %.
inline constexpr float kLedGreenThd = 0.2f, kLedOrangeThd = 0.5f, kLedRedThd = 2.0f;
// Hiss: VERIFIED -75 dBu typical (mid drive, mid density), falls as DRIVE / DENSITY rise (p.9; slope UNKNOWN -> ASSUMED).
inline constexpr float kHissDbu = -75.0f, kHissFallDb = 6.0f;

// TONE EQ: VERIFIED LOW 30/50/70/90 Hz, HIGH 8/12/16/20 kHz, gain ±9 dB continuous, minimum-phase shelves (p.11).
// FITTED COL-GR p.11: 1st-order shelves; geometric mid-point (Hz) of each and full depth (dB) at the ±9 end stop.
inline constexpr std::array<float, 4> kLowMidHz{121.84f, 195.19f, 327.55f, 568.60f};     // rms 0.020/0.022/0.024/0.188 dB
inline constexpr std::array<float, 4> kHighMidHz{2680.18f, 3859.89f, 5337.06f, 6290.59f}; // rms 0.052/0.041/0.028/0.024 dB
inline constexpr float kToneDepthAtNineDb = 9.95f;   // FITTED (fits 9.82..10.18 dB; mean used), knob law ASSUMED linear

// HF COMPRESSOR: VERIFIED THRESHOLD +10..-10 dB, X-OVER 3..20 kHz, fixed attack/release/ratio, no make-up (p.12).
inline constexpr float kHfcXoverMinHz = 3000.0f, kHfcXoverMaxHz = 20000.0f;
inline constexpr float kHfcThresholdRefDbu = 0.0f;   // UNKNOWN reference -> ASSUMED knob dB = dBu
inline constexpr float kHfcRatio = 3.0f, kHfcAttackMs = 1.0f, kHfcReleaseMs = 100.0f, kHfcKneeDb = 3.0f;   // UNKNOWN -> ASSUMED
inline constexpr float kHfcXoverQ = 0.7071f;          // UNKNOWN band split -> ASSUMED 2nd-order high-pass detector + dynamic shelf
inline constexpr std::array<float, 3> kHfcLedDb{0.5f, 3.0f, 6.0f};   // UNKNOWN LED points -> ASSUMED
// Crush mode (hold IN 5 s; X-OVER becomes WET/DRY): VERIFIED (p.12). Curves UNKNOWN -> ASSUMED:
inline constexpr float kCrushRatio = 10.0f, kCrushAttackMs = 0.5f, kCrushReleaseMs = 250.0f, kCrushKneeDb = 2.0f;
inline constexpr float kCrushMakeupRefDbu = 10.0f;     // ASSUMED: make-up keeps a +10 dBu signal level (so WET/DRY blends sensibly)

// STEREO IMAGE: VERIFIED WIDTH = side level ±6 dB, SPACE = broad bass boost/cut of the side ±12 dB (p.12, COL-RS).
inline constexpr float kWidthDb = 6.0f, kSpaceDb = 12.0f;
inline constexpr float kSpaceHz = 600.0f;            // UNKNOWN ("stereo shuffling" shelf) -> ASSUMED 1st-order shelf mid-point

// TRANSFORMER: VERIFIED 600 Ω 1:1; LF harmonics, slight top-end phase shift, under-damping weight; more level =
// more LF roll-off; +16 dBu gives about 0.5 dB loss between 40 and 30 Hz (p.13).
// FITTED COL-GR p.13 (+16 dBu): 1st-order high-pass at 31.88 Hz plus an under-damped bump (bell 28.58 Hz, Q 0.778,
// +2.37 dB) — rms 0.08 dB, max 0.19 dB over 9-200 Hz. (A plain 2nd-order high-pass fits 5x worse: rms 0.40 dB.)
inline constexpr float kTxHpHzAt16Dbu = 31.88f;
inline constexpr float kTxBumpHz = 28.58f, kTxBumpQ = 0.778f, kTxBumpDb = 2.37f;
inline constexpr float kTxHpDbPerDecadeOfLevel = 20.0f;   // ASSUMED: the corner moves x10 per 40 dB of level (√ law)
inline constexpr float kTxLfBandHz = 150.0f, kTxHarmonicAt16Dbu = 0.0015f;   // ASSUMED: ~0.1 % LF 2nd/3rd at +16 dBu
inline constexpr float kTxAllpassHz = 23000.0f;           // ASSUMED top-end phase shift (1st-order all-pass, flat level)
inline constexpr float kTxSparkleDb = 0.25f, kTxSparkleHz = 12000.0f;   // ASSUMED "touch of sparkle" (1st-order shelf)
} // namespace colour

// =============================================================================================
//  METER BRIDGE (stereo loudness / true-peak / stereo / RTA meter). VERIFIED (MTR-PID p.1-5): loudness radar,
//  true-peak meter, RTA in 1/3 octaves, vectorscope, Balance-O-Meter (left/right and mid/side), correlation meter,
//  downmix (mono) deviation, loudness statistics, compliance with BS.1770-4 / EBU R128 / ATSC A/85.
//  The measurement maths below is the standards' (VERIFIED by the standards themselves); the display options of
//  the reference unit (presets, radar speed, scales, averaging) are UNKNOWN until its user manual is read.
// =============================================================================================
namespace meter {
inline constexpr double kMomentarySeconds = 0.4, kShortTermSeconds = 3.0, kStepSeconds = 0.1;   // VERIFIED BS1770 / R128
inline constexpr double kAbsGateLufs = -70.0, kRelGateLu = -10.0;          // VERIFIED BS1770 (integrated)
inline constexpr double kLraRelGateLu = -20.0, kLraLow = 0.10, kLraHigh = 0.95;   // VERIFIED T3342
inline constexpr double kHistogramStepLu = 0.01, kHistogramTopLufs = 10.0; // our resolution (exact energies kept per bin)
inline constexpr double kCorrelationSeconds = 0.3;   // UNKNOWN integration time -> ASSUMED
inline constexpr double kRtaAverageSeconds = 0.3;    // UNKNOWN RTA speed -> ASSUMED
inline constexpr double kRtaPeakHoldSeconds = 2.0;   // UNKNOWN -> ASSUMED
inline constexpr double kRtaFilterQ = 2.78;          // ours: two cascaded band-passes give a 1/3-octave -3 dB width
inline constexpr bool kRtaMonoSum = true;            // UNKNOWN (sum or per channel) -> ASSUMED (L+R)/2
// Balance and mono deviation are measured on K-weighted signals over the short-term (3 s) window: ASSUMED definitions
// (MTR-PID only says they are based on the maker's loudness technology).
} // namespace meter

// =============================================================================================
//  FRONT-END TOOLS (the slots where the reference desk has microphone preamps). OUR OWN DESIGNS, built for the
//  engineer's in-computer front end as described (two resonance suppressors in tandem, a "shuffle" for mud, a delay
//  with tap tempo). No hardware to match: every number here is a HEM design choice, labelled OWN.
// =============================================================================================
namespace tools {
// RESO TAMER: two dynamic notches. Each band compares a narrow band-pass (Q = WIDTH knob) with a wide one (Q 0.5)
// around the same centre; when the narrow band pokes out above its usual share by more than THRESHOLD, the band
// cuts it back by up to DEPTH (y = x + (g - 1)·bp(x), a bell cut with gain g at the centre).
inline constexpr float kResoWideQ = 0.5f;                        // OWN
inline constexpr float kResoAttackMs[3] = {2.0f, 6.0f, 15.0f};   // OWN: SPEED FAST / MEDIUM / SLOW
inline constexpr float kResoReleaseMs[3] = {40.0f, 120.0f, 300.0f};
inline constexpr float kResoDetectFloorDb = -80.0f;              // OWN: below this the detector does nothing (silence)
// SHUFFLE: mid/side tool. Side low shelf (shuffling) at SHUFFLE FREQ, mid bell cut for mud, side width, mono bass.
inline constexpr float kShuffleMudQ = 1.4f;                      // OWN
inline constexpr float kShuffleMonoBassHz = 120.0f;              // OWN (2nd-order high-pass on the side)
inline constexpr float kShuffleCorrelationMs = 300.0f;           // OWN
// TAP ECHO: delay with tap tempo.
inline constexpr float kEchoMaxSeconds = 4.0f;                   // OWN longest delay (longer note values clamp to it)
inline constexpr float kEchoTimeGlideMs = 60.0f;                 // OWN: time changes glide like tape instead of clicking
inline constexpr double kTapResetSeconds = 2.0;                  // OWN: a pause longer than this starts a new tap count
inline constexpr int kTapAverage = 4;                            // OWN: tempo = mean of the last 4 intervals
} // namespace tools

} // namespace hc::console::spec

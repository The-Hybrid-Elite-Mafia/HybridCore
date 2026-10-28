#pragma once
// =============================================================================
//  HEM HISS TAMER — the ONE place for every working guess.
//
//  Rules for this file:
//    * Every number below is either VERIFIED (a named test proves it) or ASSUMED
//      (a working guess that nobody has measured against real Suno files yet).
//    * Anything nobody knows is listed as UNKNOWN at the bottom. No number is
//      invented for an UNKNOWN.
//    * python/hiss_report.py reads the "// ASSUMED" tags and the UNKNOWN block in this file to
//      build the report. Keep the tag on the same line as the value.
//    * The Python check (python/hiss_compare.py) measures results on its own; it does not read
//      these numbers, and it does not re-implement the engine.
// =============================================================================
#include <cstdint>

namespace hc::hiss::cfg {

// ---- STFT frame ------------------------------------------------------------------
inline constexpr int    kFftNormal   = 2048;      // ASSUMED: 2048 points up to 64 kHz (about 23 Hz per bin at 48 kHz)
inline constexpr int    kFftHigh     = 4096;      // ASSUMED: 4096 points above 64 kHz so a bin stays about as wide
inline constexpr double kHighRateHz  = 64000.0;   // ASSUMED: sample rate where the longer frame starts
inline constexpr int    kHopDivisor  = 4;         // VERIFIED: perfect_reconstruction test (sqrt-Hann at 75 % overlap adds up flat)

// ---- Where the focus can go ------------------------------------------------------
inline constexpr double kFocusMinHz     = 4000.0;     // ASSUMED: lowest centre the focus control offers
inline constexpr double kFocusMaxHz     = 18000.0;    // ASSUMED: highest centre the focus control offers
inline constexpr double kFocusDefaultHz = 11750.0;    // ASSUMED: middle of the 11.25-12.25 kHz range Steven named
inline constexpr double kWidthMinHz     = 200.0;      // ASSUMED: narrowest focus (flat part)
inline constexpr double kWidthMaxHz     = 6000.0;     // ASSUMED: widest focus (flat part)
inline constexpr double kWidthDefaultHz = 1000.0;     // ASSUMED: covers 11.25-12.25 kHz
inline constexpr double kSkirtHz        = 600.0;      // ASSUMED: soft edge on each side of the flat part (raised cosine)
inline constexpr double kTrackFromHz    = 2000.0;     // ASSUMED: the detector watches everything above this so moving the focus needs no re-learning

// ---- Controls --------------------------------------------------------------------
inline constexpr float  kMaxReductionDb     = 18.0f;  // ASSUMED: hard safety cap on the cut
inline constexpr float  kDefaultReductionDb = 6.0f;   // ASSUMED: conservative starting cut
inline constexpr float  kDefaultSensitivity = 0.5f;   // ASSUMED: middle
inline constexpr float  kMinSteadySeconds   = 0.25f;  // ASSUMED: shortest memory the percentile / tonal detectors use
inline constexpr float  kMaxSteadySeconds   = 2.0f;   // ASSUMED: longest memory (for slow pulsing)
inline constexpr float  kDefaultSteadySeconds = 0.5f; // ASSUMED

// ---- Detector --------------------------------------------------------------------
inline constexpr float  kPowerSmoothMs    = 40.0f;    // ASSUMED: S = a * S_old + (1 - a) * P with a = exp(-hop / 40 ms), about 0.77 per hop at 48 kHz (slower than HEM HUSH: it cut the watery score from 1.7 to 1.1 in tests)
inline constexpr float  kMinSmoothMs      = 10.0f;    // ASSUMED: fastest power smoothing the control allows
inline constexpr float  kMaxSmoothMs      = 600.0f;   // ASSUMED: slowest (for flutter, so the level between pulses is seen as one steady level)
inline constexpr int    kNeighbourMedian  = 13;       // ASSUMED: bins in the "floor" median (odd)
inline constexpr int    kTonalBaseBins    = 25;       // ASSUMED: bins in the tonal detector's local-floor median (odd)
inline constexpr int    kFloorSlots       = 8;        // ASSUMED: floor memory is 8 slots ...
inline constexpr float  kFloorSlotSeconds = 0.5f;     // ASSUMED: ... of half a second each (4 s)
inline constexpr float  kFloorBias        = 1.5f;     // ASSUMED: minimum-of-medians sits below the true mean level; this lifts it back
inline constexpr float  kQuantileLow      = 0.20f;    // ASSUMED: "quiet" quantile of the percentile detector
inline constexpr float  kQuantileHigh     = 0.80f;    // ASSUMED: "loud" quantile used to judge how steady a bin is
inline constexpr float  kQuantileBias     = 1.25f;    // ASSUMED: lifts the 20 % quantile back toward the mean level
inline constexpr float  kSteadyLowBase    = 0.45f;    // ASSUMED: steadiness ratio where confidence starts (at sensitivity 0) ...
inline constexpr float  kSteadyLowSlope   = 0.30f;    // ASSUMED: ... and how far it drops as sensitivity goes to 1
inline constexpr float  kSteadySpan       = 0.25f;    // ASSUMED: confidence goes 0 -> 1 across this much ratio
inline constexpr float  kTonalPromBaseDb  = 6.0f;     // ASSUMED: how far a steady peak must stand above its neighbours (sensitivity 0) ...
inline constexpr float  kTonalPromSlopeDb = 3.0f;     // ASSUMED: ... and how much easier it gets at sensitivity 1
inline constexpr float  kTonalHoldLow     = 0.30f;    // ASSUMED: a peak whose quietest moment is below 0.3 x its median is not steady
inline constexpr float  kTonalHoldSpan    = 0.40f;    // ASSUMED
inline constexpr float  kRampLowBase      = 2.5f;     // ASSUMED: a bin counts as "just hiss" when its level is below this many times the steady floor (sensitivity 0) ...
inline constexpr float  kRampLowSlope     = 1.5f;     // ASSUMED: ... plus this x sensitivity
inline constexpr float  kRampSpan         = 4.0f;     // ASSUMED: the cut fades out between that level and this many times higher
inline constexpr float  kWarmupSeconds    = 1.0f;     // ASSUMED: cut fades in over the first second so a song start is never cut before the floor is known

// ---- Transient guard -------------------------------------------------------------
inline constexpr float  kGuardThresholdBase = 2.0f;   // ASSUMED: band energy must jump to this many times its running average (sensitivity 1) ...
inline constexpr float  kGuardThresholdSlope= 2.0f;   // ASSUMED: ... plus this x (1 - sensitivity)
inline constexpr float  kGuardRiseMs        = 400.0f; // ASSUMED: running average climbs slowly (so a hit stays "a hit" through its decay)
inline constexpr float  kGuardFallMs        = 100.0f; // ASSUMED: ... and comes back down faster
inline constexpr float  kGuardHoldMs        = 200.0f; // ASSUMED: guard fades out over this long after a hit

// ---- Gain smoothing ---------------------------------------------------------------
inline constexpr float  kGainFallMs   = 80.0f;        // ASSUMED: gain goes down slowly (no pumping, no watery sound) ...
inline constexpr float  kGainRise     = 0.70f;        // ASSUMED: ... and comes back up fast (share of the gap closed per frame)
inline constexpr float  kBypassMs     = 10.0f;        // ASSUMED: click-free fade for Bypass and Mix changes

// ---- Auto brain: how it looks ------------------------------------------------------
inline constexpr double kSearchLowHz       = 8000.0;  // ASSUMED: where AUTO FIX looks for the artifact ...
inline constexpr double kSearchHighHz      = 16000.0; // ASSUMED: ... from here to here
inline constexpr int    kScanFft           = 8192;    // ASSUMED: long FFT for the scan (about 6 Hz per bin at 48 kHz)
inline constexpr float  kScanFloorQuantile = 0.10f;   // ASSUMED: "what is always there" = the 10th-percentile spectrum
inline constexpr double kBaselineWindowHz  = 3000.0;  // ASSUMED: running-median window that defines "normal" around a bump
inline constexpr double kMinBumpDb         = 3.0;     // ASSUMED: smaller bump than this = "no clear artifact", file left alone
inline constexpr double kTonalMaxWidthHz   = 80.0;   // ASSUMED: a bump narrower than this is called a tone
inline constexpr double kTonalMinOverDb    = 6.0;     // ASSUMED: ... and it must stand this far above the smoothed floor
inline constexpr double kPulseMinHz        = 2.0;     // ASSUMED: slowest flutter looked for
inline constexpr double kPulseMaxHz        = 40.0;    // ASSUMED: fastest flutter looked for
inline constexpr double kPulseMinProminenceDb = 8.0;  // ASSUMED: flutter line must stand this far above the rest of the modulation spectrum
inline constexpr double kPulseMinDepthDb   = 1.5;     // ASSUMED: and swing the band level at least this much
inline constexpr double kPulseMaxSkew      = 1.0;     // ASSUMED: a spiky envelope (drum hits) is not flutter
inline constexpr double kMidSideSplitDb    = 4.0;     // ASSUMED: bump must differ this much between mid and side to use Mid/Side mode
inline constexpr double kLeaveDb           = 1.0;     // ASSUMED: AUTO cuts the bump minus this, so the result stays natural
inline constexpr float  kAutoMinReductionDb= 3.0f;    // ASSUMED
inline constexpr float  kAutoMaxReductionDb= 12.0f;   // ASSUMED: AUTO never cuts more than this

// ---- Auto brain: self-check limits (a result that breaks one is backed off) --------
inline constexpr double kMaxPeakLossDb     = 0.5;     // ASSUMED: a cymbal hit may lose at most this much peak level
inline constexpr double kMaxEnvelopeErrDb  = 1.0;     // ASSUMED: RMS gap of a hit's loudness curve (only where the hit is 10 dB above the floor)
inline constexpr double kMaxAttackShiftMs  = 0.5;     // ASSUMED: a hit's rise time may change at most this much
inline constexpr double kMaxOutOfBandDb    = 0.05;    // ASSUMED: spectrum outside the focus may change at most this much
inline constexpr double kMaxMusicalNoise   = 1.5;     // ASSUMED: kurtosis ratio (Uemura-style); 1.0 = same texture as the input
inline constexpr double kBackOffFactor     = 0.5;     // ASSUMED: each back-off step halves the cut
inline constexpr int    kBackOffSteps      = 2;       // ASSUMED: then the file is left untouched
inline constexpr double kMinUsefulDropDb = 1.5;   // ASSUMED: a fix that lowers the always-there level by less than this is not worth touching the file for

// ---- Comb method (hem_hiss_comb_engine) -------------------------------------------------------------
inline constexpr double kCombSpacingHz        = 25.0;     // MEASURED: 90-100 % of the strong lines in 5 Suno stems sit on a 25 Hz grid (chance: 12 %). ASSUMED to hold for other Suno files: UNKNOWN
inline constexpr float  kCombLoDefaultHz      = 3000.0f;  // ASSUMED: lowest line touched by default (lines were seen from about 3 kHz up)
inline constexpr float  kCombHiDefaultHz      = 20000.0f; // ASSUMED: highest line touched by default (the files are low-passed at 20 kHz)
inline constexpr float  kCombAdaptDefaultSeconds = 6.0f;  // ASSUMED: memory of the pattern (about 75 visits of each point of the 80 ms fold)
inline constexpr float  kCombAdaptMinSeconds  = 0.5f;
inline constexpr float  kCombAdaptMaxSeconds  = 20.0f;
inline constexpr double kCombHighPassHz       = 1500.0;   // ASSUMED: 4th-order high-pass in front of the learner, so bass cannot leak in
inline constexpr double kCombPowerSmoothMs    = 5.0;      // ASSUMED: short-term power used to judge how quiet a moment is
inline constexpr double kCombQuietFactor       = 16;       // ASSUMED: a moment counts as "quiet" up to 16 x (12 dB above) the lowest power of the file (offline) or of the last 64 s (live). Tuned on 3 Suno stems only
inline constexpr double kCombPriorWeight      = 1.0;      // ASSUMED: shrinks a pattern point towards 0 until it has been seen often enough
inline constexpr double kCombRefreshSeconds   = 1.0;      // ASSUMED: how often the pattern is rebuilt
inline constexpr double kCombWienerBase       = 3.0;      // ASSUMED: a line must be this many times stronger than the noise between the lines (sensitivity 0) ...
inline constexpr double kCombWienerSlope      = 2.0;      // ... minus this x sensitivity (so 1.0 at sensitivity 1)
inline constexpr double kCombEdgeHz           = 300.0;    // ASSUMED: soft edge of the band
inline constexpr double kCombMaxPatternDb     = -60.0;    // ASSUMED: a learned pattern louder than this (RMS, dBFS) is not believed and is not subtracted: real patterns measured here were about -91 dBFS
inline constexpr double kCombFloorPercentile  = 0.0;       // ASSUMED: offline, the quiet floor is the lowest 0.25 s block of the file above silence (0 = lowest, 0.1 = 10th percentile; 0.1 was tried and was worse on the Drums stem)
inline constexpr double kCombSilencePower     = 1e-10;    // ASSUMED: quieter than this (power, about -100 dBFS: an artifact below that is inaudible anyway) counts as silence: not used to find the quiet floor, and nothing is subtracted there
inline constexpr double kCombAgreeMax         = 0.6;      // ASSUMED: offline learning keeps a line only if every time chunk of the quiet material agrees with the average (each within 0.6 x the average size)
inline constexpr double kCombAgreeMinSeconds  = 0.25;     // ASSUMED: a time chunk needs at least this much quiet material to vote
inline constexpr double kCombMinLines         = 12;       // ASSUMED: AUTO needs at least this many lines on the grid
inline constexpr double kCombMinGridShare     = 0.60;     // ASSUMED: ... and at least this share of all strong lines on the grid (chance level is about 0.12)
inline constexpr double kCombMinFall         = 0.30;     // ASSUMED: AUTO keeps the fix only if at least this share of the on-grid lines is gone afterwards
inline constexpr double kCombMaxLowShare      = 0.001;    // ASSUMED: ... and at most this share of what was taken out may sit below 2.5 kHz
inline constexpr double kCombLineMinDb        = 10.0;     // ASSUMED: a line must stand this far above its neighbourhood to count

// ---- Self-check scoring -------------------------------------------------------------
inline constexpr double kHighConfidenceDropShare = 0.8;  // ASSUMED: "High" needs the floor to fall by at least 80 % of the target cut
inline constexpr double kHighConfidenceBumpDb    = 5.0;  // ASSUMED: ... and the artifact to have been at least this loud
inline constexpr double kLowConfidenceDropShare  = 0.5;  // ASSUMED: below half the target cut the rating is "Low"

// =============================================================================
//  UNKNOWN — nobody has measured these. No numbers are invented for them.
// =============================================================================
//  UNKNOWN: what the 11-12 kHz shimmer in real Suno files really is (steady noise, a tone, or a flutter).
//           Stage 0 (python/hiss_analyze.py) has to be run on real original WAV downloads to find out.
//  UNKNOWN: how much of the shimmer a listener can still hear after the cut. Only blind listening answers that.
//  UNKNOWN: how the self-check limits above compare with what Steven hears. They are guesses.
//  UNKNOWN: whether a steady high-frequency wash that is part of the music can be told apart from hiss.
//           A long open cymbal wash sitting at one level for seconds looks the same as hiss to this detector.
//  UNKNOWN: CPU use on the rig's i7-14700HX. CI numbers come from a GitHub Windows runner, a different machine.
//  UNKNOWN: files where the artifact is outside 8 to 16 kHz, or two artifacts at once. AUTO only searches 8 to 16 kHz and fixes one.
//  UNKNOWN: real Suno files at all. Every test file here is made by our own generator, so the tests prove the engine does what it says on those files, not that it matches real ones.
//  UNKNOWN: anything about 13-stem sessions that sit on top of each other (a cut on one stem may be masked by another).

} // namespace hc::hiss::cfg

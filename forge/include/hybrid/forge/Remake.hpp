#pragma once
// =============================================================================
//  REMAKE — turns one of Steven's own beats (a finished mix, or its stems) back into a
//  Beat Forge song he can redo: tempo, key, chords per bar, the drum groove (kick / snare /
//  hats on the 16th grid, hats also on 32nds), the 808 / bass line, a rough lead melody,
//  and the song form (which tracks play in each bar, intro / verse / hook / outro).
//
//  Everything is measured from the audio on this PC (no network, no AI model):
//    tempo  : spectral-flux onset envelope, autocorrelation comb over 70–160 BPM with a
//             mild preference for the 100–150 range, phase from the kick band
//    key    : time-averaged chroma against the Krumhansl–Kessler major / minor profiles
//    chords : chroma per bar, best diatonic triad of the key
//    drums  : band onsets (kick 35–110 Hz, snare 150–350 Hz + 1.5–5 kHz, hats 7–16 kHz)
//             sampled on the step grid; the groove is the majority over every cycle
//    bass   : 8192-point FFT per step, harmonic sum over MIDI 26–55, snapped to the key
//    lead   : strongest harmonic pitch over MIDI 60–88 where the mids have an onset (rough)
//  Pure function of its input (worker thread; allocates freely; never touches audio state).
// =============================================================================
#include "hybrid/forge/Pattern.hpp"

#include <cstddef>
#include <functional>
#include <string>

namespace hc::forge {

struct RemakeInput {
    double sampleRate = 48000.0;
    // Full mix (left / right; right may equal left). Required unless at least one stem is given.
    const float* mixL = nullptr;
    const float* mixR = nullptr;
    std::size_t  mixFrames = 0;
    // Optional stems (mono or one channel), same rate. Used for their part when present.
    const float* drums = nullptr;  std::size_t drumFrames = 0;
    const float* bass = nullptr;   std::size_t bassFrames = 0;
    const float* music = nullptr;  std::size_t musicFrames = 0;   // keys / melody / everything else
};

struct RemakeResult {
    bool        ok = false;
    std::string error;
    Song        song;               // pattern (bars = the repeating cycle) + arrangement over the whole beat
    double      bpm = 120.0;
    double      firstDownbeat = 0;  // seconds into the file
    int         key = 9;            // 0 = C .. 11 = B
    int         scale = 1;          // index into kScaleNames: 0 MAJOR, 1 MINOR
    std::string chords;             // "Am | F | C | G"
    std::string summary;            // one line for the status bar
    // How sure the analysis is (0..1): tempo comb strength and key-profile correlation.
    double      tempoConfidence = 0, keyConfidence = 0;
};

// progress(0..1) may return false to cancel.
[[nodiscard]] RemakeResult remake(const RemakeInput& in, const std::function<bool(float)>& progress = {});

} // namespace hc::forge

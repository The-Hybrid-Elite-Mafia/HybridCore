#pragma once
// =============================================================================
//  Loudness — integrated loudness (ITU-R BS.1770-4 / EBU R128: K-weighting, 400 ms
//  blocks with 75 % overlap, -70 LUFS absolute gate, -10 LU relative gate) and the
//  export "master" step: raise a finished mix to a target loudness with a look-ahead
//  limiter holding the peaks under a ceiling. Offline only (message / worker thread).
// =============================================================================
#include <cstddef>
#include <vector>

namespace hc::media {

inline constexpr double kSilentLufs = -144.0;   // returned when nothing passes the gates

// Integrated loudness of a stereo signal in LUFS (kSilentLufs for silence / too short).
[[nodiscard]] double integratedLufs(const float* left, const float* right, std::size_t frames, double sampleRate);

struct MasterSettings {
    double targetLufs = -14.0;   // streaming-level loudness
    double ceilingDb  = -1.0;    // sample-peak ceiling after the limiter
    double maxGainDb  = 12.0;    // never raise a quiet mix more than this
};

struct MasterResult {
    double lufsBefore = kSilentLufs;
    double lufsAfter  = kSilentLufs;
    double gainDb     = 0.0;      // gain applied before the limiter (0 = left as it was)
    double limitDb    = 0.0;      // deepest limiter gain reduction, dB (>= 0)
};

// Raises left / right (in place) to the target loudness. A mix already at or above the
// target is left untouched: it is never turned down. Up to 4 passes, because the limiter
// takes a little loudness back each time.
MasterResult masterToLoudness(std::vector<float>& left, std::vector<float>& right, double sampleRate, const MasterSettings& settings = {});

} // namespace hc::media

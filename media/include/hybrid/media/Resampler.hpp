#pragma once
// =============================================================================
//  Resampler — offline sample-rate conversion to the rig rate (any integer rate in).
//
//  Windowed-sinc (Kaiser, designed for 100 dB stopband) evaluated from a 1024-phase
//  table with linear interpolation between phases; zero group delay (output sample m
//  sits exactly at input time m·in/out); exact rational positions, so no drift over
//  long files. Passband edge 0.4535 × the lower rate (20 kHz for 44.1 kHz material),
//  stopband from the lower Nyquist. Same-rate conversion is a plain copy.
//  The inner product is AVX2/FMA. Loader threads only (allocates).
// =============================================================================
#include "hybrid/media/AudioFile.hpp"

#include <cstdint>
#include <vector>

namespace hc::media {

class Resampler {
public:
    Resampler(std::uint32_t inRate, std::uint32_t outRate);

    [[nodiscard]] std::uint64_t outputLength(std::uint64_t inFrames) const noexcept;
    [[nodiscard]] bool          passthrough() const noexcept { return in_ == out_; }
    [[nodiscard]] int           taps() const noexcept { return taps_; }
    [[nodiscard]] double        passbandHz() const noexcept { return passHz_; }
    [[nodiscard]] double        stopbandHz() const noexcept { return stopHz_; }

    // Converts two channels (right may equal left). `outL/outR` hold outputLength(inFrames).
    // Returns false if `progress` cancelled.
    bool process(const float* inL, const float* inR, std::uint64_t inFrames, float* outL, float* outR,
                 const Progress& progress = {}) const;

private:
    std::uint64_t      in_ = 1, out_ = 1;     // reduced ratio
    std::uint32_t      inRate_ = 0, outRate_ = 0;
    int                taps_ = 0;
    double             passHz_ = 0, stopHz_ = 0;
    static constexpr int kPhases = 1024;
    std::vector<float> table_;                // kPhases rows × taps_
    std::vector<float> delta_;                // row p+1 minus row p
};

// Convenience: resamples a DecodedAudio in place to `outRate`.
bool resampleTo(DecodedAudio& audio, std::uint32_t outRate, const Progress& progress = {});

} // namespace hc::media

#pragma once
// Internal: HEM WAVE's wavetables, built once per process by additive synthesis (inverse FFT),
// shared read-only by every synth instance. Each frame is stored at 11 band limits: mip m
// holds harmonics 1 .. 1024 >> m, so a note can always use a table with nothing above Nyquist.
#include <array>
#include <cstddef>
#include <vector>

namespace hc::synth::detail {

inline constexpr int kTables = 8, kFrames = 64, kMips = 11, kMaxHarmonics = 1024;

struct WaveBank {
    std::array<int, kMips> len{}, off{};   // samples per cycle of each mip; offset inside a frame block
    int frameStride = 0;                    // floats per frame (all mips, each with one guard sample)
    std::vector<float> data;

    [[nodiscard]] const float* wave(int table, int frame, int mip) const noexcept {
        return data.data() + static_cast<std::size_t>(table * kFrames + frame) * static_cast<std::size_t>(frameStride) +
               static_cast<std::size_t>(off[static_cast<std::size_t>(mip)]);
    }
    [[nodiscard]] static constexpr int harmonics(int mip) noexcept { return kMaxHarmonics >> mip; }
    // Smallest mip whose highest harmonic stays at or below maxHarmonic (-1: none fits, mute).
    [[nodiscard]] static int mipFor(double maxHarmonic) noexcept {
        if (maxHarmonic < 1.0) return -1;
        for (int m = 0; m < kMips; ++m) if (harmonics(m) <= maxHarmonic) return m;
        return kMips - 1;
    }
    static const WaveBank& get();   // thread-safe, built on first call (~0.1 s)
};

extern const char* const kTableNames[kTables];

} // namespace hc::synth::detail

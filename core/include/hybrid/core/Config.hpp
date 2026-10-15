#pragma once
// =============================================================================
//  Rig constants. Values come from cmake/HybridCorePlatform.cmake definitions.
// =============================================================================
#include <cstddef>
#include <cstdint>

namespace hc {

inline constexpr std::uint32_t kSampleRate     = HC_SAMPLE_RATE;   // 48 kHz, locked
inline constexpr std::uint32_t kStemCount      = HC_STEM_COUNT;    // 13
inline constexpr std::size_t   kCacheLine      = 64;               // Raptor Lake L1/L2 line
inline constexpr std::size_t   kSimdAlign      = 32;               // AVX2 register width
inline constexpr std::uint32_t kMaxChannels    = 64;
inline constexpr std::uint32_t kMaxBlockFrames = 8192;

static_assert(kSampleRate == 48000, "HybridCore is locked to 48 kHz.");

} // namespace hc

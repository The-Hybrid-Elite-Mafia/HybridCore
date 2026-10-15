#pragma once
// =============================================================================
//  THE HYBRID CORE — single source of truth for rig-wide constants.
//  Change sample rate, stem count or engine limits HERE and nowhere else.
//  Nothing in CMake or any other file may redefine these values.
// =============================================================================
#include <cstddef>
#include <cstdint>

namespace hc {

// ---- Audio format -----------------------------------------------------------
inline constexpr std::uint32_t kSampleRate = 48000;   // locked rig rate
inline constexpr std::uint32_t kStemCount  = 13;      // HEM stem matrix width
inline constexpr std::uint32_t kBusCount   = 4;       // stereo buses: 0 = Main, 1..3 = aux

// ---- Engine limits ----------------------------------------------------------
inline constexpr std::uint32_t kMaxChannels    = 64;
inline constexpr std::uint32_t kMaxBlockFrames = 8192;

// ---- Target silicon: Intel Core i7-14700HX (Raptor Lake-HX) -----------------
inline constexpr std::size_t kCacheLine = 64;   // L1/L2 line size
inline constexpr std::size_t kSimdAlign = 32;   // AVX2 register width (no AVX-512 on this part)

static_assert(kSampleRate == 48000, "HybridCore is locked to 48 kHz.");
static_assert(kStemCount > 0 && kStemCount <= kMaxChannels);
static_assert(kBusCount > 0 && kBusCount * 2 <= kMaxChannels);

} // namespace hc

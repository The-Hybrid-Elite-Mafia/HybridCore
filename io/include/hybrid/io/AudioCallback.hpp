#pragma once
// =============================================================================
//  Contract between a hardware stream (WASAPI / ASIO) and the engine.
//  prepare() : message thread, stream stopped — allocation allowed.
//  process() : RT thread — no locks, no allocation, no syscalls that can block.
// =============================================================================
#include <cstdint>

namespace hc::io {

struct StreamConfig {
    std::uint32_t sampleRate      = 0;
    std::uint32_t blockFrames     = 0;
    std::uint32_t numInputs       = 0;
    std::uint32_t numOutputs      = 0;
    double        inputLatencyMs  = 0.0;
    double        outputLatencyMs = 0.0;
};

class AudioCallback {
public:
    virtual ~AudioCallback() = default;
    virtual void prepare(const StreamConfig& config) = 0;
    virtual void process(const float* const* inputs, std::uint32_t numInputs,
                         float* const* outputs, std::uint32_t numOutputs,
                         std::uint32_t frames) noexcept = 0;
};

// Bit flags raised by the RT side (atomic fetch_or), collected by the message thread.
namespace StreamEvent {
inline constexpr std::uint32_t DeviceLost      = 1u << 0;
inline constexpr std::uint32_t ResetRequest    = 1u << 1;
inline constexpr std::uint32_t ResyncRequest   = 1u << 2;
inline constexpr std::uint32_t Overload        = 1u << 3;
inline constexpr std::uint32_t LatencyChanged  = 1u << 4;
inline constexpr std::uint32_t ThreadFault     = 1u << 5;
inline constexpr std::uint32_t SampleRateMoved = 1u << 6;
} // namespace StreamEvent

struct BackendError {
    long        code  = 0;        // HRESULT (WASAPI) or ASIOError (ASIO)
    const char* stage = "";
};

} // namespace hc::io

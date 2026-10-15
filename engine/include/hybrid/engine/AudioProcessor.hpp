#pragma once
// =============================================================================
//  AudioProcessor — anything the engine runs on the RT thread.
//  prepare(): message thread, before the processor is handed to the RT side.
//  process(): RT thread. Must write every output channel for `frames` samples.
// =============================================================================
#include "hybrid/io/AudioCallback.hpp"

#include <cstdint>

namespace hc::engine {

struct ProcessContext {
    const float* const* inputs      = nullptr;
    std::uint32_t       numInputs   = 0;
    float* const*       outputs     = nullptr;
    std::uint32_t       numOutputs  = 0;
    std::uint32_t       frames      = 0;
    std::uint64_t       sampleTime  = 0;   // samples rendered since the stream was prepared
};

class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void prepare(const io::StreamConfig& config) = 0;
    virtual void process(const ProcessContext& ctx) noexcept = 0;
};

} // namespace hc::engine

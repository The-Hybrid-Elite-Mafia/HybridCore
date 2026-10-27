#pragma once
// A plug-in slot behind an INSERT switch on any console engine (Phase 4 tools plug in here).
// Stereo, in place, called on the audio thread: no allocation, no locks, no waiting.
// In a Mid-Side insert the left buffer carries Mid and the right buffer carries Side.
#include <cstdint>

namespace hc::console {

class InsertSlot {
public:
    virtual ~InsertSlot() = default;
    virtual void process(float* left, float* right, std::uint32_t frames) noexcept = 0;
};

} // namespace hc::console

#pragma once
// =============================================================================
//  Real-time thread policy for the i7-14700HX (8 P-cores + 12 E-cores).
//  - MMCSS "Pro Audio", AVRT_PRIORITY_CRITICAL
//  - Soft-pinned to P-core CPU sets (keeps the audio thread off E-cores)
//  - EcoQoS / power throttling disabled
//  - FTZ + DAZ on (denormals never reach the FPU slow path)
//  Construct once at the top of the audio thread, before the loop.
// =============================================================================
#include <cstdint>

namespace hc::rt {

inline constexpr unsigned kMxcsrFtzDaz = 0x8040u;

void enableFlushToZero() noexcept;

// Process-wide: HIGH_PRIORITY_CLASS + process EcoQoS off. Call once from main().
void configureProcessForAudio() noexcept;

class RealtimeThreadScope {
public:
    RealtimeThreadScope() noexcept;
    ~RealtimeThreadScope();
    RealtimeThreadScope(const RealtimeThreadScope&)            = delete;
    RealtimeThreadScope& operator=(const RealtimeThreadScope&) = delete;

    [[nodiscard]] bool mmcssActive() const noexcept { return mmcss_ != nullptr; }
    [[nodiscard]] bool pinnedToPerformanceCores() const noexcept { return pinned_; }

private:
    void*         mmcss_    = nullptr;
    bool          pinned_   = false;
    unsigned      savedCsr_ = 0;
};

} // namespace hc::rt

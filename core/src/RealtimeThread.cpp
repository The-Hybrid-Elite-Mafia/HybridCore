#include "hybrid/core/RealtimeThread.hpp"

#include <windows.h>
#include <avrt.h>
#include <xmmintrin.h>

#include <cstddef>
#include <vector>

namespace {

// CPU set IDs with the highest EfficiencyClass (P-cores). Empty on non-hybrid parts.
std::vector<ULONG> performanceCpuSetIds() {
    ULONG len = 0;
    ::GetSystemCpuSetInformation(nullptr, 0, &len, ::GetCurrentProcess(), 0);
    if (len == 0) return {};

    std::vector<std::byte> buf(len);
    auto* first = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data());
    if (!::GetSystemCpuSetInformation(first, len, &len, ::GetCurrentProcess(), 0)) return {};

    BYTE maxClass = 0;
    BYTE minClass = 0xFF;
    for (ULONG off = 0; off < len;) {
        const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
        if (e->Type == CpuSetInformation) {
            if (e->CpuSet.EfficiencyClass > maxClass) maxClass = e->CpuSet.EfficiencyClass;
            if (e->CpuSet.EfficiencyClass < minClass) minClass = e->CpuSet.EfficiencyClass;
        }
        off += e->Size;
    }
    if (maxClass == minClass) return {};

    std::vector<ULONG> ids;
    for (ULONG off = 0; off < len;) {
        const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
        if (e->Type == CpuSetInformation && e->CpuSet.EfficiencyClass == maxClass)
            ids.push_back(e->CpuSet.Id);
        off += e->Size;
    }
    return ids;
}

} // namespace

namespace hc::rt {

void enableFlushToZero() noexcept {
    _mm_setcsr(_mm_getcsr() | kMxcsrFtzDaz);
}

void configureProcessForAudio() noexcept {
    ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    PROCESS_POWER_THROTTLING_STATE p{};
    p.Version     = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    p.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    p.StateMask   = 0;   // explicitly opt out of EcoQoS
    ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &p, sizeof(p));
}

RealtimeThreadScope::RealtimeThreadScope() noexcept {
    savedCsr_ = _mm_getcsr();
    _mm_setcsr(savedCsr_ | kMxcsrFtzDaz);

    THREAD_POWER_THROTTLING_STATE t{};
    t.Version     = THREAD_POWER_THROTTLING_CURRENT_VERSION;
    t.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
    t.StateMask   = 0;
    ::SetThreadInformation(::GetCurrentThread(), ThreadPowerThrottling, &t, sizeof(t));

    try {
        const std::vector<ULONG> ids = performanceCpuSetIds();
        if (!ids.empty())
            pinned_ = ::SetThreadSelectedCpuSets(::GetCurrentThread(), ids.data(),
                                                 static_cast<ULONG>(ids.size())) != FALSE;
    } catch (...) {
        pinned_ = false;   // allocation failure: run unpinned rather than abort the stream
    }

    DWORD taskIndex = 0;
    HANDLE h = ::AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (h) {
        ::AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
        mmcss_ = h;
    } else {
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
}

RealtimeThreadScope::~RealtimeThreadScope() {
    if (mmcss_) ::AvRevertMmThreadCharacteristics(static_cast<HANDLE>(mmcss_));
    if (pinned_) ::SetThreadSelectedCpuSets(::GetCurrentThread(), nullptr, 0);
    _mm_setcsr(savedCsr_);
}

} // namespace hc::rt

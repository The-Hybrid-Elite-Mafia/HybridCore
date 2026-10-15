#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>
#include <intrin.h>
#include <immintrin.h>

#include <bit>
#include <cstddef>
#include <cstring>
#include <vector>

namespace hc::io {

CpuReport probeCpu() {
    CpuReport r{};
    int regs[4]{};

    // Brand string
    ::__cpuid(regs, 0x80000000);
    if (static_cast<unsigned>(regs[0]) >= 0x80000004u) {
        char brand[49]{};
        for (int i = 0; i < 3; ++i) {
            ::__cpuid(regs, static_cast<int>(0x80000002u + i));
            std::memcpy(brand + i * 16, regs, 16);
        }
        r.brand = brand;
        const auto first = r.brand.find_first_not_of(' ');
        const auto last  = r.brand.find_last_not_of(' ');
        r.brand = (first == std::string::npos) ? std::string{} : r.brand.substr(first, last - first + 1);
    }

    // Feature flags (OS must have enabled YMM state via XSAVE)
    ::__cpuid(regs, 0);
    const int maxLeaf = regs[0];

    ::__cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx     = (regs[2] & (1 << 28)) != 0;
    const bool fma     = (regs[2] & (1 << 12)) != 0;
    const bool ymmOs   = osxsave && ((::_xgetbv(0) & 0x6) == 0x6);

    bool avx2 = false;
    if (maxLeaf >= 7) {
        ::__cpuidex(regs, 7, 0);
        avx2     = (regs[1] & (1 << 5)) != 0;
        r.bmi2   = (regs[1] & (1 << 8)) != 0;
        r.hybrid = (regs[3] & (1 << 15)) != 0;
    }
    r.avx2 = avx && avx2 && ymmOs;
    r.fma  = fma && ymmOs;

    // P-core / E-core topology (EfficiencyClass: highest value = P-core)
    DWORD len = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0) return r;

    std::vector<std::byte> buf(len);
    auto* base = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len)) return r;

    BYTE maxClass = 0;
    for (DWORD off = 0; off < len;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (info->Processor.EfficiencyClass > maxClass) maxClass = info->Processor.EfficiencyClass;
        off += info->Size;
    }
    for (DWORD off = 0; off < len;) {
        const auto* info = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        std::uint32_t threads = 0;
        for (WORD g = 0; g < info->Processor.GroupCount; ++g)
            threads += static_cast<std::uint32_t>(std::popcount(info->Processor.GroupMask[g].Mask));
        if (info->Processor.EfficiencyClass == maxClass) { ++r.pCores; r.pThreads += threads; }
        else                                             { ++r.eCores; r.eThreads += threads; }
        off += info->Size;
    }
    return r;
}

} // namespace hc::io

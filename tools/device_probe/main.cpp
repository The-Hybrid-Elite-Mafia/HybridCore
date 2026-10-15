// hc_device_probe — validates the rig: CPU silicon, WASAPI 48 kHz exclusive support, ASIO drivers.
// Exit codes: 0 = rig ready, 2 = CPU lacks AVX2/FMA, 3 = COM init failed, 4 = no 48 kHz path found.
#include "hybrid/io/DeviceProbe.hpp"

#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace {
const wchar_t* yn(bool v) { return v ? L"YES" : L"no"; }
}

int wmain() {
    ::_setmode(::_fileno(stdout), _O_U16TEXT);
    using namespace hc::io;

    const CpuReport cpu = probeCpu();
    std::wprintf(L"== CPU ==\n  %hs\n", cpu.brand.c_str());
    std::wprintf(L"  Target rig (i7-14700HX): %ls\n", yn(cpu.isTargetRig()));
    std::wprintf(L"  AVX2 %ls | FMA %ls | BMI2 %ls | Hybrid %ls\n",
                 yn(cpu.avx2), yn(cpu.fma), yn(cpu.bmi2), yn(cpu.hybrid));
    std::wprintf(L"  P-cores %u (%u threads) | E-cores %u (%u threads)\n\n",
                 cpu.pCores, cpu.pThreads, cpu.eCores, cpu.eThreads);
    if (!cpu.avx2 || !cpu.fma) {
        std::wprintf(L"FATAL: AVX2/FMA unavailable. HybridCore is compiled /arch:AVX2.\n");
        return 2;
    }

    ComApartment com;
    if (!com.ok()) {
        std::wprintf(L"FATAL: COM initialisation failed.\n");
        return 3;
    }

    bool any48k = false;

    std::wprintf(L"== WASAPI ==\n");
    if (auto endpoints = enumerateWasapi(); !endpoints) {
        std::wprintf(L"  Enumeration failed: HRESULT 0x%08lX\n", static_cast<unsigned long>(endpoints.error()));
    } else if (endpoints->empty()) {
        std::wprintf(L"  No active endpoints.\n");
    } else {
        for (const auto& ep : *endpoints) {
            std::wprintf(L"  [%ls] %ls\n", ep.flow == Flow::Render ? L"OUT" : L"IN ", ep.name.c_str());
            if (ep.hresult != 0 && ep.mixRate == 0) {
                std::wprintf(L"        probe failed: HRESULT 0x%08lX\n", static_cast<unsigned long>(ep.hresult));
                continue;
            }
            std::wprintf(L"        shared mix: %u Hz, %u ch\n", ep.mixRate, ep.mixChannels);
            std::wprintf(L"        exclusive 48k: f32 %ls | s24in32 %ls | s16 %ls\n",
                         yn(ep.excl48kFloat32), yn(ep.excl48kInt24in32), yn(ep.excl48kInt16));
            std::wprintf(L"        period: default %.3f ms | min %.3f ms\n", ep.defaultPeriodMs, ep.minPeriodMs);
            if (ep.excl48kFloat32 || ep.excl48kInt24in32 || ep.excl48kInt16 || ep.mixRate == 48000)
                any48k = true;
        }
    }

    std::wprintf(L"\n== ASIO (64-bit) ==\n");
    const auto drivers = enumerateAsioDrivers();
    if (drivers.empty()) {
        std::wprintf(L"  No ASIO drivers registered.\n");
    } else {
        for (const auto& d : drivers) {
            std::wprintf(L"  %ls\n        CLSID %ls\n", d.name.c_str(), d.clsid.c_str());
            if (!d.description.empty()) std::wprintf(L"        %ls\n", d.description.c_str());
            std::wprintf(L"        DLL   %ls\n", d.dllPath.empty() ? L"(not registered for x64)" : d.dllPath.c_str());
            if (!d.dllPath.empty()) any48k = true;  // Rate is negotiated at open time (Phase 2).
        }
    }

#if defined(HC_HAS_ASIO_SDK)
    std::wprintf(L"\nASIO SDK: linked\n");
#else
    std::wprintf(L"\nASIO SDK: NOT linked (place SDK in external/asiosdk)\n");
#endif

    if (!any48k) {
        std::wprintf(L"\nFATAL: no 48 kHz-capable path detected.\n");
        return 4;
    }
    std::wprintf(L"\nRIG READY.\n");
    return 0;
}

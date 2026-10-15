// hc_device_probe — validates the rig and MEASURES the real stream timing.
//
//   hc_device_probe                    full probe, opens each 48 kHz-capable output for 1 s of silence
//   hc_device_probe --seconds 3        longer measurement window per device
//   hc_device_probe --no-measure       capability report only, opens nothing
//   hc_device_probe --no-audio-ok      CI mode: a machine with no audio hardware is not a failure
//
// Exit codes: 0 rig ready | 1 bad args | 2 CPU lacks AVX2/FMA | 3 COM init failed
//             4 no 48 kHz path found | 5 every measurement attempt failed
#include "hybrid/core/Config.hpp"
#include "hybrid/io/AudioCallback.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/io/WasapiExclusiveOutput.hpp"
#if defined(HC_HAS_ASIO_SDK)
#include "hybrid/io/AsioDuplex.hpp"
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>

namespace {

const wchar_t* yn(bool v) { return v ? L"YES" : L"no"; }

// Silent callback that timestamps every hardware callback (wait-free, no allocation).
class CadenceMeter final : public hc::io::AudioCallback {
public:
    static constexpr std::size_t kMaxStamps = 8192;

    struct Stats {
        std::size_t callbacks = 0;
        double      avgMs = 0.0, minMs = 0.0, maxMs = 0.0, jitterMs = 0.0;
    };

    void prepare(const hc::io::StreamConfig& config) override {
        config_ = config;
        count_.store(0, std::memory_order_relaxed);
    }

    void process(const float* const*, std::uint32_t, float* const* outputs, std::uint32_t numOutputs,
                 std::uint32_t frames) noexcept override {
        for (std::uint32_t c = 0; c < numOutputs; ++c) std::memset(outputs[c], 0, frames * sizeof(float));
        const std::size_t n = count_.load(std::memory_order_relaxed);
        if (n < kMaxStamps) {
            LARGE_INTEGER t;
            ::QueryPerformanceCounter(&t);
            stamps_[n] = t.QuadPart;
            count_.store(n + 1, std::memory_order_release);
        }
    }

    [[nodiscard]] Stats stats() const {
        Stats s;
        const std::size_t n = count_.load(std::memory_order_acquire);
        s.callbacks = n;
        const std::size_t skip = n > 16 ? 4 : 0;   // ignore start-up intervals
        if (n < skip + 3) return s;

        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        const double toMs = 1000.0 / static_cast<double>(f.QuadPart);

        double sum = 0.0, sumSq = 0.0, mn = 1e300, mx = 0.0;
        std::size_t k = 0;
        for (std::size_t i = skip + 1; i < n; ++i, ++k) {
            const double d = static_cast<double>(stamps_[i] - stamps_[i - 1]) * toMs;
            sum += d; sumSq += d * d;
            mn = std::min(mn, d); mx = std::max(mx, d);
        }
        s.avgMs    = sum / static_cast<double>(k);
        s.minMs    = mn;
        s.maxMs    = mx;
        s.jitterMs = std::sqrt(std::max(0.0, sumSq / static_cast<double>(k) - s.avgMs * s.avgMs));
        return s;
    }

    [[nodiscard]] const hc::io::StreamConfig& config() const noexcept { return config_; }

private:
    std::array<long long, kMaxStamps> stamps_{};
    std::atomic<std::size_t>          count_{0};
    hc::io::StreamConfig              config_{};
};

// Waits while dispatching window messages (ASIO drivers in an STA need the pump).
void pumpFor(double seconds) {
    const ULONGLONG end = ::GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (::GetTickCount64() < end) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

void printMeasurement(const CadenceMeter& m, double seconds) {
    const auto& c        = m.config();
    const double bufMs   = 1000.0 * c.blockFrames / c.sampleRate;
    const auto   st      = m.stats();
    const double expected = seconds * c.sampleRate / c.blockFrames;
    std::wprintf(L"        buffer granted : %u frames = %.3f ms @ %u Hz\n", c.blockFrames, bufMs, c.sampleRate);
    if (c.numInputs)  std::wprintf(L"        input latency  : %.3f ms (driver-reported)\n", c.inputLatencyMs);
    std::wprintf(L"        output latency : %.3f ms (driver-reported)\n", c.outputLatencyMs);
    if (st.callbacks < 3) {
        std::wprintf(L"        MEASURED       : FAILED — only %zu callbacks in %.1f s (expected ~%.0f)\n",
                     st.callbacks, seconds, expected);
        return;
    }
    std::wprintf(L"        MEASURED       : %zu callbacks in %.1f s (expected ~%.0f)\n", st.callbacks, seconds, expected);
    std::wprintf(L"        callback period: avg %.3f ms | min %.3f | max %.3f | jitter %.3f ms\n",
                 st.avgMs, st.minMs, st.maxMs, st.jitterMs);
}

struct Args {
    bool   measure    = true;
    bool   noAudioOk  = false;
    double seconds    = 1.0;
};

bool parse(int argc, wchar_t** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        if (k == L"--no-measure")       a.measure = false;
        else if (k == L"--no-audio-ok") a.noAudioOk = true;
        else if (k == L"--seconds" && i + 1 < argc) a.seconds = std::wcstod(argv[++i], nullptr);
        else return false;
    }
    return a.seconds > 0.0 && a.seconds <= 60.0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    hc::io::configureWideConsole();
    using namespace hc::io;

    Args args;
    if (!parse(argc, argv, args)) {
        std::wprintf(L"usage: hc_device_probe [--seconds S] [--no-measure] [--no-audio-ok]\n");
        return 1;
    }

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

    // STA: required by most ASIO drivers, fine for WASAPI.
    ComApartment com(Apartment::SingleThreaded);
    if (!com.ok()) {
        std::wprintf(L"FATAL: COM initialisation failed.\n");
        return 3;
    }

    bool any48k        = false;
    int  measureTried  = 0;
    int  measureOk     = 0;
    auto meter         = std::make_unique<CadenceMeter>();

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
            const bool excl = ep.excl48kFloat32 || ep.excl48kInt24in32 || ep.excl48kInt16;
            std::wprintf(L"        shared mix: %u Hz, %u ch\n", ep.mixRate, ep.mixChannels);
            std::wprintf(L"        exclusive %u Hz: f32 %ls | s24in32 %ls | s16 %ls\n", hc::kSampleRate,
                         yn(ep.excl48kFloat32), yn(ep.excl48kInt24in32), yn(ep.excl48kInt16));
            std::wprintf(L"        device period (reported): default %.3f ms | min %.3f ms\n",
                         ep.defaultPeriodMs, ep.minPeriodMs);
            if (excl || ep.mixRate == hc::kSampleRate) any48k = true;

            if (!args.measure || ep.flow != Flow::Render || !excl) continue;

            ++measureTried;
            WasapiExclusiveOutput stream;
            WasapiExclusiveOutput::Options opt;
            opt.deviceId = ep.id;
            auto cfg = stream.open(opt, *meter);
            if (!cfg) {
                std::wprintf(L"        MEASURED       : open failed at %hs (0x%08lX)\n", cfg.error().stage,
                             static_cast<unsigned long>(cfg.error().code));
                continue;
            }
            if (auto s = stream.start(); !s) {
                std::wprintf(L"        MEASURED       : start failed at %hs (0x%08lX)\n", s.error().stage,
                             static_cast<unsigned long>(s.error().code));
                stream.close();
                continue;
            }
            pumpFor(args.seconds);
            stream.stop();
            std::wprintf(L"        stream format  : %ls (exclusive, event-driven)\n", stream.formatName());
            stream.close();
            printMeasurement(*meter, args.seconds);
            if (meter->stats().callbacks >= 3) ++measureOk;
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
            if (d.dllPath.empty()) continue;
            any48k = true;
#if defined(HC_HAS_ASIO_SDK)
            if (!args.measure) continue;
            ++measureTried;
            AsioDuplex stream;
            AsioDuplex::Options opt;
            opt.driverName = d.name;
            auto cfg = stream.open(opt, *meter);
            if (!cfg) {
                std::wprintf(L"        MEASURED       : open failed at %hs (%ld)\n", cfg.error().stage, cfg.error().code);
                continue;
            }
            if (auto s = stream.start(); !s) {
                std::wprintf(L"        MEASURED       : start failed at %hs (%ld)\n", s.error().stage, s.error().code);
                stream.close();
                continue;
            }
            pumpFor(args.seconds);
            stream.stop();
            stream.close();
            printMeasurement(*meter, args.seconds);
            if (meter->stats().callbacks >= 3) ++measureOk;
#endif
        }
    }

#if defined(HC_HAS_ASIO_SDK)
    std::wprintf(L"\nASIO SDK: linked\n");
#else
    std::wprintf(L"\nASIO SDK: NOT linked — ASIO drivers listed but not measured (run tools/fetch-asio-sdk.ps1)\n");
#endif

    if (!any48k) {
        if (args.noAudioOk) {
            std::wprintf(L"\nNO AUDIO HARDWARE (allowed by --no-audio-ok).\n");
            return 0;
        }
        std::wprintf(L"\nFATAL: no %u Hz-capable path detected.\n", hc::kSampleRate);
        return 4;
    }
    if (measureTried > 0 && measureOk == 0) {
        std::wprintf(L"\nFATAL: %d measurement attempt(s), none delivered callbacks.\n", measureTried);
        return 5;
    }
    std::wprintf(L"\nRIG READY. Measured %d of %d stream(s).\n", measureOk, measureTried);
    return 0;
}

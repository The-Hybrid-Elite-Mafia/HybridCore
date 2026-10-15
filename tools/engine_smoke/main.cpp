// hc_engine_smoke — runs the lock-free engine on real hardware with a -18 dBFS 440 Hz tone.
//
//   hc_engine_smoke                                   WASAPI exclusive, default output, 10 s
//   hc_engine_smoke --wasapi --device 1 --period-ms 3 --seconds 30
//   hc_engine_smoke --asio "Focusrite USB ASIO" --frames 64 --seconds 30
//
// Exit codes: 0 ok, 1 bad args, 2 open failed, 3 start failed, 4 stream fault during run.
#include "hybrid/core/RealtimeThread.hpp"
#include "hybrid/engine/Engine.hpp"
#include "hybrid/engine/ToneProcessor.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/io/WasapiExclusiveOutput.hpp"
#if defined(HC_HAS_ASIO_SDK)
#include "hybrid/io/AsioDuplex.hpp"
#endif

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <fcntl.h>
#include <io.h>
#include <memory>
#include <string>
#include <vector>

namespace {

std::atomic<bool> gQuit{false};

BOOL WINAPI onConsoleCtrl(DWORD) {
    gQuit.store(true, std::memory_order_release);
    return TRUE;
}

struct Args {
    bool          asio      = false;
    std::wstring  asioName;
    int           device    = -1;
    double        periodMs  = 0.0;
    std::uint32_t frames    = 0;
    double        seconds   = 10.0;
};

bool parse(int argc, wchar_t** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring k = argv[i];
        auto next = [&]() -> const wchar_t* { return (i + 1 < argc) ? argv[++i] : nullptr; };
        if (k == L"--wasapi") {
            a.asio = false;
        } else if (k == L"--asio") {
            const wchar_t* v = next(); if (!v) return false;
            a.asio = true; a.asioName = v;
        } else if (k == L"--device") {
            const wchar_t* v = next(); if (!v) return false;
            a.device = std::wcstol(v, nullptr, 10);
        } else if (k == L"--period-ms") {
            const wchar_t* v = next(); if (!v) return false;
            a.periodMs = std::wcstod(v, nullptr);
        } else if (k == L"--frames") {
            const wchar_t* v = next(); if (!v) return false;
            a.frames = static_cast<std::uint32_t>(std::wcstoul(v, nullptr, 10));
        } else if (k == L"--seconds") {
            const wchar_t* v = next(); if (!v) return false;
            a.seconds = std::wcstod(v, nullptr);
        } else {
            return false;
        }
    }
    return a.seconds > 0.0;
}

void printEvents(std::uint32_t ev) {
    using namespace hc::io::StreamEvent;
    if (ev & DeviceLost)      std::wprintf(L"  ! device lost\n");
    if (ev & ResetRequest)    std::wprintf(L"  ! driver reset request\n");
    if (ev & ResyncRequest)   std::wprintf(L"  ! driver resync request\n");
    if (ev & Overload)        std::wprintf(L"  ! driver overload\n");
    if (ev & LatencyChanged)  std::wprintf(L"  ! latency changed\n");
    if (ev & ThreadFault)     std::wprintf(L"  ! RT thread fault (device stalled)\n");
    if (ev & SampleRateMoved) std::wprintf(L"  ! sample rate changed by driver\n");
}

constexpr std::uint32_t kFatalEvents =
    hc::io::StreamEvent::DeviceLost | hc::io::StreamEvent::ResetRequest |
    hc::io::StreamEvent::ThreadFault | hc::io::StreamEvent::SampleRateMoved;

void printConfig(const hc::io::StreamConfig& c) {
    std::wprintf(L"  %u Hz | %u frames (%.3f ms) | in %u | out %u | latency in %.2f ms / out %.2f ms\n",
                 c.sampleRate, c.blockFrames, 1000.0 * c.blockFrames / c.sampleRate,
                 c.numInputs, c.numOutputs, c.inputLatencyMs, c.outputLatencyMs);
}

// Pumps window messages (ASIO drivers in an STA need it) and reports telemetry once a second.
template <typename Stream>
int runLoop(Stream& stream, hc::engine::Engine& engine, double seconds) {
    const ULONGLONG start = ::GetTickCount64();
    ULONGLONG       lastPrint = start;
    hc::engine::Telemetry latest{};
    float           peakSincePrint = 0.0f;
    bool            haveTelemetry  = false;

    // Gain automation exercise: dip to -6 dB at 1/3, back to unity at 2/3.
    bool dipped = false, restored = false;

    while (!gQuit.load(std::memory_order_acquire)) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        hc::engine::Telemetry t;
        while (engine.popTelemetry(t)) {
            latest = t;
            haveTelemetry = true;
            if (t.loadPeak > peakSincePrint) peakSincePrint = t.loadPeak;
        }
        engine.collectGarbage();

        const std::uint32_t ev = stream.takeEvents();
        if (ev) {
            printEvents(ev);
            if (ev & kFatalEvents) return 4;
        }

        const ULONGLONG now     = ::GetTickCount64();
        const double    elapsed = static_cast<double>(now - start) / 1000.0;

        if (!dipped && elapsed >= seconds / 3.0)        { dipped   = engine.setMasterGain(0.5f); }
        if (!restored && elapsed >= 2.0 * seconds / 3.0) { restored = engine.setMasterGain(1.0f); }

        if (now - lastPrint >= 1000) {
            lastPrint = now;
            if (haveTelemetry)
                std::wprintf(L"  t=%6.2fs  load avg %5.2f%%  peak %5.2f%%  overruns %u  gain %.3f\n",
                             static_cast<double>(latest.sampleTime) / hc::kSampleRate,
                             latest.loadAvg * 100.0f, peakSincePrint * 100.0f, latest.overruns,
                             latest.masterGain);
            else
                std::wprintf(L"  (no telemetry yet — stream not delivering callbacks)\n");
            peakSincePrint = 0.0f;
        }
        if (elapsed >= seconds) break;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    return 0;
}

int resolveWasapiDevice(int index, std::wstring& id) {
    if (index < 0) return 0;
    auto endpoints = hc::io::enumerateWasapi();
    if (!endpoints) return 1;
    int n = 0;
    for (const auto& ep : *endpoints) {
        if (ep.flow != hc::io::Flow::Render) continue;
        if (n++ == index) { id = ep.id; return 0; }
    }
    return 1;
}

void listRenderDevices() {
    auto endpoints = hc::io::enumerateWasapi();
    if (!endpoints) return;
    int n = 0;
    for (const auto& ep : *endpoints)
        if (ep.flow == hc::io::Flow::Render) std::wprintf(L"    [%d] %ls\n", n++, ep.name.c_str());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    hc::io::configureWideConsole();

    Args args;
    if (!parse(argc, argv, args)) {
        std::wprintf(L"usage: hc_engine_smoke [--wasapi [--device N] [--period-ms X]] | "
                     L"[--asio \"Driver Name\" [--frames N]] [--seconds S]\n");
        return 1;
    }

    ::SetConsoleCtrlHandler(&onConsoleCtrl, TRUE);
    hc::rt::configureProcessForAudio();

    // STA on the main thread: required by most ASIO drivers, harmless for WASAPI.
    hc::io::ComApartment com(hc::io::Apartment::SingleThreaded);
    if (!com.ok()) { std::wprintf(L"COM init failed\n"); return 2; }

    hc::engine::Engine engine;
    int rc = 0;

    if (!args.asio) {
        hc::io::WasapiExclusiveOutput::Options opt;
        opt.periodMs = args.periodMs;
        if (resolveWasapiDevice(args.device, opt.deviceId) != 0) {
            std::wprintf(L"Render device %d not found. Available:\n", args.device);
            listRenderDevices();
            return 1;
        }

        hc::io::WasapiExclusiveOutput stream;
        auto cfg = stream.open(opt, engine);
        if (!cfg) {
            std::wprintf(L"WASAPI open failed at %hs: 0x%08lX\n", cfg.error().stage,
                         static_cast<unsigned long>(cfg.error().code));
            return 2;
        }
        std::wprintf(L"== WASAPI exclusive (%ls) ==\n", stream.formatName());
        printConfig(*cfg);

        std::unique_ptr<hc::engine::AudioProcessor> tone = std::make_unique<hc::engine::ToneProcessor>();
        if (!engine.install(tone)) { std::wprintf(L"install failed\n"); return 2; }

        if (auto s = stream.start(); !s) {
            std::wprintf(L"WASAPI start failed at %hs: 0x%08lX\n", s.error().stage,
                         static_cast<unsigned long>(s.error().code));
            return 3;
        }
        ::Sleep(50);
        std::wprintf(L"  RT thread: MMCSS %ls | P-core pinned %ls\n",
                     stream.rtThreadUsesMmcss() ? L"YES" : L"no", stream.rtThreadPinned() ? L"YES" : L"no");
        rc = runLoop(stream, engine, args.seconds);
        stream.close();
    } else {
#if defined(HC_HAS_ASIO_SDK)
        hc::io::AsioDuplex::Options opt;
        opt.driverName   = args.asioName;
        opt.bufferFrames = args.frames;

        hc::io::AsioDuplex stream;
        auto cfg = stream.open(opt, engine);
        if (!cfg) {
            std::wprintf(L"ASIO open failed at %hs: %ld\n", cfg.error().stage, cfg.error().code);
            std::wprintf(L"Registered 64-bit ASIO drivers:\n");
            for (const auto& d : hc::io::enumerateAsioDrivers()) std::wprintf(L"    \"%ls\"\n", d.name.c_str());
            return 2;
        }
        std::wprintf(L"== ASIO: %ls ==\n", args.asioName.c_str());
        printConfig(*cfg);

        std::unique_ptr<hc::engine::AudioProcessor> tone = std::make_unique<hc::engine::ToneProcessor>();
        if (!engine.install(tone)) { std::wprintf(L"install failed\n"); return 2; }

        if (auto s = stream.start(); !s) {
            std::wprintf(L"ASIO start failed at %hs: %ld\n", s.error().stage, s.error().code);
            return 3;
        }
        rc = runLoop(stream, engine, args.seconds);
        stream.close();
#else
        std::wprintf(L"ASIO not available: build with the Steinberg SDK in external/asiosdk.\n");
        return 1;
#endif
    }

    engine.collectGarbage();
    std::wprintf(rc == 0 ? L"\nSTREAM OK.\n" : L"\nSTREAM FAULTED.\n");
    return rc;
}

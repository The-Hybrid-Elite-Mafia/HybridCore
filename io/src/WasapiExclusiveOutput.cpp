#include "hybrid/io/WasapiExclusiveOutput.hpp"
#include "hybrid/io/DeviceProbe.hpp"
#include "hybrid/core/AlignedBuffer.hpp"
#include "hybrid/core/Config.hpp"
#include "hybrid/core/RealtimeThread.hpp"

#include "SampleConvert.hpp"
#include "WasapiFormat.hpp"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace hc::io {

namespace {
constexpr DWORD    kEventTimeoutMs      = 200;
constexpr unsigned kMaxConsecutiveStall = 5;
constexpr double   kHnsPerSecond        = 10'000'000.0;
} // namespace

struct WasapiExclusiveOutput::Impl {
    ComPtr<IMMDevice>          device;
    ComPtr<IAudioClient>       client;
    ComPtr<IAudioRenderClient> render;
    HANDLE                     event = nullptr;

    wasapi::SampleFormat format     = wasapi::SampleFormat::Float32;
    const wchar_t*       formatName = L"none";
    std::uint32_t        channels   = 0;
    std::uint32_t        frames     = 0;

    core::AlignedBuffer<float>                planar;
    core::AlignedBuffer<float>                interleaved;
    std::array<float*, hc::kMaxChannels>      planarPtrs{};

    AudioCallback* callback = nullptr;
    std::thread    thread;

    std::atomic<bool>          stopRequested{false};
    std::atomic<bool>          running{false};
    std::atomic<bool>          mmcss{false};
    std::atomic<bool>          pinned{false};
    std::atomic<std::uint32_t> events{0};

    void raise(std::uint32_t flag) noexcept { events.fetch_or(flag, std::memory_order_relaxed); }

    void writeDeviceBuffer(BYTE* dst) noexcept {
        const std::size_t samples = static_cast<std::size_t>(frames) * channels;
        switch (format) {
        case wasapi::SampleFormat::Float32:
            convert::interleave(planarPtrs.data(), channels, frames, reinterpret_cast<float*>(dst));
            break;
        case wasapi::SampleFormat::S24In32:
            convert::interleave(planarPtrs.data(), channels, frames, interleaved.data());
            convert::floatToS24In32(interleaved.data(), reinterpret_cast<std::int32_t*>(dst), samples);
            break;
        case wasapi::SampleFormat::S16:
            convert::interleave(planarPtrs.data(), channels, frames, interleaved.data());
            convert::floatToS16(interleaved.data(), reinterpret_cast<std::int16_t*>(dst), samples);
            break;
        case wasapi::SampleFormat::S24Packed:
            convert::interleave(planarPtrs.data(), channels, frames, interleaved.data());
            convert::floatToS24Packed(interleaved.data(), dst, samples);
            break;
        }
    }

    // ---- RT thread --------------------------------------------------------
    void run() noexcept {
        ComApartment         com(Apartment::MultiThreaded);
        rt::RealtimeThreadScope rtScope;
        mmcss.store(rtScope.mmcssActive(), std::memory_order_relaxed);
        pinned.store(rtScope.pinnedToPerformanceCores(), std::memory_order_relaxed);

        unsigned stalls = 0;
        while (!stopRequested.load(std::memory_order_acquire)) {
            const DWORD w = ::WaitForSingleObject(event, kEventTimeoutMs);
            if (stopRequested.load(std::memory_order_acquire)) break;
            if (w != WAIT_OBJECT_0) {
                if (++stalls >= kMaxConsecutiveStall) { raise(StreamEvent::ThreadFault); break; }
                continue;
            }
            stalls = 0;

            BYTE*   data = nullptr;
            HRESULT hr   = render->GetBuffer(frames, &data);
            if (FAILED(hr)) {
                raise(hr == AUDCLNT_E_DEVICE_INVALIDATED ? StreamEvent::DeviceLost : StreamEvent::ThreadFault);
                break;
            }

            callback->process(nullptr, 0, planarPtrs.data(), channels, frames);
            writeDeviceBuffer(data);

            hr = render->ReleaseBuffer(frames, 0);
            if (FAILED(hr)) {
                raise(hr == AUDCLNT_E_DEVICE_INVALIDATED ? StreamEvent::DeviceLost : StreamEvent::ThreadFault);
                break;
            }
        }
        running.store(false, std::memory_order_release);
    }
};

WasapiExclusiveOutput::WasapiExclusiveOutput() : impl_(std::make_unique<Impl>()) {}
WasapiExclusiveOutput::~WasapiExclusiveOutput() { close(); }

std::expected<StreamConfig, BackendError>
WasapiExclusiveOutput::open(const Options& options, AudioCallback& callback) {
    close();
    Impl& s = *impl_;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "CoCreateInstance(MMDeviceEnumerator)"});

    hr = options.deviceId.empty()
             ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s.device)
             : enumerator->GetDevice(options.deviceId.c_str(), &s.device);
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "GetDevice"});

    auto activate = [&]() -> HRESULT {
        s.client.Reset();
        return s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(s.client.GetAddressOf()));
    };
    if (FAILED(hr = activate())) return std::unexpected(BackendError{hr, "Activate(IAudioClient)"});

    WORD  channels = 2;
    DWORD mask     = 0;
    wasapi::layoutFromMix(s.client.Get(), channels, mask);
    if (channels > hc::kMaxChannels)
        return std::unexpected(BackendError{E_INVALIDARG, "device channel count exceeds kMaxChannels"});

    const wasapi::FormatSpec* chosen = nullptr;
    WAVEFORMATEXTENSIBLE      wfx{};
    for (const auto& spec : wasapi::kExclusiveFormats) {
        wfx = wasapi::makeFormat(channels, mask, spec);
        if (wasapi::exclusiveSupports(s.client.Get(), wfx)) { chosen = &spec; break; }
    }
    if (!chosen) return std::unexpected(BackendError{AUDCLNT_E_UNSUPPORTED_FORMAT, "no 48 kHz exclusive format"});

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    if (FAILED(hr = s.client->GetDevicePeriod(&defPeriod, &minPeriod)))
        return std::unexpected(BackendError{hr, "GetDevicePeriod"});

    REFERENCE_TIME period = minPeriod;
    if (options.periodMs > 0.0) {
        const auto requested = static_cast<REFERENCE_TIME>(options.periodMs * 10'000.0 + 0.5);
        period = requested > minPeriod ? requested : minPeriod;
    }

    const auto* fmt = reinterpret_cast<const WAVEFORMATEX*>(&wfx);
    hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              period, period, fmt, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        if (FAILED(hr = s.client->GetBufferSize(&aligned)))
            return std::unexpected(BackendError{hr, "GetBufferSize(align)"});
        period = static_cast<REFERENCE_TIME>(kHnsPerSecond * aligned / hc::kSampleRate + 0.5);
        if (FAILED(hr = activate())) return std::unexpected(BackendError{hr, "Activate(realign)"});
        hr = s.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                  period, period, fmt, nullptr);
    }
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "IAudioClient::Initialize(exclusive)"});

    s.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event) return std::unexpected(BackendError{HRESULT_FROM_WIN32(::GetLastError()), "CreateEvent"});
    if (FAILED(hr = s.client->SetEventHandle(s.event)))
        return std::unexpected(BackendError{hr, "SetEventHandle"});

    UINT32 bufferFrames = 0;
    if (FAILED(hr = s.client->GetBufferSize(&bufferFrames)))
        return std::unexpected(BackendError{hr, "GetBufferSize"});
    if (bufferFrames == 0 || bufferFrames > hc::kMaxBlockFrames)
        return std::unexpected(BackendError{E_UNEXPECTED, "buffer size outside kMaxBlockFrames"});

    if (FAILED(hr = s.client->GetService(IID_PPV_ARGS(&s.render))))
        return std::unexpected(BackendError{hr, "GetService(IAudioRenderClient)"});

    s.format     = chosen->format;
    s.formatName = chosen->name;
    s.channels   = channels;
    s.frames     = bufferFrames;
    s.callback   = &callback;

    s.planar.allocate(static_cast<std::size_t>(channels) * bufferFrames);
    s.interleaved.allocate(static_cast<std::size_t>(channels) * bufferFrames);
    for (std::uint32_t c = 0; c < channels; ++c) s.planarPtrs[c] = s.planar.data() + c * bufferFrames;

    StreamConfig cfg{};
    cfg.sampleRate      = hc::kSampleRate;
    cfg.blockFrames     = bufferFrames;
    cfg.numInputs       = 0;
    cfg.numOutputs      = channels;
    // Driver-reported stream latency when available; otherwise one buffer period.
    REFERENCE_TIME streamLatency = 0;
    cfg.outputLatencyMs = (SUCCEEDED(s.client->GetStreamLatency(&streamLatency)) && streamLatency > 0)
                              ? static_cast<double>(streamLatency) / 10'000.0
                              : 1000.0 * bufferFrames / hc::kSampleRate;

    callback.prepare(cfg);
    return cfg;
}

std::expected<void, BackendError> WasapiExclusiveOutput::start() {
    Impl& s = *impl_;
    if (!s.client || !s.render) return std::unexpected(BackendError{E_UNEXPECTED, "start before open"});
    if (s.running.load(std::memory_order_acquire)) return {};

    // Pre-roll one silent period so the first hardware event never underruns.
    BYTE*   data = nullptr;
    HRESULT hr   = s.render->GetBuffer(s.frames, &data);
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "GetBuffer(preroll)"});
    if (FAILED(hr = s.render->ReleaseBuffer(s.frames, AUDCLNT_BUFFERFLAGS_SILENT)))
        return std::unexpected(BackendError{hr, "ReleaseBuffer(preroll)"});

    s.events.store(0, std::memory_order_relaxed);
    s.stopRequested.store(false, std::memory_order_release);
    s.running.store(true, std::memory_order_release);

    if (FAILED(hr = s.client->Start())) {
        s.running.store(false, std::memory_order_release);
        return std::unexpected(BackendError{hr, "IAudioClient::Start"});
    }

    try {
        s.thread = std::thread([&s] { s.run(); });
    } catch (...) {
        s.client->Stop();
        s.client->Reset();
        s.running.store(false, std::memory_order_release);
        return std::unexpected(BackendError{E_OUTOFMEMORY, "std::thread"});
    }
    return {};
}

void WasapiExclusiveOutput::stop() noexcept {
    Impl& s = *impl_;
    if (s.thread.joinable()) {
        s.stopRequested.store(true, std::memory_order_release);
        if (s.event) ::SetEvent(s.event);
        s.thread.join();
    }
    if (s.client) {
        s.client->Stop();
        s.client->Reset();
    }
    s.running.store(false, std::memory_order_release);
}

void WasapiExclusiveOutput::close() noexcept {
    stop();
    Impl& s = *impl_;
    s.render.Reset();
    s.client.Reset();
    s.device.Reset();
    if (s.event) { ::CloseHandle(s.event); s.event = nullptr; }
    s.callback   = nullptr;
    s.channels   = 0;
    s.frames     = 0;
    s.formatName = L"none";
    s.planarPtrs.fill(nullptr);
}

std::uint32_t WasapiExclusiveOutput::takeEvents() noexcept {
    return impl_->events.exchange(0, std::memory_order_acq_rel);
}

bool WasapiExclusiveOutput::isRunning() const noexcept { return impl_->running.load(std::memory_order_acquire); }
const wchar_t* WasapiExclusiveOutput::formatName() const noexcept { return impl_->formatName; }
bool WasapiExclusiveOutput::rtThreadUsesMmcss() const noexcept { return impl_->mmcss.load(std::memory_order_relaxed); }
bool WasapiExclusiveOutput::rtThreadPinned() const noexcept { return impl_->pinned.load(std::memory_order_relaxed); }

} // namespace hc::io

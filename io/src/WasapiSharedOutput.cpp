#include "hybrid/io/WasapiSharedOutput.hpp"
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

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace hc::io {

namespace {
constexpr DWORD    kEventTimeoutMs      = 200;
constexpr unsigned kMaxConsecutiveStall = 5;
} // namespace

struct WasapiSharedOutput::Impl {
    ComPtr<IMMDevice>          device;
    ComPtr<IAudioClient>       client;
    ComPtr<IAudioRenderClient> render;
    HANDLE                     event = nullptr;

    std::uint32_t channels = 0;
    std::uint32_t frames   = 0;   // device buffer size in frames
    std::uint32_t mixRate  = 0;

    core::AlignedBuffer<float>           planar;
    std::array<float*, hc::kMaxChannels> planarPtrs{};

    AudioCallback* callback = nullptr;
    std::thread    thread;

    std::atomic<bool>          stopRequested{false};
    std::atomic<bool>          running{false};
    std::atomic<bool>          mmcss{false};
    std::atomic<bool>          pinned{false};
    std::atomic<std::uint32_t> events{0};

    void raise(std::uint32_t flag) noexcept { events.fetch_or(flag, std::memory_order_relaxed); }

    // ---- RT thread --------------------------------------------------------
    void run() noexcept {
        ComApartment            com(Apartment::MultiThreaded);
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

            UINT32  padding = 0;
            HRESULT hr      = client->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                raise(hr == AUDCLNT_E_DEVICE_INVALIDATED ? StreamEvent::DeviceLost : StreamEvent::ThreadFault);
                break;
            }
            const std::uint32_t free = padding < frames ? frames - padding : 0;
            if (free == 0) continue;

            BYTE* data = nullptr;
            if (FAILED(hr = render->GetBuffer(free, &data))) {
                raise(hr == AUDCLNT_E_DEVICE_INVALIDATED ? StreamEvent::DeviceLost : StreamEvent::ThreadFault);
                break;
            }
            callback->process(nullptr, 0, planarPtrs.data(), channels, free);
            convert::interleave(planarPtrs.data(), channels, free, reinterpret_cast<float*>(data));
            if (FAILED(hr = render->ReleaseBuffer(free, 0))) {
                raise(hr == AUDCLNT_E_DEVICE_INVALIDATED ? StreamEvent::DeviceLost : StreamEvent::ThreadFault);
                break;
            }
        }
        running.store(false, std::memory_order_release);
    }
};

WasapiSharedOutput::WasapiSharedOutput() : impl_(std::make_unique<Impl>()) {}
WasapiSharedOutput::~WasapiSharedOutput() { close(); }

std::expected<StreamConfig, BackendError>
WasapiSharedOutput::open(const Options& options, AudioCallback& callback) {
    close();
    Impl& s = *impl_;

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "CoCreateInstance(MMDeviceEnumerator)"});

    hr = options.deviceId.empty()
             ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &s.device)
             : enumerator->GetDevice(options.deviceId.c_str(), &s.device);
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "GetDevice"});

    if (FAILED(hr = s.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(s.client.GetAddressOf()))))
        return std::unexpected(BackendError{hr, "Activate(IAudioClient)"});

    WORD  channels = 2;
    DWORD mask     = 0;
    wasapi::layoutFromMix(s.client.Get(), channels, mask);
    if (channels > hc::kMaxChannels)
        return std::unexpected(BackendError{E_INVALIDARG, "device channel count exceeds kMaxChannels"});
    {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(s.client->GetMixFormat(&mix)) && mix) {
            s.mixRate = mix->nSamplesPerSec;
            ::CoTaskMemFree(mix);
        }
    }

    // Always 48 kHz float at the device's channel layout; Windows converts the rate if it must.
    const WAVEFORMATEXTENSIBLE wfx = wasapi::makeFormat(channels, mask, 32, 32, wasapi::kSubtypeIeeeFloat);
    const double ms = options.bufferMs > 3.0 ? options.bufferMs : 3.0;
    const auto   bufferHns = static_cast<REFERENCE_TIME>(ms * 10'000.0 + 0.5);
    hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                              bufferHns, 0, reinterpret_cast<const WAVEFORMATEX*>(&wfx), nullptr);
    if (FAILED(hr)) return std::unexpected(BackendError{hr, "IAudioClient::Initialize(shared)"});

    s.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s.event) return std::unexpected(BackendError{HRESULT_FROM_WIN32(::GetLastError()), "CreateEvent"});
    if (FAILED(hr = s.client->SetEventHandle(s.event))) return std::unexpected(BackendError{hr, "SetEventHandle"});

    UINT32 bufferFrames = 0;
    if (FAILED(hr = s.client->GetBufferSize(&bufferFrames))) return std::unexpected(BackendError{hr, "GetBufferSize"});
    if (bufferFrames == 0 || bufferFrames > hc::kMaxBlockFrames)
        return std::unexpected(BackendError{E_UNEXPECTED, "buffer size outside kMaxBlockFrames"});
    if (FAILED(hr = s.client->GetService(IID_PPV_ARGS(&s.render))))
        return std::unexpected(BackendError{hr, "GetService(IAudioRenderClient)"});

    s.channels = channels;
    s.frames   = bufferFrames;
    s.callback = &callback;
    s.planar.allocate(static_cast<std::size_t>(channels) * bufferFrames);
    for (std::uint32_t c = 0; c < channels; ++c) s.planarPtrs[c] = s.planar.data() + c * bufferFrames;

    StreamConfig cfg{};
    cfg.sampleRate  = hc::kSampleRate;
    cfg.blockFrames = bufferFrames;
    cfg.numInputs   = 0;
    cfg.numOutputs  = channels;
    REFERENCE_TIME streamLatency = 0;
    const double   engineMs      = 1000.0 * bufferFrames / hc::kSampleRate;
    cfg.outputLatencyMs = engineMs + ((SUCCEEDED(s.client->GetStreamLatency(&streamLatency)) && streamLatency > 0)
                                          ? static_cast<double>(streamLatency) / 10'000.0
                                          : 0.0);
    callback.prepare(cfg);
    return cfg;
}

std::expected<void, BackendError> WasapiSharedOutput::start() {
    Impl& s = *impl_;
    if (!s.client || !s.render) return std::unexpected(BackendError{E_UNEXPECTED, "start before open"});
    if (s.running.load(std::memory_order_acquire)) return {};

    // Fill the whole buffer with silence first so the stream starts without a gap.
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

void WasapiSharedOutput::stop() noexcept {
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

void WasapiSharedOutput::close() noexcept {
    stop();
    Impl& s = *impl_;
    s.render.Reset();
    s.client.Reset();
    s.device.Reset();
    if (s.event) { ::CloseHandle(s.event); s.event = nullptr; }
    s.callback = nullptr;
    s.channels = 0;
    s.frames   = 0;
    s.mixRate  = 0;
    s.planarPtrs.fill(nullptr);
}

std::uint32_t WasapiSharedOutput::takeEvents() noexcept { return impl_->events.exchange(0, std::memory_order_acq_rel); }
bool WasapiSharedOutput::isRunning() const noexcept { return impl_->running.load(std::memory_order_acquire); }
std::uint32_t WasapiSharedOutput::deviceMixRate() const noexcept { return impl_->mixRate; }
bool WasapiSharedOutput::rtThreadUsesMmcss() const noexcept { return impl_->mmcss.load(std::memory_order_relaxed); }
bool WasapiSharedOutput::rtThreadPinned() const noexcept { return impl_->pinned.load(std::memory_order_relaxed); }

} // namespace hc::io

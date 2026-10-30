#include "AudioHost.hpp"
#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace hc::app {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

AudioHost::AudioHost() {
    auto m = std::make_unique<engine::MixerProcessor>(engine::MixerProcessor::Source::TestTones);
    mixer_ = m.get();
    pendingMixer_ = std::move(m);
}

AudioHost::~AudioHost() { close(); }

std::vector<DeviceChoice> AudioHost::listDevices() {
    std::vector<DeviceChoice> out;
    out.push_back({DeviceChoice::Kind::Wasapi, L"", "WASAPI \xC2\xB7 Default output", "Default output"});
    if (auto eps = io::enumerateWasapi()) {
        for (const auto& ep : *eps) {
            if (ep.flow != io::Flow::Render) continue;
            if (ep.excl48kFloat32 || ep.excl48kInt24in32 || ep.excl48kInt16)
                out.push_back({DeviceChoice::Kind::Wasapi, ep.id, "WASAPI \xC2\xB7 " + toUtf8(ep.name), toUtf8(ep.name)});
            else   // no 48 kHz exclusive mode (many HDMI soundbars, TVs, Bluetooth): still playable, Windows converts
                out.push_back({DeviceChoice::Kind::WasapiShared, ep.id, "WASAPI shared \xC2\xB7 " + toUtf8(ep.name), toUtf8(ep.name)});
        }
    }
#if defined(HC_HAS_ASIO_SDK)
    for (const auto& d : io::enumerateAsioDrivers())
        if (!d.dllPath.empty()) out.push_back({DeviceChoice::Kind::Asio, d.name, "ASIO \xC2\xB7 " + toUtf8(d.name), toUtf8(d.name)});
#endif
    // Last: a clock with no sound card behind it (engine + meters run, nothing is audible).
    out.push_back({DeviceChoice::Kind::Offline, L"offline", "OFFLINE CLOCK \xC2\xB7 no sound", "Offline clock (no sound)"});
    return out;
}

bool AudioHost::installMixerOnce() {
    if (!pendingMixer_) return true;
    if (!engine_.install(pendingMixer_)) return false;   // on success pendingMixer_ is emptied
    return true;
}

void AudioHost::close() noexcept {
    if (wasapi_) { wasapi_->close(); wasapi_.reset(); }
    if (shared_) { shared_->close(); shared_.reset(); }
    if (offline_) { offline_->close(); offline_.reset(); }
#if defined(HC_HAS_ASIO_SDK)
    if (asio_) { asio_->close(); asio_.reset(); }
#endif
    engine_.collectGarbage();
    status_.running = false;
}

bool AudioHost::open(const DeviceChoice& choice) {
    close();
    current_ = choice;
    status_  = {};
    status_.device = choice.label;
    status_.deviceName = choice.name.empty() ? choice.label : choice.name;
    problem_.clear();
    peakLoad_ = 0.0f;
    driverOverloads_ = 0;
    char buf[256];

    if (choice.kind == DeviceChoice::Kind::Wasapi) {
        auto tryOpen = [&](const std::wstring& id) {
            wasapi_ = std::make_unique<io::WasapiExclusiveOutput>();
            io::WasapiExclusiveOutput::Options opt;
            opt.deviceId = id;
            return wasapi_->open(opt, engine_);
        };
        auto cfg = tryOpen(choice.id);
        if (!cfg) {
            // No 48 kHz exclusive mode on this output (many HDMI soundbars/TVs and Bluetooth only run at 44.1 kHz),
            // or another app holds it exclusively. Play it in shared mode instead: the engine stays at 48 kHz and
            // Windows converts. Same device the person chose (or Windows' default) - never a different one.
            const auto code = static_cast<unsigned long>(cfg.error().code);
            wasapi_->close();
            wasapi_.reset();
            return openShared(choice.id, code == 0x8889000Aul ? "held by another app" : "no 48 kHz exclusive mode");
        }
        if (!installMixerOnce()) { problem_ = "could not install mixer"; status_.alert = true; refreshLine2(); return false; }
        if (auto s = wasapi_->start(); !s) {
            std::snprintf(buf, sizeof(buf), "WASAPI start failed at %s (0x%08lX)", s.error().stage, static_cast<unsigned long>(s.error().code));
            problem_ = buf;
            status_.alert = true;
            refreshLine2();
            return false;
        }
        std::snprintf(buf, sizeof(buf), "WASAPI exclusive \xC2\xB7 %s \xC2\xB7 %u Hz \xC2\xB7 %u frames = %.2f ms \xC2\xB7 out %.2f ms",
                      toUtf8(wasapi_->formatName()).c_str(), cfg->sampleRate, cfg->blockFrames,
                      1000.0 * cfg->blockFrames / cfg->sampleRate, cfg->outputLatencyMs);
        status_.line1 = buf;
        status_.driver = "WASAPI EXCLUSIVE";
        status_.sampleRate = cfg->sampleRate;
        status_.blockFrames = cfg->blockFrames;
        status_.outLatencyMs = cfg->outputLatencyMs;
    } else if (choice.kind == DeviceChoice::Kind::WasapiShared) {
        return openShared(choice.id, "");
    } else if (choice.kind == DeviceChoice::Kind::Offline) {
        offline_ = std::make_unique<io::OfflineStream>();
        const io::StreamConfig cfg = offline_->open({}, engine_);
        if (!installMixerOnce()) { problem_ = "could not install mixer"; status_.alert = true; refreshLine2(); return false; }
        offline_->start();
        std::snprintf(buf, sizeof(buf), "Offline clock (no sound card) \xC2\xB7 %u Hz \xC2\xB7 %u frames = %.2f ms", cfg.sampleRate,
                      cfg.blockFrames, 1000.0 * cfg.blockFrames / cfg.sampleRate);
        status_.line1 = buf;
        status_.driver = "OFFLINE CLOCK";
        status_.sampleRate = cfg.sampleRate;
        status_.blockFrames = cfg.blockFrames;
        status_.outLatencyMs = -1.0;
    } else {
#if defined(HC_HAS_ASIO_SDK)
        asio_ = std::make_unique<io::AsioDuplex>();
        io::AsioDuplex::Options opt;
        opt.driverName = choice.id;
        auto cfg = asio_->open(opt, engine_);
        if (!cfg) {
            std::snprintf(buf, sizeof(buf), "ASIO open failed at %s (%ld)", cfg.error().stage, cfg.error().code);
            problem_ = buf;
            asio_.reset();
            status_.alert = true;
            status_.line1 = "No audio";
            refreshLine2();
            return false;
        }
        if (!installMixerOnce()) { problem_ = "could not install mixer"; status_.alert = true; refreshLine2(); return false; }
        if (auto s = asio_->start(); !s) {
            std::snprintf(buf, sizeof(buf), "ASIO start failed at %s (%ld)", s.error().stage, s.error().code);
            problem_ = buf;
            status_.alert = true;
            refreshLine2();
            return false;
        }
        std::snprintf(buf, sizeof(buf), "ASIO \xC2\xB7 %u Hz \xC2\xB7 %u frames = %.2f ms \xC2\xB7 in %u / out %u ch \xC2\xB7 latency in %.2f / out %.2f ms",
                      cfg->sampleRate, cfg->blockFrames, 1000.0 * cfg->blockFrames / cfg->sampleRate, cfg->numInputs,
                      cfg->numOutputs, cfg->inputLatencyMs, cfg->outputLatencyMs);
        status_.line1 = buf;
        status_.driver = "ASIO";
        status_.sampleRate = cfg->sampleRate;
        status_.blockFrames = cfg->blockFrames;
        status_.outLatencyMs = cfg->outputLatencyMs;
#else
        problem_ = "this build has no ASIO support";
        status_.alert = true;
        refreshLine2();
        return false;
#endif
    }
    status_.running = true;
    refreshLine2();
    return true;
}

bool AudioHost::openShared(const std::wstring& id, const std::string& note) {
    char buf[256];
    shared_ = std::make_unique<io::WasapiSharedOutput>();
    io::WasapiSharedOutput::Options opt;
    opt.deviceId = id;
    auto cfg = shared_->open(opt, engine_);
    if (!cfg) {
        std::snprintf(buf, sizeof(buf), "This output could not be opened (%s, 0x%08lX) - pick another device with the DEVICE button",
                      cfg.error().stage, static_cast<unsigned long>(cfg.error().code));
        problem_ = buf;
        shared_.reset();
        status_.alert = true;
        status_.line1 = "No audio";
        refreshLine2();
        return false;
    }
    if (!installMixerOnce()) { problem_ = "could not install mixer"; status_.alert = true; refreshLine2(); return false; }
    if (auto s = shared_->start(); !s) {
        std::snprintf(buf, sizeof(buf), "WASAPI shared start failed at %s (0x%08lX)", s.error().stage, static_cast<unsigned long>(s.error().code));
        problem_ = buf;
        status_.alert = true;
        refreshLine2();
        return false;
    }
    const unsigned mix = shared_->deviceMixRate();
    std::snprintf(buf, sizeof(buf), "WASAPI shared%s%s \xC2\xB7 engine %u Hz \xC2\xB7 device %u Hz (Windows converts) \xC2\xB7 out %.2f ms",
                  note.empty() ? "" : " - ", note.c_str(), cfg->sampleRate, mix, cfg->outputLatencyMs);
    status_.line1 = buf;
    status_.driver = "WASAPI SHARED";
    status_.sampleRate = cfg->sampleRate;
    status_.blockFrames = cfg->blockFrames;
    status_.outLatencyMs = cfg->outputLatencyMs;
    status_.running = true;
    refreshLine2();
    return true;
}

void AudioHost::refreshLine2() {
    char buf[256];
    status_.loadAvg  = lastTelemetry_.loadAvg;
    status_.loadPeak = peakLoad_;
    status_.dropouts = static_cast<std::uint64_t>(lastTelemetry_.overruns) + driverOverloads_;
    if (!problem_.empty()) {
        status_.line2 = problem_;
        return;
    }
    std::snprintf(buf, sizeof(buf), "CPU %.1f%% avg \xC2\xB7 %.1f%% peak \xC2\xB7 overruns %u \xC2\xB7 driver dropouts %u \xC2\xB7 source: %s",
                  lastTelemetry_.loadAvg * 100.0f, peakLoad_ * 100.0f, lastTelemetry_.overruns, driverOverloads_,
                  mixer_->source() == engine::MixerProcessor::Source::TestTones ? "13 test tones"
                  : mixer_->source() == engine::MixerProcessor::Source::Stems  ? "loaded stems"
                                                                                : "hardware inputs");
    status_.line2 = buf;
}

void AudioHost::poll() {
    engine::Telemetry t;
    bool got = false;
    while (engine_.popTelemetry(t)) {
        lastTelemetry_ = t;
        peakLoad_ = std::max(peakLoad_ * 0.98f, t.loadPeak);
        got = true;
    }
    engine_.collectGarbage();
    mixer_->collectRetired();   // stems the audio thread let go of are freed here, never on the audio thread

    std::uint32_t ev = 0;
    if (wasapi_) ev |= wasapi_->takeEvents();
    if (shared_) ev |= shared_->takeEvents();
#if defined(HC_HAS_ASIO_SDK)
    if (asio_) ev |= asio_->takeEvents();
#endif
    using namespace io::StreamEvent;
    if (ev & (ResetRequest | SampleRateMoved | ResyncRequest)) {
        const DeviceChoice again = current_;
        open(again);   // driver asked for a restart (buffer size / rate changed in its panel)
        return;
    }
    if (ev & (DeviceLost | ThreadFault)) {
        close();
        problem_ = (ev & DeviceLost) ? "Device disconnected - pick a device with the DEVICE button"
                                     : "Audio device stopped responding - pick a device with the DEVICE button";
        status_.alert = true;
        refreshLine2();
        return;
    }
    if (ev & Overload) { ++driverOverloads_; got = true; }
    if (got && ++framesSinceLine2_ >= 2) { framesSinceLine2_ = 0; refreshLine2(); }
}

} // namespace hc::app

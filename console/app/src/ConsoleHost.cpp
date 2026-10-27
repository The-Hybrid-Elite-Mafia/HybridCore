#include "ConsoleHost.hpp"

#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace hem::console {

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

ConsoleHost::ConsoleHost(hc::console::ConsoleRig& rig) {
    auto p = std::make_unique<hc::console::ConsoleProcessor>(rig);
    player_ = p.get();
    pending_ = std::move(p);
}

ConsoleHost::~ConsoleHost() { close(); }

std::vector<Device> ConsoleHost::listDevices() {
    std::vector<Device> out;
    out.push_back({Device::Kind::Wasapi, L"", "DEFAULT OUTPUT"});
    if (auto eps = hc::io::enumerateWasapi()) {
        for (const auto& ep : *eps) {
            if (ep.flow != hc::io::Flow::Render) continue;
            if (!(ep.excl48kFloat32 || ep.excl48kInt24in32 || ep.excl48kInt16)) continue;
            out.push_back({Device::Kind::Wasapi, ep.id, toUtf8(ep.name)});
        }
    }
#if defined(HC_HAS_ASIO_SDK)
    for (const auto& d : hc::io::enumerateAsioDrivers())
        if (!d.dllPath.empty()) out.push_back({Device::Kind::Asio, d.name, "ASIO  " + toUtf8(d.name)});
#endif
    out.push_back({Device::Kind::Offline, L"offline", "NO SOUND (OFFLINE CLOCK)"});
    return out;
}

bool ConsoleHost::installOnce() {
    if (!pending_) return true;
    return engine_.install(pending_);   // empties pending_ on success
}

void ConsoleHost::close() noexcept {
    if (wasapi_) { wasapi_->close(); wasapi_.reset(); }
    if (offline_) { offline_->close(); offline_.reset(); }
#if defined(HC_HAS_ASIO_SDK)
    if (asio_) { asio_->close(); asio_.reset(); }
#endif
    engine_.collectGarbage();
    status_.running = false;
}

bool ConsoleHost::open(const Device& d) {
    close();
    current_ = d;
    status_ = {};
    overloads_ = 0;
    char buf[256];
    if (d.kind == Device::Kind::Wasapi) {
        auto tryOpen = [&](const std::wstring& id) {
            wasapi_ = std::make_unique<hc::io::WasapiExclusiveOutput>();
            hc::io::WasapiExclusiveOutput::Options opt;
            opt.deviceId = id;
            return wasapi_->open(opt, engine_);
        };
        std::string name = d.label;
        auto cfg = tryOpen(d.id);
        if (!cfg && d.id.empty()) {
            // The Windows default output may have no 48 kHz exclusive mode: use the first output that has one.
            if (auto eps = hc::io::enumerateWasapi()) {
                for (const auto& ep : *eps) {
                    if (ep.flow != hc::io::Flow::Render || ep.id.empty()) continue;
                    if (!(ep.excl48kFloat32 || ep.excl48kInt24in32 || ep.excl48kInt16)) continue;
                    wasapi_->close();
                    auto again = tryOpen(ep.id);
                    if (again) { cfg = std::move(again); name = toUtf8(ep.name); break; }
                }
            }
        }
        if (!cfg) {
            const auto code = static_cast<unsigned long>(cfg.error().code);
            if (code == 0x88890008ul) status_.problem = "This output can't play 48 kHz exclusive: click DEVICE for the next one";
            else if (code == 0x8889000Aul) status_.problem = "Another app is using this output: close it or click DEVICE";
            else { std::snprintf(buf, sizeof buf, "Output failed to open (%s, 0x%08lX): click DEVICE", cfg.error().stage, code); status_.problem = buf; }
            wasapi_.reset();
            status_.alert = true;
            status_.line = "NO SOUND  " + d.label;
            return false;
        }
        if (!installOnce()) { status_.problem = "Could not start the console engine"; status_.alert = true; return false; }
        if (auto s = wasapi_->start(); !s) {
            std::snprintf(buf, sizeof buf, "Output failed to start (%s, 0x%08lX)", s.error().stage, static_cast<unsigned long>(s.error().code));
            status_.problem = buf; status_.alert = true; status_.line = "NO SOUND  " + d.label;
            return false;
        }
        status_.sampleRate = cfg->sampleRate; status_.blockFrames = cfg->blockFrames; status_.outLatencyMs = cfg->outputLatencyMs;
        std::snprintf(buf, sizeof buf, "%s  %.1f ms", name.c_str(), cfg->outputLatencyMs);
        status_.line = buf;
    } else if (d.kind == Device::Kind::Offline) {
        offline_ = std::make_unique<hc::io::OfflineStream>();
        const hc::io::StreamConfig cfg = offline_->open({}, engine_);
        if (!installOnce()) { status_.problem = "Could not start the console engine"; status_.alert = true; return false; }
        offline_->start();
        status_.sampleRate = cfg.sampleRate; status_.blockFrames = cfg.blockFrames;
        status_.line = "NO SOUND (OFFLINE CLOCK)";
    } else {
#if defined(HC_HAS_ASIO_SDK)
        asio_ = std::make_unique<hc::io::AsioDuplex>();
        hc::io::AsioDuplex::Options opt;
        opt.driverName = d.id;
        auto cfg = asio_->open(opt, engine_);
        if (!cfg) {
            std::snprintf(buf, sizeof buf, "ASIO failed to open (%s, %ld): click DEVICE", cfg.error().stage, cfg.error().code);
            status_.problem = buf; status_.alert = true; status_.line = "NO SOUND  " + d.label;
            asio_.reset();
            return false;
        }
        if (!installOnce()) { status_.problem = "Could not start the console engine"; status_.alert = true; return false; }
        if (auto s = asio_->start(); !s) {
            std::snprintf(buf, sizeof buf, "ASIO failed to start (%s, %ld)", s.error().stage, s.error().code);
            status_.problem = buf; status_.alert = true; status_.line = "NO SOUND  " + d.label;
            return false;
        }
        status_.sampleRate = cfg->sampleRate; status_.blockFrames = cfg->blockFrames; status_.outLatencyMs = cfg->outputLatencyMs;
        std::snprintf(buf, sizeof buf, "%s  %.1f ms", d.label.c_str(), cfg->outputLatencyMs);
        status_.line = buf;
#else
        status_.problem = "This build has no ASIO";
        status_.alert = true;
        return false;
#endif
    }
    if (status_.sampleRate != 0 && status_.sampleRate != 48000) {
        std::snprintf(buf, sizeof buf, "The device runs at %.0f Hz: the console is built for 48 kHz", status_.sampleRate);
        status_.problem = buf;
    }
    status_.running = true;
    return true;
}

void ConsoleHost::poll() {
    hc::engine::Telemetry t;
    while (engine_.popTelemetry(t)) {
        status_.load = t.loadAvg;
        status_.loadPeak = std::max(status_.loadPeak * 0.98f, t.loadPeak);
        status_.dropouts = static_cast<std::uint64_t>(t.overruns) + overloads_;
    }
    engine_.collectGarbage();
    player_->collectRetired();   // stems the audio thread let go of are freed here, never on the audio thread
    std::uint32_t ev = 0;
    if (wasapi_) ev |= wasapi_->takeEvents();
#if defined(HC_HAS_ASIO_SDK)
    if (asio_) ev |= asio_->takeEvents();
#endif
    using namespace hc::io::StreamEvent;
    if (ev & (ResetRequest | SampleRateMoved | ResyncRequest)) { const Device again = current_; open(again); return; }
    if (ev & (DeviceLost | ThreadFault)) {
        close();
        status_.problem = (ev & DeviceLost) ? "The output device was disconnected: click DEVICE" : "The output device stopped responding: click DEVICE";
        status_.alert = true;
        status_.line = "NO SOUND";
        return;
    }
    if (ev & Overload) ++overloads_;
}

} // namespace hem::console

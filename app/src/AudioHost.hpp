#pragma once
// =============================================================================
//  AudioHost — owns the engine, the 13-stem MixerProcessor and the current
//  hardware stream (WASAPI exclusive or ASIO). Message-thread only.
// =============================================================================
#include "hybrid/engine/Engine.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/io/WasapiExclusiveOutput.hpp"
#if defined(HC_HAS_ASIO_SDK)
#include "hybrid/io/AsioDuplex.hpp"
#endif

#include <memory>
#include <string>
#include <vector>

namespace hc::app {

struct DeviceChoice {
    enum class Kind : std::uint8_t { Wasapi, Asio };
    Kind         kind = Kind::Wasapi;
    std::wstring id;      // WASAPI endpoint id ("" = default output) or ASIO driver name
    std::string  label;   // UTF-8 for the UI
};

struct HostStatus {
    bool        running = false;
    bool        alert   = false;
    std::string device;     // short label
    std::string line1;      // format / buffer / latency
    std::string line2;      // CPU / overruns / problems
};

class AudioHost {
public:
    AudioHost();
    ~AudioHost();
    AudioHost(const AudioHost&)            = delete;
    AudioHost& operator=(const AudioHost&) = delete;

    [[nodiscard]] dsp::StemMatrix& matrix() noexcept { return mixer_->matrix(); }
    [[nodiscard]] engine::MixerProcessor::Source source() const noexcept { return mixer_->source(); }
    void setSource(engine::MixerProcessor::Source s) noexcept { mixer_->setSource(s); }

    [[nodiscard]] static std::vector<DeviceChoice> listDevices();

    bool open(const DeviceChoice& choice);   // closes the current stream first
    void close() noexcept;
    [[nodiscard]] const DeviceChoice& current() const noexcept { return current_; }

    // Call every UI frame: drains telemetry, handles device loss / driver resets.
    void poll();
    [[nodiscard]] const HostStatus& status() const noexcept { return status_; }

private:
    bool installMixerOnce();
    void refreshLine2();

    engine::Engine                          engine_;
    engine::MixerProcessor*                 mixer_ = nullptr;          // owned by engine_ once installed
    std::unique_ptr<engine::AudioProcessor> pendingMixer_;
    std::unique_ptr<io::WasapiExclusiveOutput> wasapi_;
#if defined(HC_HAS_ASIO_SDK)
    std::unique_ptr<io::AsioDuplex>         asio_;
#endif
    DeviceChoice   current_;
    HostStatus     status_;
    engine::Telemetry lastTelemetry_{};
    float          peakLoad_ = 0.0f;
    unsigned       framesSinceLine2_ = 0;
    unsigned       driverOverloads_  = 0;
    std::string    problem_;
};

[[nodiscard]] std::string toUtf8(const std::wstring& w);
[[nodiscard]] std::wstring fromUtf8(const std::string& s);

} // namespace hc::app

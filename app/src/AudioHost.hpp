#pragma once
// =============================================================================
//  AudioHost — owns the engine, the 13-stem MixerProcessor and the current
//  hardware stream (WASAPI exclusive or ASIO). Message-thread only.
// =============================================================================
#include "hybrid/engine/Engine.hpp"
#include "hybrid/engine/MixerProcessor.hpp"
#include "hybrid/io/OfflineStream.hpp"
#include "hybrid/io/WasapiExclusiveOutput.hpp"
#if defined(HC_HAS_ASIO_SDK)
#include "hybrid/io/AsioDuplex.hpp"
#endif

#include <memory>
#include <string>
#include <vector>

namespace hc::app {

struct DeviceChoice {
    enum class Kind : std::uint8_t { Wasapi, Asio, Offline };
    Kind         kind = Kind::Wasapi;
    std::wstring id;      // WASAPI endpoint id ("" = default output) or ASIO driver name
    std::string  label;   // UTF-8 for the UI ("WASAPI · Speakers")
    std::string  name;    // device name alone
};

struct HostStatus {
    bool        running = false;
    bool        alert   = false;
    std::string device;     // short label
    std::string line1;      // format / buffer / latency
    std::string line2;      // CPU / overruns / problems
    // Live numbers for the glass display (valid while running).
    std::string   driver;              // "WASAPI EXCLUSIVE", "ASIO", "OFFLINE CLOCK"
    std::string   deviceName;
    double        sampleRate   = 0.0;
    std::uint32_t blockFrames  = 0;
    double        outLatencyMs = -1.0;  // as reported by the driver; -1 = none (offline clock)
    float         loadAvg      = 0.0f;  // fraction
    float         loadPeak     = 0.0f;
    std::uint64_t dropouts     = 0;     // engine overruns + driver overload reports
};

class AudioHost {
public:
    AudioHost();
    ~AudioHost();
    AudioHost(const AudioHost&)            = delete;
    AudioHost& operator=(const AudioHost&) = delete;

    [[nodiscard]] dsp::StemMatrix& matrix() noexcept { return mixer_->matrix(); }
    [[nodiscard]] dsp::BusLimiter& limiter() noexcept { return mixer_->limiter(); }
    [[nodiscard]] forge::ForgeEngine& forge() noexcept { return mixer_->forge(); }
    [[nodiscard]] bool forgeActive() const noexcept { return mixer_->forgeActive(); }
    // ANALYZER feed (null until a device has been opened once).
    [[nodiscard]] analysis::ScopeTap* scopeTap() noexcept { return mixer_->scopeTap(); }
    [[nodiscard]] engine::MixerProcessor::Source source() const noexcept { return mixer_->source(); }
    void setSource(engine::MixerProcessor::Source s) noexcept { mixer_->setSource(s); }

    // Stems: hand a loaded stem to the audio thread (nullptr clears the slot). False = queue full, retry.
    [[nodiscard]] bool submitStem(std::uint32_t slot, std::unique_ptr<engine::StemAudio>& audio) noexcept { return mixer_->submitStem(slot, audio); }
    // Transport (stems).
    void play() noexcept { mixer_->play(); }
    void pause() noexcept { mixer_->pause(); }
    void stop() noexcept { mixer_->stop(); }
    void seek(std::uint64_t frame) noexcept { mixer_->seek(frame); }
    void setLoop(bool on) noexcept { mixer_->setLoop(on); }
    [[nodiscard]] bool loop() const noexcept { return mixer_->loop(); }
    [[nodiscard]] bool wantsPlay() const noexcept { return mixer_->wantsPlay(); }
    [[nodiscard]] bool playing() const noexcept { return mixer_->playing(); }
    [[nodiscard]] std::uint64_t stemPlayhead() const noexcept { return mixer_->stemPlayhead(); }
    [[nodiscard]] std::uint64_t stemSongFrames() const noexcept { return mixer_->stemSongFrames(); }

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
    std::unique_ptr<io::OfflineStream>      offline_;
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

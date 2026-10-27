#pragma once
// =============================================================================
//  ConsoleHost — the HEM Digital Console's audio host: owns the engine, the ConsoleProcessor (the rig + 15
//  file slots + transport) and the current hardware stream (WASAPI exclusive, ASIO when the build has the
//  SDK, or the offline clock). Message thread only.
// =============================================================================
#include "hybrid/console/ConsolePlayer.hpp"
#include "hybrid/engine/Engine.hpp"
#include "hybrid/io/OfflineStream.hpp"
#include "hybrid/io/WasapiExclusiveOutput.hpp"
#if defined(HC_HAS_ASIO_SDK)
#include "hybrid/io/AsioDuplex.hpp"
#endif

#include <memory>
#include <string>
#include <vector>

namespace hem::console {

struct Device {
    enum class Kind : std::uint8_t { Wasapi, Asio, Offline };
    Kind kind = Kind::Wasapi;
    std::wstring id;      // WASAPI endpoint id ("" = default) or ASIO driver name
    std::string label;    // UTF-8 for the screen
};

struct HostStatus {
    bool running = false, alert = false;
    std::string line;       // short device line for the DEVICE button
    std::string problem;    // what went wrong (empty = fine)
    double sampleRate = 0, outLatencyMs = -1;
    std::uint32_t blockFrames = 0;
    float load = 0, loadPeak = 0;
    std::uint64_t dropouts = 0;
};

class ConsoleHost {
public:
    explicit ConsoleHost(hc::console::ConsoleRig& rig);
    ~ConsoleHost();
    ConsoleHost(const ConsoleHost&) = delete;
    ConsoleHost& operator=(const ConsoleHost&) = delete;

    [[nodiscard]] static std::vector<Device> listDevices();
    bool open(const Device& d);
    void close() noexcept;
    void poll();   // every UI frame: telemetry, device loss / driver resets, frees released stems
    [[nodiscard]] const HostStatus& status() const noexcept { return status_; }
    [[nodiscard]] const Device& current() const noexcept { return current_; }
    [[nodiscard]] hc::console::ConsoleProcessor& player() noexcept { return *player_; }

private:
    bool installOnce();
    hc::engine::Engine engine_;
    hc::console::ConsoleProcessor* player_ = nullptr;          // owned by engine_ once installed
    std::unique_ptr<hc::engine::AudioProcessor> pending_;
    std::unique_ptr<hc::io::WasapiExclusiveOutput> wasapi_;
    std::unique_ptr<hc::io::OfflineStream> offline_;
#if defined(HC_HAS_ASIO_SDK)
    std::unique_ptr<hc::io::AsioDuplex> asio_;
#endif
    Device current_;
    HostStatus status_;
    unsigned overloads_ = 0;
};

[[nodiscard]] std::string toUtf8(const std::wstring& w);
[[nodiscard]] std::wstring fromUtf8(const std::string& s);

} // namespace hem::console

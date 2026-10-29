#pragma once
// =============================================================================
//  WASAPI shared-mode, event-driven render stream. The engine always runs at
//  48 kHz float; Windows converts to whatever the device mixes at
//  (AUTOCONVERTPCM + SRC_DEFAULT_QUALITY). This is the path for outputs that
//  have no 48 kHz exclusive mode: HDMI sinks, Bluetooth headphones, devices
//  another app already holds.
//
//  Each period the callback gets the free space in the device buffer, so
//  `frames` varies between calls (never above StreamConfig::blockFrames).
//  Own RT thread (MMCSS Pro Audio, P-core pinned); the only wait is the event.
//  open/start/stop/close/takeEvents: message thread only.
// =============================================================================
#include "hybrid/io/AudioCallback.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace hc::io {

class WasapiSharedOutput {
public:
    struct Options {
        std::wstring deviceId;          // empty -> default render endpoint
        double       bufferMs = 20.0;   // shared-mode buffer the engine fills ahead
    };

    WasapiSharedOutput();
    ~WasapiSharedOutput();
    WasapiSharedOutput(const WasapiSharedOutput&)            = delete;
    WasapiSharedOutput& operator=(const WasapiSharedOutput&) = delete;

    [[nodiscard]] std::expected<StreamConfig, BackendError> open(const Options& options, AudioCallback& callback);
    [[nodiscard]] std::expected<void, BackendError>         start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint32_t takeEvents() noexcept;
    [[nodiscard]] bool          isRunning() const noexcept;
    /** The rate Windows mixes this device at (the engine itself stays at 48 kHz). */
    [[nodiscard]] std::uint32_t deviceMixRate() const noexcept;
    [[nodiscard]] bool          rtThreadUsesMmcss() const noexcept;
    [[nodiscard]] bool          rtThreadPinned() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hc::io

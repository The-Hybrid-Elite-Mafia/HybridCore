#pragma once
// =============================================================================
//  WASAPI exclusive-mode, event-driven render stream at 48 kHz.
//  Owns its own RT thread (MMCSS Pro Audio, P-core pinned). The only wait in
//  that thread is on the hardware period event.
//  open/start/stop/close/takeEvents: message thread only.
// =============================================================================
#include "hybrid/io/AudioCallback.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace hc::io {

class WasapiExclusiveOutput {
public:
    struct Options {
        std::wstring deviceId;        // empty -> default render endpoint
        double       periodMs = 0.0;  // 0 -> device minimum period
    };

    WasapiExclusiveOutput();
    ~WasapiExclusiveOutput();
    WasapiExclusiveOutput(const WasapiExclusiveOutput&)            = delete;
    WasapiExclusiveOutput& operator=(const WasapiExclusiveOutput&) = delete;

    [[nodiscard]] std::expected<StreamConfig, BackendError> open(const Options& options, AudioCallback& callback);
    [[nodiscard]] std::expected<void, BackendError>         start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint32_t  takeEvents() noexcept;
    [[nodiscard]] bool           isRunning() const noexcept;
    [[nodiscard]] const wchar_t* formatName() const noexcept;
    [[nodiscard]] bool           rtThreadUsesMmcss() const noexcept;
    [[nodiscard]] bool           rtThreadPinned() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hc::io

#pragma once
// =============================================================================
//  ASIO full-duplex host at 48 kHz. Requires the Steinberg ASIO SDK
//  (HC_HAS_ASIO_SDK). ASIO is a single-instance API: one open stream per process.
//  The driver owns the callback thread; this host only does lock-free work on it.
//  open/start/stop/close/takeEvents: message thread only, inside an STA
//  (ComApartment(Apartment::SingleThreaded)) that pumps window messages.
// =============================================================================
#if !defined(HC_HAS_ASIO_SDK)
#error "AsioDuplex.hpp requires the Steinberg ASIO SDK (place it in external/asiosdk)."
#endif

#include "hybrid/io/AudioCallback.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <string>

namespace hc::io {

class AsioDuplex {
public:
    struct Options {
        std::wstring  driverName;            // registry key name, as listed by enumerateAsioDrivers()
        std::uint32_t bufferFrames = 0;      // 0 -> driver preferred size
        std::uint32_t maxInputs    = 64;
        std::uint32_t maxOutputs   = 64;
    };

    AsioDuplex();
    ~AsioDuplex();
    AsioDuplex(const AsioDuplex&)            = delete;
    AsioDuplex& operator=(const AsioDuplex&) = delete;

    [[nodiscard]] std::expected<StreamConfig, BackendError> open(const Options& options, AudioCallback& callback);
    [[nodiscard]] std::expected<void, BackendError>         start();
    void stop() noexcept;
    void close() noexcept;

    [[nodiscard]] std::uint32_t takeEvents() noexcept;
    [[nodiscard]] bool          isRunning() const noexcept;

    struct Impl;   // public only so the C-style ASIO callbacks can reach it; defined in AsioDuplex.cpp

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace hc::io

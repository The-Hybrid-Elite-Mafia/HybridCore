#pragma once
// =============================================================================
//  hc::io — device + rig discovery. Non-real-time; message thread only.
// =============================================================================
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace hc::io {

// RAII COM apartment. MTA for WASAPI threads; STA on the thread that hosts an ASIO driver.
enum class Apartment : std::uint8_t { MultiThreaded, SingleThreaded };

class ComApartment {
public:
    explicit ComApartment(Apartment kind = Apartment::MultiThreaded) noexcept;
    ~ComApartment();
    ComApartment(const ComApartment&)            = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    [[nodiscard]] bool ok() const noexcept { return ok_; }
private:
    bool ok_     = false;
    bool owns_   = false;
};

enum class Flow : std::uint8_t { Render, Capture };

struct WasapiEndpoint {
    std::wstring  id;
    std::wstring  name;
    Flow          flow              = Flow::Render;
    std::uint32_t mixRate           = 0;
    std::uint16_t mixChannels       = 0;
    bool          excl48kFloat32    = false;
    bool          excl48kInt24in32  = false;
    bool          excl48kInt16      = false;
    double        defaultPeriodMs   = 0.0;
    double        minPeriodMs       = 0.0;
    long          hresult           = 0;   // 0 = endpoint fully probed
};

struct AsioDriverEntry {
    std::wstring name;
    std::wstring clsid;
    std::wstring description;
    std::wstring dllPath;     // empty -> no 64-bit InprocServer32 registered
};

struct CpuReport {
    std::string   brand;
    bool          avx2      = false;
    bool          fma       = false;
    bool          bmi2      = false;
    bool          hybrid    = false;
    std::uint32_t pCores    = 0;
    std::uint32_t pThreads  = 0;
    std::uint32_t eCores    = 0;
    std::uint32_t eThreads  = 0;
    [[nodiscard]] bool isTargetRig() const noexcept {
        return brand.find("i7-14700HX") != std::string::npos;
    }
};

[[nodiscard]] std::expected<std::vector<WasapiEndpoint>, long> enumerateWasapi();
[[nodiscard]] std::vector<AsioDriverEntry>                    enumerateAsioDrivers();
[[nodiscard]] CpuReport                                       probeCpu();

} // namespace hc::io

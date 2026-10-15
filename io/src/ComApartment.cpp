#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>
#include <objbase.h>

namespace hc::io {

ComApartment::ComApartment(Apartment kind) noexcept {
    const DWORD mode = (kind == Apartment::SingleThreaded) ? COINIT_APARTMENTTHREADED
                                                           : COINIT_MULTITHREADED;
    const HRESULT hr = ::CoInitializeEx(nullptr, mode);
    if (SUCCEEDED(hr)) {            // S_OK or S_FALSE: balanced Uninitialize required
        ok_   = true;
        owns_ = true;
    } else if (hr == RPC_E_CHANGED_MODE) {
        ok_   = true;               // Thread already in the other apartment; usable, not ours to tear down.
        owns_ = false;
    }
}

ComApartment::~ComApartment() {
    if (owns_) ::CoUninitialize();
}

} // namespace hc::io

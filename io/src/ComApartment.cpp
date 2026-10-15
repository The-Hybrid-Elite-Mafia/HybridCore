#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace hc::io {

void configureWideConsole() noexcept {
    // Console -> UTF-16 via WriteConsoleW; pipe/file (CI logs) -> UTF-8 bytes.
    const HANDLE h       = ::GetStdHandle(STD_OUTPUT_HANDLE);
    const bool   console = h && h != INVALID_HANDLE_VALUE && ::GetFileType(h) == FILE_TYPE_CHAR;
    ::_setmode(::_fileno(stdout), console ? _O_U16TEXT : _O_U8TEXT);
}

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

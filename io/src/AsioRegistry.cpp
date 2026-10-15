#include "hybrid/io/DeviceProbe.hpp"

#include <windows.h>
#include <cwchar>
#include <iterator>

namespace {

std::wstring readRegString(HKEY root, const wchar_t* subKey, const wchar_t* value) {
    DWORD bytes = 0;
    if (::RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t))
        return {};

    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, subKey, value, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS)
        return {};
    s.resize(::wcsnlen(s.c_str(), s.size()));
    return s;
}

} // namespace

namespace hc::io {

// ASIO drivers self-register under HKLM\SOFTWARE\ASIO\<name> { CLSID, Description }.
// Only the native 64-bit view is read: 32-bit drivers cannot load into this process.
std::vector<AsioDriverEntry> enumerateAsioDrivers() {
    std::vector<AsioDriverEntry> out;

    HKEY asioRoot = nullptr;
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0,
                        KEY_READ | KEY_WOW64_64KEY, &asioRoot) != ERROR_SUCCESS)
        return out;

    for (DWORD index = 0;; ++index) {
        wchar_t name[256];
        DWORD   nameLen = static_cast<DWORD>(std::size(name));
        const LSTATUS st = ::RegEnumKeyExW(asioRoot, index, name, &nameLen,
                                           nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;

        AsioDriverEntry e{};
        e.name        = name;
        e.clsid       = readRegString(asioRoot, name, L"CLSID");
        e.description = readRegString(asioRoot, name, L"Description");

        if (!e.clsid.empty()) {
            const std::wstring inproc = L"CLSID\\" + e.clsid + L"\\InprocServer32";
            e.dllPath = readRegString(HKEY_CLASSES_ROOT, inproc.c_str(), nullptr);
        }
        out.push_back(std::move(e));
    }

    ::RegCloseKey(asioRoot);
    return out;
}

} // namespace hc::io

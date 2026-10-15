#include "hybrid/io/DeviceProbe.hpp"
#include "WasapiFormat.hpp"

#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <propidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using hc::io::wasapi::exclusiveSupports;
using hc::io::wasapi::kSubtypeIeeeFloat;
using hc::io::wasapi::kSubtypePcm;
using hc::io::wasapi::makeFormat;

namespace {

// Defined locally to avoid INITGUID / ksuser.lib link-order issues.
constexpr PROPERTYKEY kPkeyFriendlyName{
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

constexpr double kHnsPerMs = 10'000.0;

void probeEndpoint(IMMDevice* dev, hc::io::WasapiEndpoint& ep) {
    LPWSTR rawId = nullptr;
    if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
        ep.id = rawId;
        ::CoTaskMemFree(rawId);
    }

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT pv;
        ::PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            ep.name = pv.pwszVal;
        ::PropVariantClear(&pv);
    }

    ComPtr<IAudioClient> client;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) { ep.hresult = hr; return; }

    WORD  channels = 2;
    DWORD mask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { ep.hresult = hr; return; }

    ep.mixRate     = mix->nSamplesPerSec;
    ep.mixChannels = mix->nChannels;
    if (mix->nChannels > 0) channels = mix->nChannels;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    } else if (channels != 2) {
        mask = 0;  // Unknown layout: let the driver map channels positionally.
    }
    ::CoTaskMemFree(mix);

    ep.excl48kFloat32   = exclusiveSupports(client.Get(), makeFormat(channels, mask, 32, 32, kSubtypeIeeeFloat));
    ep.excl48kInt24in32 = exclusiveSupports(client.Get(), makeFormat(channels, mask, 32, 24, kSubtypePcm));
    ep.excl48kInt16     = exclusiveSupports(client.Get(), makeFormat(channels, mask, 16, 16, kSubtypePcm));

    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    hr = client->GetDevicePeriod(&defPeriod, &minPeriod);
    if (SUCCEEDED(hr)) {
        ep.defaultPeriodMs = static_cast<double>(defPeriod) / kHnsPerMs;
        ep.minPeriodMs     = static_cast<double>(minPeriod) / kHnsPerMs;
    } else {
        ep.hresult = hr;
    }
}

} // namespace

namespace hc::io {

std::expected<std::vector<WasapiEndpoint>, long> enumerateWasapi() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return std::unexpected(static_cast<long>(hr));

    std::vector<WasapiEndpoint> out;
    for (const EDataFlow df : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> collection;
        hr = enumerator->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, &collection);
        if (FAILED(hr)) return std::unexpected(static_cast<long>(hr));

        UINT count = 0;
        if (FAILED(collection->GetCount(&count))) continue;

        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> dev;
            if (FAILED(collection->Item(i, &dev))) continue;
            WasapiEndpoint ep{};
            ep.flow = (df == eRender) ? Flow::Render : Flow::Capture;
            probeEndpoint(dev.Get(), ep);
            out.push_back(std::move(ep));
        }
    }
    return out;
}

} // namespace hc::io

#pragma once
// Shared WASAPI format helpers (internal to hc::io).
#include "hybrid/core/Config.hpp"

#include <windows.h>
#include <objbase.h>
#include <audioclient.h>
#include <mmreg.h>

namespace hc::io::wasapi {

// Defined locally to avoid INITGUID / ksuser.lib link-order issues.
inline constexpr GUID kSubtypeIeeeFloat{
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
inline constexpr GUID kSubtypePcm{
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum class SampleFormat : unsigned char { Float32, S24In32, S16, S24Packed };

struct FormatSpec {
    SampleFormat   format;
    WORD           containerBits;
    WORD           validBits;
    const GUID*    subtype;
    const wchar_t* name;
};

// Preference order for exclusive mode.
inline constexpr FormatSpec kExclusiveFormats[] = {
    {SampleFormat::Float32,   32, 32, &kSubtypeIeeeFloat, L"float32"},
    {SampleFormat::S24In32,   32, 24, &kSubtypePcm,       L"int24-in-32"},
    {SampleFormat::S16,       16, 16, &kSubtypePcm,       L"int16"},
    {SampleFormat::S24Packed, 24, 24, &kSubtypePcm,       L"int24-packed"},
};

inline WAVEFORMATEXTENSIBLE makeFormat(WORD channels, DWORD mask, WORD containerBits,
                                       WORD validBits, const GUID& subtype) noexcept {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels       = channels;
    f.Format.nSamplesPerSec  = hc::kSampleRate;
    f.Format.wBitsPerSample  = containerBits;
    f.Format.nBlockAlign     = static_cast<WORD>(channels * (containerBits / 8));
    f.Format.nAvgBytesPerSec = hc::kSampleRate * f.Format.nBlockAlign;
    f.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = validBits;
    f.dwChannelMask          = mask;
    f.SubFormat              = subtype;
    return f;
}

inline WAVEFORMATEXTENSIBLE makeFormat(WORD channels, DWORD mask, const FormatSpec& s) noexcept {
    return makeFormat(channels, mask, s.containerBits, s.validBits, *s.subtype);
}

inline bool exclusiveSupports(IAudioClient* client, const WAVEFORMATEXTENSIBLE& f) noexcept {
    return client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                     reinterpret_cast<const WAVEFORMATEX*>(&f), nullptr) == S_OK;
}

// Channel count + mask from the device mix format. Caller frees nothing.
inline void layoutFromMix(IAudioClient* client, WORD& channels, DWORD& mask) noexcept {
    channels = 2;
    mask     = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(client->GetMixFormat(&mix)) || !mix) return;
    if (mix->nChannels > 0) channels = mix->nChannels;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    } else if (channels != 2) {
        mask = 0;
    }
    ::CoTaskMemFree(mix);
}

} // namespace hc::io::wasapi

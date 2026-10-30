"""HEM Hiss — independent measurements (Python, numpy only).

These do NOT share code with the C++ side. They are written separately from the C++ metrics on purpose,
so that if the two agree, it is not because one copied the other.
Used three ways:
  * hiss_compare.py   checks the C++ results against these numbers
  * hiss_report.py    builds pictures and the plain-words report
  * python hiss_analyze.py file.wav     "Stage 0": looks at a real file and prints where the always-there level sits
"""
import json
import struct
import sys

import numpy as np


def read_wav(path):
    """Returns (rate, left, right, info). Samples are float64 in -1..1."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise ValueError(path + " is not a WAV file")
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        tag, ln = d[pos:pos + 4], struct.unpack("<I", d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + ln]
        if tag == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
            if fmt[0] == 0xFFFE and len(body) >= 26:
                fmt = (struct.unpack("<H", body[24:26])[0],) + fmt[1:]
        elif tag == b"data":
            data = body
        pos += 8 + ln + (ln & 1)
    tag, ch, rate, _, _, bits = fmt
    if tag == 3 and bits == 32:
        x = np.frombuffer(data[: len(data) // 4 * 4], dtype="<f4").astype(np.float64)
    elif tag == 1 and bits == 16:
        x = np.frombuffer(data[: len(data) // 2 * 2], dtype="<i2").astype(np.float64) / 32768.0
    elif tag == 1 and bits == 24:
        b = np.frombuffer(data[: len(data) // 3 * 3], dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    elif tag == 1 and bits == 32:
        x = np.frombuffer(data[: len(data) // 4 * 4], dtype="<i4").astype(np.float64) / 2147483648.0
    else:
        raise ValueError("unsupported WAV format")
    x = x[: len(x) // ch * ch].reshape(-1, ch)
    left = x[:, 0]
    right = x[:, 1] if ch == 2 else x[:, 0]
    return rate, left, right, {"channels": ch, "bits": bits, "float": tag == 3}


def mid_of(l, r):
    return 0.5 * (l + r)


def stft_power(x, rate, nfft=8192, hop=None):
    """|X|^2 / sum(w^2) per frame (Hann). Returns (freqs, power[frames, bins])."""
    hop = hop or nfft // 2
    w = np.hanning(nfft)
    n = (len(x) - nfft) // hop + 1
    if n < 1:
        return np.fft.rfftfreq(nfft, 1.0 / rate), np.zeros((0, nfft // 2 + 1))
    idx = np.arange(nfft)[None, :] + hop * np.arange(n)[:, None]
    fr = x[idx] * w
    p = np.abs(np.fft.rfft(fr, axis=1)) ** 2 / np.sum(w ** 2)
    return np.fft.rfftfreq(nfft, 1.0 / rate), p


def band_floor_db(x, rate, lo, hi, q=10.0, nfft=8192):
    """The 'always-there' level: q-th percentile (over time) of the frame-by-frame power in the band."""
    f, p = stft_power(x, rate, nfft)
    if p.shape[0] == 0:
        return -200.0
    sel = (f >= lo) & (f <= hi)
    bp = p[:, sel].sum(axis=1)
    bp = bp[bp > 1e-18]
    if bp.size == 0:
        return -200.0
    return 10.0 * np.log10(np.percentile(bp, q) + 1e-30)


def floor_spectrum_db(x, rate, lo=6500, hi=17500, q=10.0, nfft=8192):
    f, p = stft_power(x, rate, nfft)
    sel = (f >= lo) & (f <= hi)
    ok = p.sum(axis=1) > 1e-18
    return f[sel], 10.0 * np.log10(np.percentile(p[ok][:, sel], q, axis=0) + 1e-30)


def onsets(x, rate, count=48):
    """Sudden jumps in the energy above 6 kHz (spectral-flux style, written independently)."""
    nfft, hop = 1024, 256
    f, p = stft_power(x, rate, nfft, hop)
    if p.shape[0] < 8:
        return []
    e = p[:, f > 6000].sum(axis=1) + 1e-20
    le = np.log10(e)
    base = np.convolve(le, np.ones(16) / 16.0, mode="same")
    jump = le[1:] - np.roll(base, 8)[1:]
    cand = [i + 1 for i in range(8, len(jump) - 1) if jump[i] > 0.7 and jump[i] >= jump[i - 1] and jump[i] > jump[i + 1]]
    cand.sort(key=lambda i: -jump[i - 1])
    chosen = []
    for i in cand:
        if all(abs(i - j) > int(0.3 * rate / hop) for j in chosen):
            chosen.append(i)
        if len(chosen) >= count:
            break
    return sorted(i * hop for i in chosen)


def hit_peak_change_db(before, after, rate, starts):
    """For each hit: peak of the 6 kHz+ signal in the first 60 ms, after vs before. Returns the worst (most negative)."""
    k = np.fft.rfftfreq(len(before), 1.0 / rate) > 6000
    hp = lambda s: np.fft.irfft(np.fft.rfft(s) * k, n=len(s))
    hb, ha = hp(before), hp(after)
    worst = 0.0
    n = int(0.06 * rate)
    for s in starts:
        a, b = max(0, s - int(0.002 * rate)), min(len(before), s + n)
        pb, pa = np.max(np.abs(hb[a:b])), np.max(np.abs(ha[a:b]))
        if pb > 1e-6:
            worst = min(worst, 20.0 * np.log10(pa / pb + 1e-12))
    return worst


def out_of_band_db(before, after, rate, centre, half_span, min_db=-110.0):
    """Biggest change of the long-average spectrum outside centre +- half_span (bins above min_db only)."""
    f, pb = stft_power(before, rate, 8192)
    _, pa = stft_power(after, rate, 8192)
    mb, ma = pb.mean(axis=0), pa.mean(axis=0)
    sel = (np.abs(f - centre) > half_span) & (f > 20) & (10 * np.log10(mb + 1e-30) > min_db)
    if not sel.any():
        return 0.0
    return float(np.max(np.abs(10 * np.log10((ma[sel] + 1e-30) / (mb[sel] + 1e-30)))))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("usage: python hiss_analyze.py file.wav   (Stage 0: where does the always-there level sit?)")
        sys.exit(1)
    rate, l, r, info = read_wav(sys.argv[1])
    m, s = mid_of(l, r), 0.5 * (l - r)
    out = {"file": sys.argv[1], "rate": rate, "seconds": len(l) / rate}
    for name, x in (("mid", m), ("side", s)):
        f, db = floor_spectrum_db(x, rate)
        k = max(1, int(round(3000 / (f[1] - f[0]))))
        base = np.array([np.median(db[max(0, i - k):i + k + 1]) for i in range(len(db))])
        ex = db - base
        i = int(np.argmax(np.where((f > 8000) & (f < 16000), ex, -1e9)))
        out[name] = {"peak_hz": float(f[i]), "bump_db": float(ex[i])}
    print(json.dumps(out, indent=2))

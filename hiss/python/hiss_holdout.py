"""Hold-out measurement: original vs processed, plus the free "noisereduce" baseline.

usage: python hiss_holdout.py <orig_dir> <tool_out_dir> <report_dir>

<tool_out_dir> is what `hc_hiss_cli combstems` wrote (<name>_fixed.wav, <name>_removed.wav, <name>_result.json).
For every stem it measures (all free tools: numpy, scipy, pyloudnorm, noisereduce):
  - null test      original minus processed: level, spectrogram, how much of it follows the music (envelope match),
                   how much of it is one repeating 40 ms pattern. A stem is FLAGGED if music could be in it.
  - loudness       ITU-R BS.1770 integrated loudness (pyloudnorm), true peak (4x oversampled), mono sum, timing (cross-correlation lag)
  - comb           lines on the 25 Hz grid in the quiet floor, before / after (independent Python check)
  - baseline       the same stem through noisereduce (stationary), same measurements
Nothing here says "better" - it prints numbers side by side.
"""
import glob
import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import noisereduce as nr
import numpy as np
import pyloudnorm as pyln
import soundfile as sf
from scipy.signal import resample_poly, spectrogram

from hiss_comb import comb


def db(x):
    return 10 * np.log10(max(float(x), 1e-30))


def rms_db(x):
    return db(np.mean(np.asarray(x, dtype=np.float64) ** 2))


def true_peak_db(x):
    y = resample_poly(x, 4, 1, axis=0)
    return 20 * np.log10(max(float(np.abs(y).max()), 1e-30))


def lufs(x, fs):
    try:
        v = pyln.Meter(fs).integrated_loudness(x)
        return float(v) if np.isfinite(v) else None
    except Exception:
        return None


def lag_samples(a, b, fs):
    """how many samples b is late relative to a (0 = aligned), measured on the loudest 8 seconds"""
    n = int(8 * fs)
    m = a.mean(axis=1)
    if len(m) <= n:
        i0 = 0
    else:
        e = np.convolve(m ** 2, np.ones(n), "valid")
        i0 = int(np.argmax(e))
    x = a[i0:i0 + n].mean(axis=1)
    y = b[i0:i0 + n].mean(axis=1)
    N = 1 << (len(x) * 2 - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(y, N) * np.conj(np.fft.rfft(x, N)), N)
    k = int(np.argmax(c))
    return k if k < N // 2 else k - N


def periodic_fraction(rem, fs, ref):
    """share of the removed energy that is ONE pattern repeating every 1/25 s, measured where the original is not digital silence"""
    P = int(round(fs / 25))
    out = []
    for ch in range(rem.shape[1]):
        r = rem[:, ch]
        live = np.abs(ref[:, ch]) > 0
        n = (len(r) // P) * P
        r = np.where(live, r, 0.0)[:n].reshape(-1, P)
        tot = float((r ** 2).sum())
        if tot <= 0:
            continue
        w = (r != 0).sum(axis=0).clip(min=1)
        mean = r.sum(axis=0) / w
        expl = float(((mean[None, :] * (r != 0)) ** 2).sum())
        out.append(expl / tot)
    return float(np.mean(out)) if out else None


def envelope_corr(orig, rem, fs):
    """does the removed part rise and fall with the music? (3-20 kHz level, 0.1 s steps, only where there is sound)"""
    h = int(0.1 * fs)
    n = (len(orig) // h) * h
    if n == 0:
        return None
    o = orig[:n].mean(axis=1).reshape(-1, h)
    r = rem[:n].mean(axis=1).reshape(-1, h)
    eo = 10 * np.log10((o ** 2).mean(axis=1) + 1e-20)
    er = 10 * np.log10((r ** 2).mean(axis=1) + 1e-20)
    ok = eo > -90
    if ok.sum() < 20 or er[ok].std() < 1e-6:
        return None
    return float(np.corrcoef(eo[ok], er[ok])[0, 1])


def spectro_png(x, fs, path, title):
    f, t, S = spectrogram(x, fs, nperseg=4096, noverlap=2048)
    plt.figure(figsize=(9, 3.4))
    plt.pcolormesh(t, f / 1000, 10 * np.log10(S + 1e-22), vmin=-170, vmax=-100, shading="auto")
    plt.ylabel("kHz")
    plt.xlabel("seconds")
    plt.title(title, fontsize=9)
    plt.colorbar(label="dB")
    plt.tight_layout()
    plt.savefig(path, dpi=90)
    plt.close()


def measure(name, orig, test, fs):
    rem = orig - test
    m = {}
    m["same_bits"] = bool(np.array_equal(orig, test))
    m["removed_rms_db"] = round(rms_db(rem), 2) if np.any(rem) else None
    m["removed_peak_db"] = round(20 * np.log10(max(float(np.abs(rem).max()), 1e-30)), 2) if np.any(rem) else None
    m["env_corr"] = envelope_corr(orig, rem, fs) if np.any(rem) else None
    m["periodic_share"] = periodic_fraction(rem, fs, orig) if np.any(rem) else None
    m["lufs_before"], m["lufs_after"] = lufs(orig, fs), lufs(test, fs)
    m["truepeak_before_db"], m["truepeak_after_db"] = round(true_peak_db(orig), 3), round(true_peak_db(test), 3)
    mo, so = orig.sum(axis=1) * 0.5, (orig[:, 0] - orig[:, 1]) * 0.5
    mt, st = test.sum(axis=1) * 0.5, (test[:, 0] - test[:, 1]) * 0.5
    m["mid_rms_before_db"], m["mid_rms_after_db"] = round(rms_db(mo), 3), round(rms_db(mt), 3)
    m["side_rms_before_db"], m["side_rms_after_db"] = round(rms_db(so), 3), round(rms_db(st), 3)
    m["mono_sum_change_rms_db"] = round(rms_db(mo - mt), 2) if np.any(mo - mt) else None
    m["lag_samples"] = lag_samples(orig, test, fs)
    # log-spectral distance between original and processed in the quiet floor is not used: the null test above is the direct measure
    return m


def comb_counts(path):
    c = comb(path)
    return {ch: (None if v is None else {"lines": v["lines"], "on_grid": v["on_grid"]}) for ch, v in c.items()}


def main():
    orig_dir, out_dir, rep_dir = sys.argv[1:4]
    os.makedirs(rep_dir, exist_ok=True)
    tmp = os.path.join(rep_dir, "_nr")
    os.makedirs(tmp, exist_ok=True)
    rows = []
    for f in sorted(glob.glob(os.path.join(orig_dir, "*.wav"))):
        name = os.path.splitext(os.path.basename(f))[0]
        fixed_p = os.path.join(out_dir, name + "_fixed.wav")
        if not os.path.exists(fixed_p):
            continue
        orig, fs = sf.read(f, always_2d=True, dtype="float64")
        fixed, fs2 = sf.read(fixed_p, always_2d=True, dtype="float64")
        assert fs == fs2 and orig.shape == fixed.shape
        with open(os.path.join(out_dir, name + "_result.json")) as fh:
            res = json.load(fh)
        row = {"stem": name, "fs": fs, "seconds": round(len(orig) / fs, 1), "tool": measure(name, orig, fixed, fs),
               "tool_result": {k: res[k] for k in ("found", "touched", "confidence", "linesBefore", "onGridBefore", "onGridAfter", "reductionDb", "failReason")}}
        row["comb_before"] = comb_counts(f)
        if res["touched"]:
            row["comb_after_tool"] = comb_counts(fixed_p)
            rem = orig - fixed
            spectro_png(rem.mean(axis=1), fs, os.path.join(rep_dir, name + "_removed.png"), name + " - what the tool took out (null test)")
            flags = []
            t = row["tool"]
            if t["env_corr"] is not None and t["env_corr"] > 0.30:
                flags.append("removed part follows the music (envelope match %.2f)" % t["env_corr"])
            if t["periodic_share"] is not None and t["periodic_share"] < 0.95:
                flags.append("removed part is not one repeating pattern (%.3f)" % t["periodic_share"])
            if t["removed_rms_db"] is not None and t["removed_rms_db"] > -60:
                flags.append("removed part is louder than -60 dBFS")
            row["tool"]["flags"] = flags
        # baseline: noisereduce, stationary, each channel on its own
        y = np.stack([nr.reduce_noise(y=orig[:, c].astype(np.float32), sr=fs, stationary=True) for c in range(orig.shape[1])], axis=1).astype(np.float64)
        y = y[:len(orig)]
        nr_p = os.path.join(tmp, name + "_noisereduce.wav")
        sf.write(nr_p, y.astype(np.float32), fs, subtype="FLOAT")
        row["noisereduce"] = measure(name, orig, y, fs)
        row["comb_after_noisereduce"] = comb_counts(nr_p)
        rows.append(row)
        print(name, "tool touched=%s" % res["touched"], "| comb lines before", row["comb_before"], "after tool", row.get("comb_after_tool"), "after noisereduce", row["comb_after_noisereduce"], flush=True)
        with open(os.path.join(rep_dir, "holdout.json"), "w") as fh:
            json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    main()

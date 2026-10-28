"""Stage 0 for real files: what is the artifact, really?  (measures only; changes nothing)

usage: python hiss_characterise.py <out_folder> <file.wav|.mp3> [more files]
For each file writes <name>.json and <name>.png (spectrogram + long-term average spectrum of the whole file and of its quiet parts).
MP3 files are decoded with ffmpeg and the encoder delay is NOT removed (compare MP3 vs WAV with hiss_codec_compare.py).
"""
import json
import os
import subprocess
import sys

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from hiss_analyze import read_wav, stft_power


def load(path):
    if path.lower().endswith(".mp3"):
        raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "2", "-ar", "48000", "-"], capture_output=True, check=True).stdout
        x = np.frombuffer(raw, dtype="<f4").astype(np.float64).reshape(-1, 2)
        return 48000, x[:, 0], x[:, 1]
    r, l, rr, _ = read_wav(path)
    return r, l, rr


def smooth_db(db, k):
    pad = np.pad(db, (k, k), mode="edge")
    return np.array([np.median(pad[i:i + 2 * k + 1]) for i in range(len(db))])


def analyse(path, outdir):
    rate, l, r = load(path)
    name = os.path.splitext(os.path.basename(path))[0][:60]
    res = {"file": os.path.basename(path), "rate": rate, "seconds": round(len(l) / rate, 2)}
    chans = {"mid": 0.5 * (l + r), "side": 0.5 * (l - r)}
    fig, axs = plt.subplots(1, 2, figsize=(15, 4.2), gridspec_kw={"width_ratios": [2.2, 1]})
    for cname, x in chans.items():
        f, p = stft_power(x, rate, 8192)
        if p.shape[0] < 8:
            continue
        total = p.sum(axis=1)
        live = total > 1e-16
        if live.sum() < 8:
            res[cname] = {"silent": True}
            continue
        order = np.argsort(total)
        cand = order[live[order]]
        quiet = cand[: max(8, int(0.15 * len(cand)))]
        ltas = 10 * np.log10(p[live].mean(axis=0) + 1e-30)
        qltas = 10 * np.log10(p[quiet].mean(axis=0) + 1e-30)
        q10 = 10 * np.log10(np.percentile(p[live], 10, axis=0) + 1e-30)
        k = int(round(3000 / (f[1] - f[0])))
        base = smooth_db(q10, k)
        ex = q10 - base
        band = (f > 6500) & (f < 18000)
        i = int(np.argmax(np.where(band, ex, -1e9)))
        # tones: narrow lines in q10 that stand out from a 400 Hz median
        k2 = int(round(200 / (f[1] - f[0])))
        fine = q10 - smooth_db(q10, k2)
        tones = [(float(f[j]), float(fine[j])) for j in range(len(f)) if 6000 < f[j] < 23000 and fine[j] > 6 and fine[j] == fine[max(0, j - 3):j + 4].max()]
        # where does the music end? highest frequency where the average spectrum is > 20 dB over its own top-end floor
        top = np.median(ltas[(f > 0.92 * rate / 2)])
        above = np.where(ltas > top + 20)[0]
        cutoff = float(f[above.max()]) if len(above) else None
        # flutter: 1024-FFT envelope of the band around the bump, hits blanked
        pk = float(f[i])
        res[cname] = {"bump_hz": pk, "bump_db_over_3k_median": float(ex[i]), "q10_level_db_at_bump": float(q10[i]),
                      "quiet_level_db_at_bump": float(qltas[i]), "avg_level_db_at_bump": float(ltas[i]),
                      "tones": sorted(tones, key=lambda t: -t[1])[:6], "spectral_cutoff_hz": cutoff, "quiet_frames": int(len(quiet)), "live_frames": int(live.sum())}
        axs[1].plot(f / 1000, smooth_db(ltas, 2), lw=0.8, label=cname + " average")
        axs[1].plot(f / 1000, smooth_db(qltas, 2), lw=0.8, ls="--", label=cname + " quiet 15%")
        if cname == "mid":
            res["band_env"] = flutter(x, rate, pk)
            axs[0].specgram(x + 1e-12, NFFT=2048, Fs=rate, noverlap=1024, cmap="magma", vmin=-160, vmax=-40)
    axs[0].set_ylim(0, rate / 2); axs[0].set_title(name + " (mid)", fontsize=9); axs[0].set_xlabel("s"); axs[0].set_ylabel("Hz")
    axs[1].set_xlim(0, rate / 2000); axs[1].set_ylim(-170, -20); axs[1].grid(alpha=.3); axs[1].legend(fontsize=7); axs[1].set_xlabel("kHz"); axs[1].set_ylabel("dB per bin")
    fig.tight_layout()
    fig.savefig(os.path.join(outdir, name + ".png"), dpi=100)
    plt.close(fig)
    json.dump(res, open(os.path.join(outdir, name + ".json"), "w"), indent=1)
    return res


def flutter(x, rate, centre, width=1000):
    nfft, hop = 1024, 256
    f, p = stft_power(x, rate, nfft, hop)
    sel = (f > centre - width / 2) & (f < centre + width / 2)
    e = p[:, sel].sum(axis=1) + 1e-20
    d = 10 * np.log10(e)
    med = np.median(d)
    keep = d < med + 10
    w = int(rate / hop)
    tr = np.convolve(d, np.ones(w) / w, mode="same")
    det = np.where(keep, d - tr, 0.0)
    if keep.sum() < len(d) / 2:
        return {"skipped": "too many hits"}
    sp = np.abs(np.fft.rfft(det * np.hanning(len(det)), n=1 << int(np.ceil(np.log2(len(det) * 2))))) ** 2
    fr = np.fft.rfftfreq(1 << int(np.ceil(np.log2(len(det) * 2))), hop / rate)
    m = (fr >= 2) & (fr <= 40)
    j = int(np.argmax(np.where(m, sp, 0)))
    rest = sp[(fr > 1) & (fr < 90) & (np.abs(fr - fr[j]) > 1.5)]
    return {"rate_hz": float(fr[j]), "prominence_db": float(10 * np.log10(sp[j] / (np.median(rest) + 1e-30))),
            "depth_db_5_95": float(np.percentile(det[keep], 95) - np.percentile(det[keep], 5)),
            "steadiness_q10_over_median": float(np.percentile(e, 10) / np.median(e))}


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for pth in sys.argv[2:]:
        r = analyse(pth, out)
        print(json.dumps({k: v for k, v in r.items() if k != "side"}, default=str)[:700])
        print("  side:", json.dumps(r.get("side"), default=str)[:300])

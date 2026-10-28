"""Known-answer test for the 25 Hz comb remover.

usage: python hiss_knownanswer.py <cli> <fingerprint_removed.wav> <outdir> <song_dir> [<song_dir> ...]

For every clean human multitrack folder (mono .wav stems):
  1. mix the stems to a stereo test mix (fixed pans, peak -3 dBFS)
  2. build the shimmer fingerprint from a REAL Suno file: the "removed" file the tool wrote for a Suno stem is exactly
     one repeating pattern; its lines (k x 25 Hz, size and phase) are re-made at this song's sample rate
  3. add it to the mix at known levels, run the tool, and measure
        - how much of the added comb is gone
        - how much was taken out that was NOT the added comb (error in the 3-10 kHz and 10-20 kHz "air" bands)
  4. run the tool on the clean mix alone (nothing added): it should leave it bit for bit

Test music is used for measuring only. It is never shipped.
"""
import glob
import json
import os
import subprocess
import sys

import numpy as np
import soundfile as sf

LEVELS_DB = [-100.0, -90.0, -80.0, -70.0]
PERIOD_S = 1.0 / 25.0


def mix_song(folder):
    files = sorted(glob.glob(os.path.join(folder, "*.wav")))
    sig = None
    fs = None
    n_used = 0
    for i, f in enumerate(files):
        x, r = sf.read(f, always_2d=True, dtype="float64")
        x = x.mean(axis=1)
        if fs is None:
            fs = r
            sig = np.zeros((len(x), 2))
        n = min(len(x), len(sig))
        pan = (((i * 0.37) % 1.0) * 1.6 - 0.8)          # -0.8 .. +0.8
        th = (pan + 1.0) * np.pi / 4.0
        sig[:n, 0] += np.cos(th) * x[:n]
        sig[:n, 1] += np.sin(th) * x[:n]
        n_used += 1
    sig *= 10 ** (-3 / 20) / max(1e-9, np.abs(sig).max())
    return fs, sig.astype(np.float32), n_used


def fingerprint(removed_wav):
    x, fs = sf.read(removed_wav, always_2d=True, dtype="float64")
    P = int(round(fs / 25))
    n = len(x)
    # loudest whole period, start on a multiple of P so the pattern index matches the sample clock
    best, bo = -1, 0
    for k in range(5, n // P - 2):
        e = float((x[k * P:(k + 1) * P] ** 2).sum())
        if e > best:
            best, bo = e, k
    seg = x[bo * P:(bo + 1) * P]
    C = np.fft.rfft(seg, axis=0) * (2.0 / P)           # line j sits at j x 25 Hz
    return C                                           # shape (P/2+1, 2)


def synth(C, fs, n, level_db):
    t = np.arange(n) / fs
    out = np.zeros((n, 2))
    jmax = int(min(20000, 0.5 * fs - 500) / 25)
    for j in range(120, jmax + 1):
        for ch in range(2):
            a, ph = abs(C[j, ch]), np.angle(C[j, ch])
            out[:, ch] += a * np.cos(2 * np.pi * j * 25.0 * t + ph)
    rms = np.sqrt((out ** 2).mean())
    out *= 10 ** (level_db / 20) / rms
    return out


def band_rms_db(x, fs, lo, hi):
    X = np.fft.rfft(x, axis=0)
    f = np.fft.rfftfreq(len(x), 1 / fs)
    m = (f >= lo) & (f < hi)
    p = (np.abs(X[m]) ** 2).sum() / (len(x) ** 2) * 2
    return 10 * np.log10(p / x.shape[1] + 1e-30)


def run_cli(cli, wav_in, outdir, force=False):
    subprocess.run([cli, "comb", wav_in, outdir] + (["force"] if force else []), check=True, stdout=subprocess.DEVNULL)
    name = os.path.splitext(os.path.basename(wav_in))[0]
    with open(os.path.join(outdir, name + "_result.json")) as fh:
        res = json.load(fh)
    fixed, _ = sf.read(os.path.join(outdir, name + "_fixed.wav"), always_2d=True, dtype="float64")
    return res, fixed


def main():
    cli, fp_wav, outdir = sys.argv[1:4]
    songs = sys.argv[4:]
    os.makedirs(outdir, exist_ok=True)
    C = fingerprint(fp_wav)
    rows = []
    for folder in songs:
        song = os.path.basename(os.path.normpath(folder))
        fs, mix, nst = mix_song(folder)
        n = len(mix)
        work = os.path.join(outdir, song)
        os.makedirs(work, exist_ok=True)
        # clean alone
        cin = os.path.join(work, "clean.wav")
        sf.write(cin, mix, fs, subtype="FLOAT")
        res, fixed = run_cli(cli, cin, os.path.join(work, "out_clean"))
        same = bool(np.array_equal(fixed.astype(np.float32), mix))
        rows.append({"song": song, "stems": nst, "fs": fs, "seconds": round(n / fs, 1), "level_db": None, "clean_only": True,
                     "found": res["found"], "touched": res["touched"], "bit_exact": same,
                     "max_change_db": float(20 * np.log10(np.abs(fixed - mix).max() + 1e-30))})
        print(song, "clean only:", "found" if res["found"] else "no comb found", "| touched" if res["touched"] else "| untouched", "| bit-exact" if same else "")
        for lv in LEVELS_DB:
          for mode in ("auto", "forced"):
              A = synth(C, fs, n, lv)
              test = (mix.astype(np.float64) + A).astype(np.float32)
              tin = os.path.join(work, "with_%d.wav" % int(-lv))
              sf.write(tin, test, fs, subtype="FLOAT")
              res, fixed = run_cli(cli, tin, os.path.join(work, "out_%s_%d" % (mode, int(-lv))), mode == "forced")
              A32 = test.astype(np.float64) - mix.astype(np.float64)       # the comb as really stored
              resid = fixed - mix.astype(np.float64)
              removed = test.astype(np.float64) - fixed                    # what the tool took out
              E = removed - A32                                            # taken out minus the added comb
              skip = int(1.0 * fs)                                         # ignore the first second (learning edge)
              def rms(x): return float(np.sqrt((x[skip:] ** 2).mean()))
              row = {"song": song, "stems": nst, "fs": fs, "mode": mode, "level_db": lv, "clean_only": False, "found": res["found"], "touched": res["touched"],
                     "linesBefore": res["onGridBefore"], "linesAfter": res["onGridAfter"], "confidence": res["confidence"],
                     "added_rms_db": round(20 * np.log10(rms(A32) + 1e-30), 2),
                     "left_over_rms_db": round(20 * np.log10(rms(resid) + 1e-30), 2),
                     "removed_db": round(20 * np.log10(rms(A32) / (rms(resid) + 1e-30)), 2),
                     "taken_out_rms_db": round(20 * np.log10(rms(removed) + 1e-30), 2),
                     "wrong_taken_rms_db": round(20 * np.log10(rms(E) + 1e-30), 2)}
              for lo, hi, tag in ((20, 3000, "low"), (3000, 10000, "mid"), (10000, 20000, "air")):
                  row["added_%s_db" % tag] = round(band_rms_db(A32[skip:], fs, lo, hi), 2)
                  row["wrong_%s_db" % tag] = round(band_rms_db(E[skip:], fs, lo, hi), 2)
              rows.append(row)
              print("  %s added %.0f dB: touched=%s removed=%.1f dB (lines %s->%s) wrong-taken %.1f dB re-added; air %.1f vs %.1f" % (
                  mode, lv, row["touched"], row["removed_db"], row["linesBefore"], row["linesAfter"], row["wrong_taken_rms_db"] - row["added_rms_db"],
                  row["wrong_air_db"], row["added_air_db"]))
    with open(os.path.join(outdir, "knownanswer.json"), "w") as fh:
        json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    main()

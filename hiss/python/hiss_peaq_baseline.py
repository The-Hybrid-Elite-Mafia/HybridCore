"""PEAQ (free, open source "peaqb-fast", ITU-R BS.1387 Basic) + a fairer noisereduce baseline.

usage: python hiss_peaq_baseline.py <peaqb_binary> <orig_dir> <tool_out_dir> <report_dir>

For each stem the tool actually changed:
  1. fair baseline: noisereduce with a noise sample taken from the quietest 2 s of the stem (not the default "whole file is noise" guess)
  2. PEAQ ODG on two 10 s excerpts (the loudest, and the quietest one that is not silence), reference = original,
     test = tool / noisereduce default / noisereduce fair. All files are rounded to 16 bit first (PEAQ needs 16 bit).
     ODG 0 = cannot be told apart from the reference; -4 = very annoying. Controls: reference vs itself, reference + noise.
  3. whole-stem numbers for the fair baseline: comb lines left, how much it removed, whether the removed part follows the music, loudness

ViSQOL: NOT run (needs a Bazel build and a model file that cannot be fetched here). Reported as UNKNOWN.
"""
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import noisereduce as nr
import numpy as np
import soundfile as sf

from hiss_comb import comb
from hiss_holdout import envelope_corr, lufs, rms_db


def odg(peaq, ref_wav, test_wav):
    out = subprocess.run([peaq, "-r", ref_wav, "-t", test_wav], capture_output=True, text=True, timeout=600).stdout
    m = re.search(r"ODG:\s*(-?[0-9.]+)", out)
    return float(m.group(1)) if m else None


def windows(x, fs, secs, quiet):
    n = int(secs * fs)
    m = x.mean(axis=1)
    c = np.concatenate([[0.0], np.cumsum(m ** 2)])
    e = (c[n:] - c[:-n]) / n
    step = n // 2
    idx = np.arange(0, len(e), step)
    lv = 10 * np.log10(e[idx] + 1e-30)
    live = lv > -85
    if quiet:
        cand = np.where(live)[0]
        j = cand[np.argmin(lv[cand])]
    else:
        j = int(np.argmax(lv))
    return int(idx[j]), n


def to16(x):
    return np.clip(np.round(x * 32768.0), -32768, 32767) / 32768.0


def main():
    peaq, orig_dir, out_dir, rep = sys.argv[1:5]
    os.makedirs(rep, exist_ok=True)
    rows = []
    for f in sorted(glob.glob(os.path.join(orig_dir, "*.wav"))):
        name = os.path.splitext(os.path.basename(f))[0]
        res = json.load(open(os.path.join(out_dir, name + "_result.json")))
        if not res["touched"]:
            continue
        orig, fs = sf.read(f, always_2d=True, dtype="float64")
        fixed, _ = sf.read(os.path.join(out_dir, name + "_fixed.wav"), always_2d=True, dtype="float64")
        nrp = os.path.join(rep, "_nr", name + "_noisereduce.wav")
        dflt = sf.read(nrp, always_2d=True, dtype="float64")[0] if os.path.exists(nrp) else None
        # fair baseline: noise sample = quietest 2 s that is not silence
        i0, n = windows(orig, fs, 2.0, True)
        fair = np.stack([nr.reduce_noise(y=orig[:, c].astype(np.float32), sr=fs, y_noise=orig[i0:i0 + n, c].astype(np.float32), stationary=True)
                         for c in range(orig.shape[1])], axis=1).astype(np.float64)[:len(orig)]
        row = {"stem": name}
        cand = {"tool": fixed, "noisereduce_fair": fair}
        if dflt is not None:
            cand["noisereduce_default"] = dflt[:len(orig)]
        with tempfile.TemporaryDirectory() as td:
            for kind in ("loud", "quiet"):
                s, n10 = windows(orig, fs, 10.0, kind == "quiet")
                ref = os.path.join(td, "ref.wav")
                sf.write(ref, to16(orig[s:s + n10]), fs, subtype="PCM_16")
                row["odg_%s_reference_vs_itself" % kind] = odg(peaq, ref, ref)
                noisy = os.path.join(td, "noisy.wav")
                rng = np.random.default_rng(1)
                sf.write(noisy, to16(orig[s:s + n10] + rng.standard_normal((n10, orig.shape[1])) * 0.003), fs, subtype="PCM_16")
                row["odg_%s_control_added_noise" % kind] = odg(peaq, ref, noisy)
                for k, v in cand.items():
                    t = os.path.join(td, "t.wav")
                    sf.write(t, to16(v[s:s + n10]), fs, subtype="PCM_16")
                    row["odg_%s_%s" % (kind, k)] = odg(peaq, ref, t)
        rem = orig - fair
        row["fair_removed_rms_db"] = round(rms_db(rem), 2)
        row["fair_env_corr"] = envelope_corr(orig, rem, fs)
        row["fair_lufs_before"], row["fair_lufs_after"] = lufs(orig, fs), lufs(fair, fs)
        sf.write(os.path.join(rep, "_fair_" + name + ".wav"), fair.astype(np.float32), fs, subtype="FLOAT")
        c = comb(os.path.join(rep, "_fair_" + name + ".wav"))
        row["fair_comb_on_grid"] = {k: (v and v["on_grid"]) for k, v in c.items()}
        os.remove(os.path.join(rep, "_fair_" + name + ".wav"))
        rows.append(row)
        print(json.dumps(row), flush=True)
        with open(os.path.join(rep, "peaq_baseline.json"), "w") as fh:
            json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    main()

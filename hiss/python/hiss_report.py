"""Builds the pictures and the plain-words report from a folder written by hc_hiss_cli demo.

usage: python hiss_report.py <folder> <test_log.txt> <config_header> <out_folder>
Writes into out_folder:
  spectrogram_<name>.png    before / after / removed, side by side, same colour scale
  REPORT.md                 TESTED / ASSUMED / UNKNOWN in plain words
  ABX_SHEET.md + abx_key.json   a blind listening sheet (the key stays in the json file, not in the sheet)
"""
import glob
import json
import os
import random
import re
import sys

import numpy as np

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

from hiss_analyze import mid_of, read_wav  # noqa: E402


def spectrogram(ax, x, rate, title, vmin, vmax):
    ax.specgram(x, NFFT=2048, Fs=rate, noverlap=1536, cmap="magma", vmin=vmin, vmax=vmax)
    ax.set_ylim(6000, 18000)
    ax.set_title(title, fontsize=9)
    ax.set_xlabel("seconds")


def pictures(folder, out):
    made = []
    for js in sorted(glob.glob(os.path.join(folder, "*_result.json"))):
        res = json.load(open(js))
        name = res["name"]
        bp = os.path.join(folder, name + "_before.wav")
        if not os.path.exists(bp):
            continue
        rate, bl, br, _ = read_wav(bp)
        _, fl, fr, _ = read_wav(os.path.join(folder, name + "_fixed.wav"))
        _, rl, rr, _ = read_wav(os.path.join(folder, name + "_removed.wav"))
        fig, axs = plt.subplots(1, 3, figsize=(15, 4), sharey=True)
        eps = 1e-12
        spectrogram(axs[0], mid_of(bl, br) + eps * 0, rate, "BEFORE", -150, -70)
        spectrogram(axs[1], mid_of(fl, fr), rate, "AFTER (fixed)", -150, -70)
        spectrogram(axs[2], mid_of(rl, rr), rate, "REMOVED (what was cut)", -150, -70)
        fig.suptitle("%s  -  %s  -  confidence %s" % (name, "fixed" if res["touched"] else "left untouched", res["confidence"]), fontsize=11)
        fig.tight_layout()
        path = os.path.join(out, "spectrogram_%s.png" % name)
        fig.savefig(path, dpi=110)
        plt.close(fig)
        made.append(path)
    return made


def config_lines(header):
    assumed, unknown = [], []
    txt = open(header, encoding="utf-8").read().splitlines()
    in_unknown = False
    for ln in txt:
        s = ln.strip()
        if "// ASSUMED" in s:
            m = re.search(r"constexpr\s+\w+\s+(\w+)\s*=\s*([^;]+);", s)
            note = s.split("// ASSUMED", 1)[1].lstrip(": ").strip()
            assumed.append(("%s = %s" % (m.group(1), m.group(2).strip()) if m else s) + ("  - " + note if note else ""))
        if "UNKNOWN" in s and s.startswith("//"):
            in_unknown = True
        if in_unknown and s.startswith("//"):
            unknown.append(s.lstrip("/ ").strip())
        elif in_unknown and not s.startswith("//"):
            in_unknown = False
    return assumed, [u for u in unknown if u]


def main(folder, log, header, out):
    os.makedirs(out, exist_ok=True)
    pics = pictures(folder, out)
    log_text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
    passes = len(re.findall(r"^\s+PASS", log_text, re.M))
    fails = len(re.findall(r"^\s+FAIL", log_text, re.M))
    measured = re.findall(r"^\s+MEASURED (.+)$", log_text, re.M)
    assumed, unknown = config_lines(header)
    rows = []
    for js in sorted(glob.glob(os.path.join(folder, "*_result.json"))):
        r = json.load(open(js))
        rows.append(r)
    L = []
    L.append("# HEM Hiss Tamer + Hiss Auto - test report\n")
    L.append("## TESTED (numbers measured by the tests, on files we made ourselves)\n")
    L.append("- %d checks passed, %d failed.\n" % (passes, fails))
    for m in measured:
        L.append("- " + m)
    L.append("\n### What AUTO FIX did on the demo files\n")
    L.append("| file | touched | confidence | found | floor drop (dB) | worst hit change (dB) | attack shift (ms) | outside change (dB) | watery score |")
    L.append("|---|---|---|---|---|---|---|---|---|")
    for r in rows:
        L.append("| %s | %s | %s | %s | %.1f | %.2f | %.2f | %.3f | %.2f |" % (r["name"], "yes" if r["touched"] else "no", r["confidence"], r["kind"], r["dropDb"], r["peakLossDb"], r["attackShiftMs"], r["outOfBandDb"], r["musicalNoise"]))
    L.append("\n## ASSUMED (working guesses - all live in one file, HissConfig.hpp)\n")
    L.extend("- " + a for a in assumed)
    L.append("\n## UNKNOWN (nobody has measured this yet)\n")
    L.extend("- " + u for u in unknown)
    L.append("- Whether this beats other tools: not measured, so no claim is made.")
    L.append("- The Python check re-measures results independently; it does not re-implement the engine itself.\n")
    open(os.path.join(out, "REPORT.md"), "w", encoding="utf-8").write("\n".join(L) + "\n")

    # blind A/B sheet: for each fixed demo file, a random order of 'original' and 'fixed (level-matched)'
    rnd = random.Random(2026)
    key, sheet = {}, ["# Blind listening sheet\n", "Listen to A then B, loop the same 10 seconds, same volume. Tick which one has less hiss and which one you like more.\n"]
    for r in rows:
        if not r["touched"]:
            continue
        a_is_fixed = rnd.random() < 0.5
        key[r["name"]] = {"A": "fixed" if a_is_fixed else "original", "B": "original" if a_is_fixed else "fixed", "level_match_gain_db": r["levelMatchGainDb"]}
        sheet.append("## %s\n- A: `%s`\n- B: `%s`\n- Less hiss: A / B / same\n- Better overall: A / B / same\n- Any watery or dull sound? ______\n" % (
            r["name"], r["name"] + ("_fixed.wav" if a_is_fixed else "_before.wav"), r["name"] + ("_before.wav" if a_is_fixed else "_fixed.wav")))
    open(os.path.join(out, "ABX_SHEET.md"), "w", encoding="utf-8").write("\n".join(sheet))
    json.dump(key, open(os.path.join(out, "abx_key.json"), "w"), indent=2)
    print("report written;", len(pics), "pictures")


if __name__ == "__main__":
    main(*sys.argv[1:5])

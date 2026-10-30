"""Checks the C++ results against separately written Python measurements.

usage: python hiss_compare.py <folder written by hc_hiss_cli demo>
Prints a table and exits 1 if the C++ and Python numbers disagree by more than the allowed gap,
or if an untouched file is not bit-for-bit the same, or if fixed + removed does not add back to the original.
"""
import glob
import json
import os
import sys

import numpy as np

from hiss_analyze import (band_floor_db, hit_peak_change_db, mid_of, onsets, out_of_band_db, read_wav)

DROP_GAP_DB = 1.0       # ASSUMED: C++ and Python 'floor drop' may differ by this much (different frame grids, same idea)
OOB_LIMIT_DB = 0.05
PEAK_LIMIT_DB = 0.5


def main(folder):
    bad, rows = 0, []
    for js in sorted(glob.glob(os.path.join(folder, "*_result.json"))):
        res = json.load(open(js))
        name = res["name"]
        p = lambda suffix: os.path.join(folder, name + suffix)
        rate, bl, br, _ = read_wav(p("_before.wav")) if os.path.exists(p("_before.wav")) else (None,) * 4
        if rate is None:
            continue
        _, fl, fr, _ = read_wav(p("_fixed.wav"))
        _, rl, rr, _ = read_wav(p("_removed.wav"))
        line = {"name": name, "touched": res["touched"]}
        if not res["touched"]:
            same = np.array_equal(bl, fl) and np.array_equal(br, fr)
            line["untouched_bit_exact"] = bool(same)
            if not same:
                bad += 1
            rows.append(line)
            continue
        bm, fm = mid_of(bl, br), mid_of(fl, fr)
        lo, hi = res["focusHz"] - max(res["chosenWidthHz"], 300) / 2, res["focusHz"] + max(res["chosenWidthHz"], 300) / 2
        py_drop = band_floor_db(bm, rate, lo, hi) - band_floor_db(fm, rate, lo, hi)
        tp = p("_truth.json")
        # Known-answer files: use the true hit positions the generator wrote down (an onset detector can be fooled by flutter).
        starts = json.load(open(tp))["hitStarts"] if os.path.exists(tp) else onsets(bm, rate)
        py_peak = hit_peak_change_db(bm, fm, rate, starts)
        py_oob = out_of_band_db(bm, fm, rate, res["focusHz"], max(res["chosenWidthHz"], 300) / 2 + 600 + 300)
        gap = float(np.max(np.abs(bl - (fl + rl))))   # fixed is stored in the file's own bit depth, so allow rounding
        line.update(cpp_drop=res["dropDb"], py_drop=float(py_drop), py_peak=float(py_peak), py_oob=float(py_oob), add_back_gap=gap, hits=len(starts))
        quant = 1.0 / (2 ** 22)   # 24-bit rounding
        if abs(py_drop - res["dropDb"]) > DROP_GAP_DB:
            bad += 1; line["FAIL"] = "C++ and Python disagree about the drop"
        if py_peak < -PEAK_LIMIT_DB:
            bad += 1; line["FAIL"] = "Python sees a hit lose too much"
        if py_oob > OOB_LIMIT_DB:
            bad += 1; line["FAIL"] = "Python sees a change outside the focus"
        if gap > quant:
            bad += 1; line["FAIL"] = "fixed + removed does not add back to the original"
        rows.append(line)
    for r in rows:
        print(json.dumps(r))
    print("compare: %d problem(s)" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

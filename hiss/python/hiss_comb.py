"""Looks for a line comb (many narrow steady lines) in the quiet-floor spectrum of a file.

usage: python hiss_comb.py file.wav|file.mp3 [spacing_hz=25]
Prints, for mid and side: how many narrow lines stand >10 dB above their own neighbourhood between 3 and 20 kHz,
and how many of them sit on a grid of the given spacing. A random set of lines would land on a 25 Hz grid
(within +-1.5 Hz) only about 12% of the time.
"""
import sys

import numpy as np
from scipy.ndimage import median_filter
from scipy.signal import find_peaks

from hiss_analyze import stft_power
from hiss_characterise import load


def comb(path, spacing=25.0):
    rate, l, r = load(path)
    out = {}
    for ch, x in (("mid", 0.5 * (l + r)), ("side", 0.5 * (l - r))):
        f, p = stft_power(x, rate, 65536, 32768)
        live = p.sum(axis=1) > 1e-16
        if live.sum() < 8:
            out[ch] = None
            continue
        db = 10 * np.log10(np.percentile(p[live], 10, axis=0) + 1e-30)
        ex = db - median_filter(db, size=801, mode="nearest")
        pk, _ = find_peaks(ex, height=10, distance=20)
        pk = [i for i in pk if 3000 < f[i] < 20000]
        tol = 1.5 / spacing
        on = sum(1 for i in pk if abs(f[i] / spacing - round(f[i] / spacing)) < tol)
        out[ch] = {"lines": len(pk), "on_grid": on, "chance_on_grid": round(2 * tol * len(pk), 1),
                   "strongest": sorted([(round(float(f[i])), round(float(ex[i]), 1)) for i in pk], key=lambda t: -t[1])[:5]}
    return out


if __name__ == "__main__":
    print(comb(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 25.0))

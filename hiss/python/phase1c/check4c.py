"""Check 4c: flutter meter v5 (exact-frequency line + self-calibration by standard addition). Rules: SPLIT-ADDENDUM-3-2026-10-29.json."""
import json, sys, os, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == "synthetic"
MIN_S, REF = 19.5, 0.20
def bp(x, s): return signal.sosfiltfilt(signal.butter(4, [5000, min(15000, s/2*0.9)], btype="band", fs=s, output="sos"), x)
def line(x, s, f=25.0):
    """Complex 25 Hz line of the loudness curve, read at exactly 25.000 Hz, divided by the mean envelope. None when it refuses."""
    if len(x) < MIN_S*s: return None
    b = bp(x, s)
    if np.sqrt(np.mean(b*b)) < 10**(-90/20): return None
    N = 1 << int(np.ceil(np.log2(len(b)))); Z = np.fft.fft(b, N); Z[1:N//2] *= 2; Z[N//2+1:] = 0
    e = signal.sosfiltfilt(signal.butter(4, 200, fs=s, output="sos"), np.abs(np.fft.ifft(Z))[:len(b)])
    w = np.hanning(len(e)); t = np.arange(len(e))/s
    return complex(2*np.sum((e-e.mean())*w*np.exp(-2j*np.pi*f*t))/w.sum()/e.mean())
def inject(x, s, m, theta):
    t = np.arange(len(x))/s; return x + m*np.sin(2*np.pi*25*t + theta)*bp(x, s)
def depth_v5(x, s):
    """Returns (depth, g) or None. Self-calibrated: adds 0.20 flutter at phase 0 and 90 degrees."""
    a0 = line(x, s)
    if a0 is None: return None
    a1 = line(inject(x, s, REF, 0.0), s); a2 = line(inject(x, s, REF, np.pi/2), s)
    g1, g2 = abs(a1-a0)/REF, abs(a2-a0)/REF
    if abs(g1-g2) > 0.15*max(g1, g2): return None
    g = 0.5*(g1+g2); return float(abs(a0)/g), float(g)
if SYN_ONLY:   # no audio files: irregular-hit noise, 19.5 s, 48 kHz
    rng = np.random.default_rng(1); s = 48000; n = int(19.6*s); x = rng.standard_normal(n)*0.05
    for t0 in np.sort(rng.random(100)*19.0):
        i = int(t0*s); L = min(n-i, int(0.3*s)); x[i:i+L] += rng.standard_normal(L)*np.exp(-np.arange(L)/(0.08*s))*0.3
    nat = depth_v5(x, s)[0]; ests = [depth_v5(inject(x, s, 0.30, ph), s)[0] for ph in (0.3, 1.7, 3.1)]
    ok = nat < 0.08 and all(0.24 <= v <= 0.36 for v in ests)
    print("CHECK4c synthetic:", "PASS" if ok else "FAIL", round(nat, 3), [round(v, 3) for v in ests]); sys.exit(0 if ok else 1)
c2 = json.load(open(os.path.join(out_dir, "check2.json"))); c4b = json.load(open(os.path.join(out_dir, "check4b.json")))
used = {r["clip"] for r in c4b["clips"]}
hum = [r for r in c2["real"] if r["group"] == "human_reference" and "C_8_20k_db" in r]
hum.sort(key=lambda r: r["C_8_20k_db"]-r["A_5_10k_db"], reverse=True)
rest = [r for r in hum[24:] if r["sha256"][:8] not in used]
rng = np.random.default_rng(5150); rows = []; clips = []
for r in rest:
    x, s = h1c.load_mono(h1c.analysis_path(analysis_dir, r["sha256"]), max_s=40); clips.append((r["sha256"][:8], x[:int(19.5*s)], s))
for name, x, s in clips:
    base = depth_v5(x, s)
    if base is None: rows.append({"clip": name, "refused": True, "ok": False}); continue
    nat, g0 = base
    for m in (0.15, 0.30, 0.45):
        y = inject(x, s, m, rng.random()*2*np.pi); est = depth_v5(y, s)
        if est is None: rows.append({"clip": name, "injected": m, "refused": True, "ok": False}); continue
        rows.append({"clip": name, "injected": m, "estimate": round(est[0], 4), "natural_depth": round(nat, 4), "g_clean": round(g0, 3), "g_injected": round(est[1], 3),
                     "allowed_error": round(0.2*m + nat, 4), "error": round(abs(est[0]-m), 4), "ok": bool(abs(est[0]-m) <= 0.2*m + nat)})
res = {"rule_file": "SPLIT-ADDENDUM-3-2026-10-29.json", "trials": rows, "n_ok": sum(r["ok"] for r in rows), "n": len(rows), "pass": sum(r["ok"] for r in rows) >= 25,
       "natural_depth_by_clip": {c: next((r["natural_depth"] for r in rows if r["clip"] == c and "natural_depth" in r), None) for c, _, _ in clips}}
json.dump(res, open(os.path.join(out_dir, "check4c.json"), "w"), indent=1)
print("CHECK4c", res["n_ok"], "of", res["n"], "PASS" if res["pass"] else "FAIL")

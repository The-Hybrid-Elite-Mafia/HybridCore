"""Check 4b: flutter meter v4, judged on test clips never used for development. Rules: SPLIT-ADDENDUM-2026-10-29.json."""
import json, sys, os, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == "synthetic"
def bp(x, s): return signal.sosfiltfilt(signal.butter(4, [5000, min(15000, s/2*0.9)], btype="band", fs=s, output="sos"), x)
def flutter_depth(x, s, f=25.0):
    """Meter v4. Returns None when it refuses."""
    if len(x) < 19.5*s: return None
    b = bp(x, s)
    if np.sqrt(np.mean(b*b)) < 10**(-90/20): return None
    e = signal.sosfiltfilt(signal.butter(4, 200, fs=s, output="sos"), np.abs(signal.hilbert(b)))
    w = np.hanning(len(e)); A = 2*np.abs(np.fft.rfft((e-e.mean())*w))/w.sum(); fr = np.fft.rfftfreq(len(e), 1/s)
    k = int(np.argmin(np.abs(fr-f))); return float(A[k-1:k+2].max()/e.mean())
def inject(x, s, m, rng):
    t = np.arange(len(x))/s; return x + m*np.sin(2*np.pi*25*t + rng.random()*6.283)*bp(x, s)
if SYN_ONLY:   # no audio files: white-noise 'cymbal' with 5 hits per second, 20 s
    rng = np.random.default_rng(1); s = 48000; n = 20*s; x = rng.standard_normal(n)*0.05
    for t0 in np.sort(rng.random(100)*19.5):   # irregular hits (a steady 5 per second would itself be a 25 Hz line)
        i = int(t0*s); L = min(n-i, int(0.3*s)); x[i:i+L] += rng.standard_normal(L)*np.exp(-np.arange(L)/(0.08*s))*0.3
    d0 = flutter_depth(x, s); d = [flutter_depth(inject(x, s, 0.3, np.random.default_rng(i)), s) for i in range(5)]
    ok = d0 < 0.08 and all(0.24 <= v <= 0.36 for v in d); print("CHECK4b synthetic:", "PASS" if ok else "FAIL", round(d0, 3), [round(v, 3) for v in d]); sys.exit(0 if ok else 1)
c2 = json.load(open(os.path.join(out_dir, "check2.json")))
hum = [r for r in c2["real"] if r["group"] == "human_reference" and "C_8_20k_db" in r]
hum.sort(key=lambda r: r["C_8_20k_db"]-r["A_5_10k_db"], reverse=True)
rng = np.random.default_rng(4242); clips = []; refused = []
for r in hum[24:]:
    x, s = h1c.load_mono(h1c.analysis_path(analysis_dir, r["sha256"]), max_s=40)
    x = x[:int(19.5*s)]
    if flutter_depth(x, s) is None: refused.append(r["sha256"][:8]); continue
    clips.append((r["sha256"][:8], x, s))
    if len(clips) == 10: break
assert len(clips) == 10, len(clips)
rows = []
for name, x, s in clips:
    y = inject(x, s, 0.30, rng); d = flutter_depth(y, s)
    rows.append({"clip": name, "rate": s, "reading": round(d, 4), "baseline_reading": round(flutter_depth(x, s), 4), "ok": bool(0.24 <= d <= 0.36)})
# sensitivity
JIT = (197, 411, 677, 1013, 1500)
thr = float(np.percentile([abs((flutter_depth(x[j:], s) or 0) - flutter_depth(x, s)) if len(x[j:]) >= 19.5*s else abs(flutter_depth(np.concatenate([x[j:], x[:j]]), s) - flutter_depth(x, s)) for _, x, s in clips for j in JIT], 95))
lad = []
for m in (0.05, 0.1, 0.2, 0.3, 0.5):
    det = sum(flutter_depth(inject(x, s, m, rng), s) > thr for _, x, s in clips); lad.append({"depth": m, "present_of_10": int(det)})
floor = next((q["depth"] for q in lad if q["present_of_10"] >= 9 and all(z["present_of_10"] >= 9 for z in lad if z["depth"] >= q["depth"])), None)
res = {"rule_file": "SPLIT-ADDENDUM-2026-10-29.json", "clips": rows, "pass_count": sum(r["ok"] for r in rows), "pass": sum(r["ok"] for r in rows) >= 9,
       "refused_files_skipped": refused, "readings_mean": round(float(np.mean([r["reading"] for r in rows])), 4), "readings_sd": round(float(np.std([r["reading"] for r in rows])), 4),
       "repeatability_threshold": round(thr, 4), "sensitivity": lad, "floor_depth": floor}
json.dump(res, open(os.path.join(out_dir, "check4b.json"), "w"), indent=1)
print("CHECK4b", res["pass_count"], "of 10", "PASS" if res["pass"] else "FAIL", "mean", res["readings_mean"], "sd", res["readings_sd"], "floor", floor)

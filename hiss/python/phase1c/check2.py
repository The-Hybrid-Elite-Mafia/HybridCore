"""Check 2: metric sanity. Part A: synthetic proof that the ratio can rise because the bottom band fell.
Part B: real files, absolute levels, ratio, loudness-normalised levels, bandwidth-matched ratio, n files/frames, medians, bootstrap intervals."""
import json, sys, os, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == "synthetic"
rng = np.random.default_rng(7)
sr = 48000
def cymbal(secs=30):
    n = int(sr*secs); w = rng.standard_normal(n)
    # tilt: -3 dB/oct above 2 kHz via filter, plus hits every 0.25 s with a 120 ms decay
    b,a = signal.butter(1, 2000, fs=sr); y = signal.lfilter(b,a,w)*0.6 + w*0.15
    env = np.zeros(n)
    for t in np.arange(0, secs, 0.25):
        i = int(t*sr); L = min(n-i, int(0.6*sr)); env[i:i+L] = np.maximum(env[i:i+L], np.exp(-np.arange(L)/(0.12*sr))*(0.3+0.7*rng.random()))
    y = y*(0.02+env)
    return 0.2*y/np.max(np.abs(y))
def band_gain(x, lo, hi, gdb):
    f = [0, lo*0.9, lo, hi, hi*1.1, sr/2]; g = [1,1,10**(gdb/20),10**(gdb/20),1,1]
    h = signal.firwin2(4097, f, g, fs=sr); return signal.fftconvolve(x, h, mode="same")
base = cymbal(); m0 = h1c.ratio_metrics(base, sr)
syn = []
for X in (3,6,10):
    y = band_gain(base, 5000, 10000, -X); m = h1c.ratio_metrics(y, sr)
    d_ratio = m["ratio_16_20_over_5_10_db"]-m0["ratio_16_20_over_5_10_db"]; d_abs = m["B_16_20k_db"]-m0["B_16_20k_db"]
    syn.append({"cut_5_10k_db":X,"ratio_rise_db":round(d_ratio,2),"abs_16_20k_change_db":round(d_abs,2),
                "ok": abs(d_ratio-X)<=0.5 and abs(d_abs)<=0.3})
# control: genuinely adding air (+X dB at 16-20k) also raises the ratio, abs level rises too
for X in (3,6,10):
    y = band_gain(base, 16000, 20000, X); m = h1c.ratio_metrics(y, sr)
    syn.append({"add_air_16_20k_db":X,"ratio_rise_db":round(m["ratio_16_20_over_5_10_db"]-m0["ratio_16_20_over_5_10_db"],2),
                "abs_16_20k_change_db":round(m["B_16_20k_db"]-m0["B_16_20k_db"],2)})
if SYN_ONLY:
    ok = all(x["ok"] for x in syn if "ok" in x); print("CHECK2 synthetic:", "PASS" if ok else "FAIL"); sys.exit(0 if ok else 1)
c1 = json.load(open(os.path.join(out_dir,"check1.json")))
real = []; qual = {r["sha256"]:r for r in c1["real"] if "shape" in r}
assets = {a["sha256"]:a for a in json.load(open(os.path.join(out_dir,"..","assets.json")))}
for h, r in qual.items():
    p = h1c.analysis_path(analysis_dir, h)
    x, s = h1c.load_mono(p, max_s=180)
    m = h1c.ratio_metrics(x, s)
    row = {"sha256":h,"group":assets[h]["group"],"rate":s,"cutoff_hz":r["cutoff_hz"],"shape":r["shape"],"qualifies":r["qualifies_for_8_20k"]}
    if m: row.update(m)
    if m and "A_5_10k_db" in m:
        try:
            L = h1c.lufs(x, s); row["lufs"] = round(L,2)
            g = -23.0 - L; row["A_5_10k_db_at_-23LUFS"] = round(m["A_5_10k_db"]+g,2); row["B_16_20k_db_at_-23LUFS"] = round(m["B_16_20k_db"]+g,2)
        except Exception as e: row["lufs_error"]=str(e)[:60]
    real.append(row)
def summarize(rows, key):
    v = [r[key] for r in rows if key in r]
    if len(v) < 3: return {"n_files":len(v)}
    lo,hi = h1c.boot_ci(v)
    return {"n_files":len(v),"n_loud_frames":int(sum(r.get("n_loud",0) for r in rows if key in r)),"median":round(float(np.median(v)),2),"ci95":[round(lo,2),round(hi,2)]}
groups = sorted({r["group"] for r in real})
summ = {}
for g in groups:
    rows = [r for r in real if r["group"]==g]
    summ[g] = {k: summarize(rows,k) for k in ("A_5_10k_db","B_16_20k_db","ratio_16_20_over_5_10_db","ratio_16_19_over_5_10_db","B_16_20k_db_at_-23LUFS")}
    summ[g]["qualifying_files"] = sum(1 for r in rows if r["qualifies"])
bm = {}
for g in groups:
    rows = [r for r in real if r["group"]==g and r["qualifies"]]
    bm[g] = {k: summarize(rows,k) for k in ("ratio_16_19_over_5_10_db","B_16_20k_db_at_-23LUFS")}
res = {"synthetic":syn,"synthetic_cut_ok": all(s["ok"] for s in syn if "ok" in s),"real":real,"group_summary_all":summ,"group_summary_bandwidth_matched_ge19k":bm,
 "loud_part":"enter >= P70 / leave < P60 of per-file 5-16 kHz frame power over non-silent (> -80 dB) frames; frames 2048 hop 512",
 "note":"Groups mix instrument roles (drums, voice, bass...). A role-matched comparison needs a blind role label and is Phase 2."}
json.dump(res, open(os.path.join(out_dir,"check2.json"),"w"), indent=1)
for s in syn: print(s)
print("cut ok:", res["synthetic_cut_ok"], "| real rows:", len(real))

"""Check 7: three-way check of the C++ meters (hc_hiss_cli meters), the Python meters, and libebur128 (separately written, MIT) on real files.
Tolerances (SPLIT-ADDENDUM-2026-10-29.json): band levels and ratios 0.05 dB, flutter 0.005, loudness 0.1 LU, true peak 0.2 dB, cutoff 100 Hz, comb 0.05 dB."""
import json, sys, os, subprocess, tempfile, numpy as np
from scipy import signal
import scipy.io.wavfile as wf
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir, cli, ref = sys.argv[1:5]
c6 = json.load(open(os.path.join(out_dir, "check6.json")))
def py_flutter(x, s):
    sys.argv = [sys.argv[0], out_dir, analysis_dir, "synthetic"]
    import importlib.util
    src = open(os.path.join(os.path.dirname(__file__), "check4c.py")).read(); head = src[:src.index("if SYN_ONLY:")]
    ns = {}; exec(compile("import sys, os, json\nimport numpy as np\nfrom scipy import signal\nsys.path.insert(0, %r)\nimport h1c\nout_dir, analysis_dir = %r, %r\nSYN_ONLY = False\n" % (os.path.dirname(__file__), out_dir, analysis_dir) + head.split("SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == \"synthetic\"")[1], "c4c", "exec"), ns)
    r = ns["depth_v5"](x, s); return None if r is None else r[0]
def py_comb(x, s):
    P = int(round(s/25.0))
    if abs(P*25-s) > 1e-6: return None
    Q = 2*P; M = len(x)//Q; y = x[:M*Q].reshape(M, Q).mean(axis=0); Y = np.fft.rfft(y)/Q; pw = 2*np.abs(Y)**2
    ks = np.arange(int(np.ceil(3000/25)), int(np.floor(20000/25))+1)
    on = pw[2*ks]; off = 0.5*(pw[2*ks-1]+pw[2*ks+1]); return float(np.median(10*np.log10(np.maximum(on-off, 1e-30))))
rows = []
for r in c6["files"]:
    p = h1c.analysis_path(analysis_dir, r["sha256"]); x, s = h1c.load_mono(p, max_s=60)
    with tempfile.TemporaryDirectory() as d:
        w = os.path.join(d, "m.wav"); wf.write(w, s, x.astype(np.float32))
        o = subprocess.run([cli, "meters", w], capture_output=True, text=True).stdout
        cpp = {l.split()[0]: l.split()[1] for l in o.strip().split("\n")}
        e = subprocess.run([ref, str(s)], input=x.astype("<f4").tobytes(), capture_output=True).stdout.decode().split()
    ratio = h1c.ratio_metrics(x, s); cut = h1c.cutoff_audit(x, s); fl = py_flutter(x, s); cb = py_comb(x, s)
    cmp = {}
    def add(name, c, p_, tol):
        if p_ is None: cmp[name] = {"cpp": c, "py": None, "skipped": "python side refused / not valid"}; return
        cmp[name] = {"cpp": round(float(c), 4), "py": round(float(p_), 4), "diff": round(float(c)-float(p_), 4), "tol": tol, "ok": bool(abs(float(c)-float(p_)) <= tol)}
    add("lufs_vs_python", cpp["lufs"], h1c.lufs(x, s), 0.1)
    add("lufs_vs_libebur128", cpp["lufs"], float(e[0]), 0.1)
    add("true_peak_vs_libebur128", cpp["true_peak_dbfs"], float(e[1]), 0.2)
    add("rms_dbfs", cpp["rms_dbfs"], 10*np.log10(np.mean(x**2)+1e-30), 0.05)
    if ratio and "A_5_10k_db" in ratio and cpp["ratio_valid"] == "1":
        for k in ("A_5_10k_db", "B_16_20k_db", "C_8_20k_db", "ratio_16_20_over_5_10_db", "ratio_16_19_over_5_10_db"): add(k, cpp[k], ratio[k], 0.05)
    else: cmp["ratio"] = {"cpp_valid": cpp["ratio_valid"], "py_valid": bool(ratio and "A_5_10k_db" in ratio), "ok": (cpp["ratio_valid"] == "1") == bool(ratio and "A_5_10k_db" in ratio)}
    cmp["cutoff_hz"] = {"cpp": float(cpp["cutoff_hz"]), "py": cut["cutoff_hz"], "shape_cpp": cpp["cutoff_shape"], "shape_py": cut["shape"], "diff": round(float(cpp["cutoff_hz"])-cut["cutoff_hz"], 1),
                         "ok": cpp["cutoff_shape"] == cut["shape"] and abs(float(cpp["cutoff_hz"])-cut["cutoff_hz"]) <= 100.0}
    if cpp["comb_valid"] == "1" and cb is not None: add("comb_median_line_db", cpp["comb_median_line_db"], cb, 0.05)
    if cpp["flutter_valid"] == "1" and fl is not None: add("flutter_depth", cpp["flutter_depth"], fl, 0.005)
    elif cpp["flutter_valid"] == "1" or fl is not None: cmp["flutter"] = {"cpp_valid": cpp["flutter_valid"], "py_valid": fl is not None, "ok": False}
    ok = all(v.get("ok", True) for v in cmp.values())
    rows.append({"sha256": r["sha256"], "group": r["group"], "rate": s, "seconds": round(len(x)/s, 1), "ok": ok, "compare": cmp})
bad = [(r["group"], k, v) for r in rows for k, v in r["compare"].items() if not v.get("ok", True)]
res = {"libebur128": "jiixyj/libebur128 commit 67b33ab (2021-02-14), MIT, built with gcc here", "files": rows, "n_ok": sum(r["ok"] for r in rows), "n": len(rows), "failures": bad[:40]}
json.dump(res, open(os.path.join(out_dir, "check7.json"), "w"), indent=1)
print("CHECK7", res["n_ok"], "of", res["n"], "files fully agree")
for b in bad[:15]: print(b)

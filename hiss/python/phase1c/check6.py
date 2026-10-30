"""Check 6: independent tool check. Headline numbers by my Python vs ffmpeg (separate code), on the same mono-mixed signal.
sox: not installable here (reported). Sonic Visualiser by hand: cannot be run here (reported UNKNOWN)."""
import json, sys, os, re, subprocess, tempfile, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
c2 = json.load(open(os.path.join(out_dir,"check2.json")))
rows = [r for r in c2["real"] if "A_5_10k_db" in r and r["qualifies"]]
pick = []
for g in ("human_reference","no_brakes","days240","twelve_stem_june","ending_swoosh","reading_same_letter","song_4m31","theres_a_difference"):
    gr = [r for r in rows if r["group"]==g][:2]
    pick += gr
pick = pick[:14]
def ff(cmd):
    return subprocess.run(["ffmpeg","-hide_banner","-nostats"]+cmd, capture_output=True, text=True).stderr
def bandrms_py(x, s, lo, hi):
    X = np.fft.rfft(x); f = np.fft.rfftfreq(len(x),1/s); m = (f>=lo)&(f<hi)
    p = (np.abs(X[m])**2).sum()*2/len(x)**2     # Parseval: mean-square of the band-limited signal
    return 10*np.log10(p+1e-30)
def true_peak_py(x, s):
    return 20*np.log10(np.max(np.abs(signal.resample_poly(x, 4, 1)))+1e-30)
res = []
for r in pick:
    p = h1c.analysis_path(analysis_dir, r["sha256"])
    x, s = h1c.load_mono(p, max_s=60)
    with tempfile.TemporaryDirectory() as d:
        w = os.path.join(d,"m.wav")
        import scipy.io.wavfile as wf
        wf.write(w, s, x.astype(np.float32))                      # float32, no rounding, same samples both tools read
        o1 = ff(["-i",w,"-af","ebur128=peak=true","-f","null","-"])
        I = float(re.findall(r"I:\s+(-?[\d.]+) LUFS", o1)[-1]); TP = float(re.findall(r"Peak:\s+(-?[\d.]+) dBFS", o1)[-1])
        o2 = ff(["-i",w,"-af","astats=metadata=0:measure_perchannel=none","-f","null","-"])
        rms = float(re.findall(r"RMS level dB:\s+(-?[\d.]+)", o2)[-1])
        bands = {}
        for lo, hi in ((8000,12000),(12000,16000),(16000,19000)):
            ge = f"entry(0,-120);entry({lo-1},-120);entry({lo},0);entry({hi-1},0);entry({hi},-120);entry({s/2},-120)"
            o3 = ff(["-i",w,"-af",f"firequalizer=gain_entry='{ge}':delay=0.2:accuracy=1,astats=metadata=0:measure_perchannel=none","-f","null","-"])
            bands[f"{lo}-{hi}"] = float(re.findall(r"RMS level dB:\s+(-?[\d.]+)", o3)[-1])
    py = {"I_lufs": h1c.lufs(x, s), "tp_dbfs": true_peak_py(x, s), "rms_dbfs": 10*np.log10(np.mean(x**2)+1e-30)}
    for lo, hi in ((8000,12000),(12000,16000),(16000,19000)): py[f"{lo}-{hi}"] = bandrms_py(x, s, lo, hi)
    ffm = {"I_lufs":I,"tp_dbfs":TP,"rms_dbfs":rms, **bands}
    diffs = {k: round(py[k]-ffm[k],3) for k in py}
    ok = abs(diffs["I_lufs"])<=0.1 and abs(diffs["tp_dbfs"])<=0.2 and abs(diffs["rms_dbfs"])<=0.2 and all(abs(diffs[k])<=0.2 for k in ("8000-12000","12000-16000","16000-19000"))
    res.append({"sha256":r["sha256"],"group":r["group"],"rate":s,"python":{k:round(v,2) for k,v in py.items()},"ffmpeg":{k:round(v,2) for k,v in ffm.items()},"diff_py_minus_ffmpeg":diffs,"ok":bool(ok)})
out = {"tools":{"python":"numpy/scipy/pyloudnorm","ffmpeg":subprocess.run(["ffmpeg","-version"],capture_output=True,text=True).stdout.split("\n")[0],
  "sox":"NOT AVAILABLE (apt install failed); ffmpeg is used as the second tool and the sox check is skipped","sonic_visualiser_by_hand":"UNKNOWN - a desktop program with a screen; cannot be run in this workspace"},
  "tolerance":{"lufs":0.1,"db":0.2},"files":res,"n_ok":sum(r["ok"] for r in res),"n":len(res),
  "worst_abs_diff_db":max(max(abs(v) for v in r["diff_py_minus_ffmpeg"].values()) for r in res)}
json.dump(out, open(os.path.join(out_dir,"check6.json"),"w"), indent=1)
print("CHECK6", out["n_ok"], "of", out["n"], "worst", out["worst_abs_diff_db"])
for r in res:
    if not r["ok"]: print(r["group"], r["diff_py_minus_ffmpeg"])

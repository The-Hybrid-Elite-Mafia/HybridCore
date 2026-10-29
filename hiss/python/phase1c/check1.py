"""Check 1: bandwidth audit. Part A = synthetic known-cutoff tests. Part B = every real asset (by hash)."""
import json, sys, os, subprocess, tempfile, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == "synthetic"
rng = np.random.default_rng(1)
def tone_noise(sr, secs=20):
    n = int(sr*secs); w = rng.standard_normal(n)
    b,a = signal.butter(1, 0.2); return signal.lfilter(b,a,w)*0+w   # white noise: flat everywhere
syn = []
for sr in (44100, 48000):
    for fc in (16000, 18000, 20000):
        if fc >= sr/2-300: continue
        x = tone_noise(sr); h = signal.firwin(2049, fc, fs=sr, window=("kaiser",14.0))
        y = signal.fftconvolve(x, h, mode="same")
        r = h1c.cutoff_audit(y, sr); syn.append({"case":f"FIR wall {fc}","sr":sr,"expected_hz":fc,"expected_shape":"WALL",**r,
            "ok": r["shape"]=="WALL" and abs(r["cutoff_hz"]-fc)<=300})
    x = tone_noise(sr); r = h1c.cutoff_audit(x, sr)
    syn.append({"case":"full band","sr":sr,"expected_hz":int(sr/2),"expected_shape":"NONE",**r,"ok": r["shape"]=="NONE"})
    # gentle: 2nd-order lowpass at 14 kHz
    fr = [0,11000,17000,sr/2]; gn = [1.0,1.0,10**(-40/20),10**(-40/20)]   # slope from 11 to 17 kHz, 6 kHz wide, -40 dB
    hg = signal.firwin2(2049, fr, gn, fs=sr); y = signal.fftconvolve(tone_noise(sr), hg, mode="same"); r = h1c.cutoff_audit(y, sr)
    syn.append({"case":"gentle 6 kHz slope","sr":sr,"expected_shape":"GENTLE",**r,"ok": r["shape"]=="GENTLE"})
# real MP3 encoder (LAME via ffmpeg). The encoder does not write its lowpass into the header here, so the true cutoff is UNKNOWN.
# These cases only test: the tool calls a codec lowpass a WALL or GENTLE end (not NONE) and puts it below 20 kHz.
for sr in (44100, 48000):
    for ch,br in ((1,64),(2,128),(2,192)):
        with tempfile.TemporaryDirectory() as d:
            x = tone_noise(sr, 20)*0.1
            p=os.path.join(d,"n.f32"); open(p,"wb").write(x.astype(np.float32).tobytes())
            m=os.path.join(d,"n.mp3")
            subprocess.run(["ffmpeg","-v","error","-f","f32le","-ar",str(sr),"-ac","1","-i",p,"-ac",str(ch),"-b:a",f"{br}k",m],check=True)
            y,srr = h1c.load_mono(m); r = h1c.cutoff_audit(y, srr)
            syn.append({"case":f"MP3 {br}k {ch}ch (true cutoff UNKNOWN)","sr":srr,"expected_shape":"WALL or GENTLE, below 20 kHz",**r,
              "ok": r["shape"] in ("WALL","GENTLE") and r["cutoff_hz"]<20000})
if SYN_ONLY:
    ok = all(x["ok"] for x in syn); print("CHECK1 synthetic:", "PASS" if ok else "FAIL", len(syn), "cases"); sys.exit(0 if ok else 1)
assets = json.load(open(os.path.join(out_dir,"..","assets.json")))
seen = {}; real = []
for a in assets:
    h = a["sha256"]
    if h in seen: continue
    seen[h]=1
    p = os.path.join(analysis_dir, h[:16] + ".wav")
    if not os.path.exists(p):
        alt=[q for q in os.listdir(analysis_dir) if q.startswith(h[:16])]
        if not alt: real.append({"sha256":h,"error":"analysis copy missing"}); continue
        p=os.path.join(analysis_dir,alt[0])
    try:
        x,sr = h1c.load_mono(p, max_s=120)
        r = h1c.cutoff_audit(x, sr)
        r.update({"sha256":h,"group":a["group"],"codec":a.get("codec"),"seconds_read":round(len(x)/sr,1)})
        r["qualifies_for_8_20k"] = r["cutoff_hz"] >= 19000
        real.append(r)
    except Exception as e:
        real.append({"sha256":h,"error":str(e)[:120]})
res = {"synthetic":syn,"synthetic_all_ok":all(s["ok"] for s in syn),"real":real,
       "real_summary":{}}
from collections import Counter
q=[r for r in real if "qualifies_for_8_20k" in r]
res["real_summary"]={"n_unique_files":len(real),"n_measured":len(q),"n_qualify_ge_19k":sum(r["qualifies_for_8_20k"] for r in q),
   "by_shape":dict(Counter(r["shape"] for r in q)),
   "by_group_qualify":{g:[sum(r["qualifies_for_8_20k"] for r in q if r["group"]==g), sum(1 for r in q if r["group"]==g)] for g in sorted({r["group"] for r in q})}}
json.dump(res, open(os.path.join(out_dir,"check1.json"),"w"), indent=1)
print(json.dumps(res["real_summary"],indent=1)); print("synthetic ok:",res["synthetic_all_ok"])
for s in syn: print(s["case"],s["sr"],s["shape"],s["cutoff_hz"],s["ok"])

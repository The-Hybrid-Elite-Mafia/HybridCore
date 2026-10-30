"""Shared helpers for Hiss Tamer 1C. Plain numpy/scipy. No file names are used; files are keyed by SHA-256."""
import subprocess, numpy as np, json, os
from scipy import signal

def load_mono(path, max_s=None):
    """Decode with ffmpeg to float32 mono at the file's own rate. Returns (x, rate)."""
    pr = subprocess.run(["ffprobe","-v","error","-select_streams","a:0","-show_entries","stream=sample_rate","-of","csv=p=0",path],capture_output=True,text=True)
    sr = int(pr.stdout.strip().split("\n")[0])
    cmd = ["ffmpeg","-v","error","-i",path,"-ac","1","-f","f32le","-"]
    if max_s: cmd[3:3] = ["-t", str(max_s)]
    raw = subprocess.run(cmd,capture_output=True).stdout
    return np.frombuffer(raw,dtype=np.float32).astype(np.float64), sr

def avg_psd_db(x, sr, nfft=8192):
    """Average power spectrum in dB (power per bin), Hann, 50 % overlap. Returns (freqs, dB)."""
    f, p = signal.welch(x, fs=sr, window="hann", nperseg=nfft, noverlap=nfft//2, scaling="spectrum")
    return f, 10*np.log10(p + 1e-24)

def smooth_db(f, db, width_hz=200):
    df = f[1]-f[0]; n = max(1,int(round(width_hz/df)))
    p = 10**(db/10); k = np.ones(n)/n
    pad = n//2 + 1
    pp = np.concatenate([p[pad:0:-1], p, p[-2:-pad-2:-1]])   # mirror the edges so the ends do not fake a drop
    return 10*np.log10(np.convolve(pp,k,mode="same")[pad:pad+len(p)]+1e-30)

def band_db(f, db, lo, hi):
    m = (f>=lo)&(f<hi); return float(10*np.log10(np.mean(10**(db[m]/10))+1e-30))

def cutoff_audit(x, sr):
    """Where does the sound end, and what shape is the end.
    ref = median smoothed level 5-10 kHz. WALL: >=20 dB fall inside 500 Hz. GENTLE: fall to ref-30 dB without a wall.
    NONE: stays above ref-30 dB to 0.97*Nyquist. Cutoff = frequency where the fall finishes (WALL) or where level first reaches ref-30 (GENTLE)."""
    f, db = avg_psd_db(x, sr); s = smooth_db(f, db, 100)
    nyq = sr/2.0
    m = (f>=5000)&(f<10000); ref = float(np.median(s[m]))
    df = f[1]-f[0]; span = max(1,int(round(500/df)))
    top = f <= nyq*0.995
    idx = np.where((f>=8000)&top)[0]
    best = (0.0,None)
    for i in idx:
        j = i+span
        if j >= len(f): break
        drop = s[i]-s[j]
        if drop > best[0]: best = (drop,i)
    res = {"sample_rate":sr,"ref_5_10k_db":round(ref,1)}
    if best[0] >= 20.0:
        i = best[1]; res["shape"]="WALL"; res["drop_db_in_500hz"]=round(float(best[0]),1)
        # cutoff = where the level first falls 6 dB below its pre-wall value (start of the fall, not the end)
        lvl = s[max(0,i-int(1000/df))]
        k = i
        while k < len(f)-1 and s[k] > lvl-6: k += 1
        res["cutoff_hz"]=int(f[k])
    else:
        below = np.where((f>=8000)&(s < ref-30))[0]
        if len(below):
            res["shape"]="GENTLE"; res["cutoff_hz"]=int(f[below[0]])
        else:
            res["shape"]="NONE"; res["cutoff_hz"]=int(nyq)
    res["level_at_nyquist_minus_1k_db"]=round(float(np.interp(nyq-1000,f,s)-ref),1)
    return res

# ---------- frame features (checks 2, 4, 5) ----------
def stft_pow(x, sr, n=2048, hop=512):
    """Power per bin per frame, scaled so a 1.0-amplitude sine adds 0.5 total. Returns (freqs, P[frames,bins])."""
    w = signal.get_window("hann", n); k = 2.0/(w.sum()**2)
    nfr = 1 + (len(x)-n)//hop
    if nfr < 1: return None, None
    idx = np.arange(n)[None,:] + hop*np.arange(nfr)[:,None]
    X = np.fft.rfft(x[idx]*w[None,:], axis=1)
    return np.fft.rfftfreq(n, 1.0/sr), (np.abs(X)**2)*k*0.5   # *0.5 so that amplitude-1 sine -> 0.5 (mean-square)

def band_pow(f, P, lo, hi):
    m = (f>=lo)&(f<hi); return P[:,m].sum(axis=1)

def db(v): return 10*np.log10(np.maximum(v,1e-30))

def loud_mask(e, silence_db=-80.0, hi_pct=70, lo_pct=60):
    """Hysteresis. e = per-frame 5-16 kHz power. Non-silent = above silence_db. Enter at P70, leave below P60 of non-silent frames."""
    ns = db(e) > silence_db
    if ns.sum() < 20: return np.zeros(len(e), bool)
    hi = np.percentile(e[ns], hi_pct); lo = np.percentile(e[ns], lo_pct)
    m = np.zeros(len(e), bool); on = False
    for i, v in enumerate(e):
        if not on and v >= hi and ns[i]: on = True
        elif on and (v < lo or not ns[i]): on = False
        m[i] = on
    return m

def ratio_metrics(x, sr, n=2048, hop=512, top=20000.0, mask=None):
    """Absolute band levels (dB re 1.0-amplitude-sine mean-square) over loud frames, and the ratios."""
    f, P = stft_pow(x, sr, n, hop)
    if P is None: return None
    top = min(top, sr/2.0)
    A = band_pow(f,P,5000,10000); B = band_pow(f,P,16000,top); C = band_pow(f,P,8000,top)
    B19 = band_pow(f,P,16000,min(19000.0,sr/2.0))
    e = band_pow(f,P,5000,16000); m = loud_mask(e) if mask is None else mask[:len(e)]
    out = {"n_frames": int(len(e)), "n_loud": int(m.sum())}
    if m.sum() < 20: return out
    a, b, c, b19 = A[m].mean(), B[m].mean(), C[m].mean(), B19[m].mean()
    out.update({"A_5_10k_db": float(db(a)), "B_16_20k_db": float(db(b)), "C_8_20k_db": float(db(c)),
                "ratio_16_20_over_5_10_db": float(db(b)-db(a)), "ratio_16_19_over_5_10_db": float(db(b19)-db(a))})
    return out

def block_bootstrap_median_ratio(ratio_frames_db, block=86, n=2000, seed=0):
    """Not used for files; kept for frame-level intervals."""
    rng = np.random.default_rng(seed); k = int(np.ceil(len(ratio_frames_db)/block))
    bl = [ratio_frames_db[i*block:(i+1)*block] for i in range(k)]
    meds = [np.median(np.concatenate([bl[j] for j in rng.integers(0,k,k)])) for _ in range(n)]
    return float(np.percentile(meds,2.5)), float(np.percentile(meds,97.5))

def boot_ci(v, stat=np.median, n=2000, seed=0):
    v = np.asarray(v, float); rng = np.random.default_rng(seed)
    s = [stat(v[rng.integers(0,len(v),len(v))]) for _ in range(n)]
    return float(np.percentile(s,2.5)), float(np.percentile(s,97.5))

def lufs(x, sr):
    import pyloudnorm as pln
    return float(pln.Meter(sr).integrated_loudness(x))

def analysis_path(d, h):
    """Name-stripped analysis copy for a hash (16-char prefix, any extension)."""
    for q in os.listdir(d):
        if q.startswith(h[:16]): return os.path.join(d, q)
    raise FileNotFoundError(h[:16])

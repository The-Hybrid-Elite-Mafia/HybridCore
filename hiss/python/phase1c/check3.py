"""Check 3: line-level sanity for the 25 Hz comb.
A) Injected known lines, measured two ways: averaged spectrum (Welch) and narrow-band demodulation (heterodyne + boxcar).
B) Real 48 kHz files: level of every line (3-20 kHz), with its measurement bandwidth; compare with a local masking threshold and with the delivery noise floor.
ASSUMED (all labelled): playback calibration SPL = dBFS_rms + 103; Terhardt hearing threshold; masker-to-tone offset 6 dB; Bark bands."""
import json, sys, os, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
SYN_ONLY = len(sys.argv) > 3 and sys.argv[3] == "synthetic"
SPL_OFFSET = 103.0      # ASSUMED
MASK_OFFSET_DB = 6.0    # ASSUMED
rng = np.random.default_rng(3)
sr = 48000; P = sr//25   # 1920 samples = one period of the 25 Hz comb
BARK_EDGES = [3150,3700,4400,5300,6400,7700,9500,12000,15500,20000]   # ASSUMED Bark edges, upper part only

def ath_spl(f_hz):
    f = np.asarray(f_hz)/1000.0
    return 3.64*f**-0.8 - 6.5*np.exp(-0.6*(f-3.3)**2) + 1e-3*f**4        # Terhardt, dB SPL (ASSUMED model)

def welch_lines(x, ks, nmul=8):
    """Averaged spectrum. Line tone power (mean-square) read at its bin. Bandwidth = Hann ENBW = 1.5 bins."""
    nfft = P*nmul
    f, p = signal.welch(x, fs=sr, window="hann", nperseg=nfft, noverlap=nfft//2, scaling="spectrum")   # mean-square per bin for a tone
    bin_hz = sr/nfft; idx = [int(round(k*25/bin_hz)) for k in ks]
    return np.array([p[i] for i in idx]), 1.5*bin_hz
def demod_lines(x, ks, direct=False):
    """Narrow-band demodulation. Heterodyne to 0 Hz, boxcar over whole periods. Bandwidth (ENBW of a boxcar) = 1/T."""
    M = len(x)//P; xx = x[:M*P]; t = np.arange(len(xx))/sr
    if direct:
        amp = np.array([2*abs(np.mean(xx*np.exp(-2j*np.pi*k*25*t))) for k in ks])
    else:
        y = xx.reshape(M,P).mean(axis=0); Y = np.fft.rfft(y)/P; amp = 2*np.abs(Y[ks])
    return amp**2/2, 1.0/(M*P/sr)
def demod_on_off(x, ks):
    """Fold over TWO periods (12.5 Hz resolution). On-line power at k*25 Hz; off-line power = mean of the two neighbours at k*25 +/- 12.5 Hz.
    Music that happens to sit on the grid shows up in both, so (on - off) isolates what repeats every 1/25 s. Bandwidth = 1/T."""
    Q = 2*P; M = len(x)//Q; y = x[:M*Q].reshape(M,Q).mean(axis=0); Y = np.fft.rfft(y)/Q
    pw = 2*np.abs(Y)**2
    on = pw[2*ks]; off = 0.5*(pw[2*ks-1]+pw[2*ks+1])
    return on, off, 1.0/(M*Q/sr)
def dbf(v): return 10*np.log10(np.maximum(v,1e-30))

# ---- A) injected lines ----
ks = np.arange(120, 801, 40)            # every 40th line from 3 kHz to 20 kHz (kept small so the direct method is fast)
syn = []
for bg_db, bg_name in ((-100.0,"quiet background (white, -100 dBFS rms)"), (-60.0,"loud background (white, -60 dBFS rms)")):
    for L in (-110.0, -100.0, -90.0):
        n = sr*20; t = np.arange(n)/sr
        x = rng.standard_normal(n)*10**(bg_db/20)
        amp = np.sqrt(2)*10**(L/20)            # sine amplitude giving per-line rms L
        for k in ks: x += amp*np.sin(2*np.pi*k*25*t + rng.random()*6.283)
        pw, bw_w = welch_lines(x, ks); pd, bw_d = demod_lines(x, ks, direct=True); pf, _ = demod_lines(x, ks, direct=False)
        # in-bandwidth noise for each method (white): density * bandwidth
        dens = 10**(bg_db/10)/(sr/2)
        n_w, n_d = dens*bw_w, dens*bw_d
        syn.append({"background":bg_name,"line_dbfs_true":L,
          "welch":{"bandwidth_hz":round(bw_w,2),"median_est_db":round(float(np.median(dbf(pw))),2),"error_db":round(float(np.median(dbf(pw))-L),2),"noise_in_bw_db":round(float(dbf(n_w)),1),"line_over_noise_db":round(float(L-dbf(n_w)),1)},
          "demod":{"bandwidth_hz":round(bw_d,3),"median_est_db":round(float(np.median(dbf(pd))),2),"error_db":round(float(np.median(dbf(pd))-L),2),"noise_in_bw_db":round(float(dbf(n_d)),1),"line_over_noise_db":round(float(L-dbf(n_d)),1)},
          "demod_fold_vs_direct_db":round(float(np.max(np.abs(dbf(pf)-dbf(pd)))),4),
          "methods_agree_within_1db": bool(abs(np.median(dbf(pw))-np.median(dbf(pd)))<=1.0)})
# pass rule (SPLIT): methods agree within 1 dB where the line is at least 6 dB above in-bandwidth noise in BOTH methods
rows_ok = [s for s in syn if s["welch"]["line_over_noise_db"]>=6 and s["demod"]["line_over_noise_db"]>=6]
passA = bool(rows_ok) and all(s["methods_agree_within_1db"] and abs(s["welch"]["error_db"])<=1.0 and abs(s["demod"]["error_db"])<=1.0 for s in rows_ok)

if SYN_ONLY:
    print("CHECK3 synthetic:", "PASS" if passA else "FAIL"); sys.exit(0 if passA else 1)
# ---- B) real files ----
c1 = json.load(open(os.path.join(out_dir,"check1.json"))); assets = {a["sha256"]:a for a in json.load(open(os.path.join(out_dir,"..","assets.json")))}
floor16 = -98.1 ; floor24 = -146.3         # white quantisation noise rms, dBFS (undithered 16 / 24 bit): 6.02*N+1.76 dB below full-scale sine => rms of noise = -(6.02N+1.76)+(-3.01)... see note
# full-scale sine rms = -3.01 dBFS; quantisation SNR = 6.02N+1.76 dB vs that. => noise rms = -3.01-(6.02N+1.76)
floor16 = -3.01-(6.02*16+1.76); floor24 = -3.01-(6.02*24+1.76)
real = []
for r in c1["real"]:
    h = r["sha256"]
    if "shape" not in r or r["sample_rate"] != 48000 or assets[h]["group"] == "human_reference": continue
    x, s = h1c.load_mono(h1c.analysis_path(analysis_dir, h), max_s=120)
    if len(x) < sr*8: continue
    kk = np.arange(120, 801)                       # lines 3.0 .. 20.0 kHz
    on, off, bw = demod_on_off(x, kk)
    pd = np.maximum(on-off, 1e-30); lv = dbf(pd); lv_on = dbf(on); lv_off = dbf(off)
    f, Pf = h1c.stft_pow(x, s)
    e = h1c.band_pow(f, Pf, 5000, 16000); lm = h1c.loud_mask(e); ns = h1c.db(e) > -80
    quiet = (~lm) & ns
    rows = []
    for lo, hi in zip(BARK_EDGES[:-1], BARK_EDGES[1:]):
        sel = (kk*25 >= lo) & (kk*25 < hi)
        comb_db = float(dbf(pd[sel].sum()))                       # total comb power (on-line minus neighbours) in this band, dBFS rms
        bp = h1c.band_pow(f, Pf, lo, hi)
        mask_loud = float(h1c.db(bp[lm].mean())) if lm.sum() > 20 else None
        mask_quiet = float(h1c.db(np.median(bp[quiet]))) if quiet.sum() > 20 else None
        ath_band = float(ath_spl((lo+hi)/2)) - SPL_OFFSET          # ATH of a tone, in dBFS rms under the assumed calibration
        rows.append({"band_hz":[lo,hi],"n_lines":int(sel.sum()),"comb_total_db":round(comb_db,1),"max_line_db":round(float(lv[sel].max()),1),"median_line_db":round(float(np.median(lv[sel])),1),
          "masker_loud_db":None if mask_loud is None else round(mask_loud,1),"mask_threshold_loud_db":None if mask_loud is None else round(mask_loud-MASK_OFFSET_DB,1),
          "masker_quiet_db":None if mask_quiet is None else round(mask_quiet,1),"mask_threshold_quiet_db":None if mask_quiet is None else round(mask_quiet-MASK_OFFSET_DB,1),
          "ath_tone_db":round(ath_band,1),
          "comb_above_loud_threshold": None if mask_loud is None else bool(comb_db > mask_loud-MASK_OFFSET_DB),
          "comb_above_quiet_threshold": None if mask_quiet is None else bool(comb_db > mask_quiet-MASK_OFFSET_DB),
          "comb_above_16bit_floor_in_band": bool(comb_db > floor16 + 10*np.log10((hi-lo)/(sr/2)))})
    real.append({"sha256":h,"group":assets[h]["group"],"seconds":round(len(x)/s,1),"periods_used":len(x)//P,"line_bandwidth_hz":round(bw,4),
       "median_line_db_3_20k":round(float(np.median(lv)),1),"max_line_db_3_20k":round(float(lv.max()),1),"median_on_line_db":round(float(np.median(lv_on)),1),"median_off_line_db":round(float(np.median(lv_off)),1),"lines_on_gt_off_pct":round(float(100*np.mean(on>off)),1),"n_loud":int(lm.sum()),"n_quiet":int(quiet.sum()),"bands":rows})
summ = {"n_files":len(real),
 "files_any_band_above_loud_threshold": sum(any(b["comb_above_loud_threshold"] for b in r["bands"] if b["comb_above_loud_threshold"] is not None) for r in real),
 "files_any_band_above_quiet_threshold": sum(any(b["comb_above_quiet_threshold"] for b in r["bands"] if b["comb_above_quiet_threshold"] is not None) for r in real),
 "files_any_band_above_16bit_floor": sum(any(b["comb_above_16bit_floor_in_band"] for b in r["bands"]) for r in real),
 "max_line_over_all_files_db": max((r["max_line_db_3_20k"] for r in real), default=None)}
res = {"assumed":{"spl_offset_db":SPL_OFFSET,"mask_offset_db":MASK_OFFSET_DB,"bark_edges_hz":BARK_EDGES,"ath":"Terhardt formula","note_spl":"SPL = dBFS_rms + 103 (-20 dBFS rms = 83 dB SPL). SPLIT.json wrote the same rule but called it a 'full-scale sine = 103'; a full-scale sine is -3 dBFS rms = 100 dB SPL under this rule."},
 "format_floors_dbfs_rms_white":{"16bit_undithered":round(floor16,1),"24bit":round(floor24,1),"mp3_aac":"UNKNOWN"},
 "synthetic":syn,"synthetic_pass":passA,"real":real,"summary":summ}
json.dump(res, open(os.path.join(out_dir,"check3.json"),"w"), indent=1)
for s in syn: print(s["background"], s["line_dbfs_true"], "welch err", s["welch"]["error_db"], "L/N", s["welch"]["line_over_noise_db"], "| demod err", s["demod"]["error_db"], "L/N", s["demod"]["line_over_noise_db"], "| fold-vs-direct", s["demod_fold_vs_direct_db"])
print("PASS A:", passA); print(json.dumps(summ))

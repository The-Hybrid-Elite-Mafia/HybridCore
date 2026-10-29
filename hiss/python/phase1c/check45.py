"""Checks 4 and 5: synthetic injection and sensitivity floor, on human cymbal-rich excerpts.
Cymbal-rich = the human-reference files with the largest 8-20 kHz share (picked by number, not by name).
Trials = 10 different excerpts (12 s each). An injected effect is 'recovered' when the measured change is within the stated tolerance of the known injected change."""
import json, sys, os, numpy as np
from scipy import signal
sys.path.insert(0, os.path.dirname(__file__))
import h1c
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
c2 = json.load(open(os.path.join(out_dir,"check2.json")))
hum = [r for r in c2["real"] if r["group"]=="human_reference" and r["qualifies"] and "C_8_20k_db" in r]
hum.sort(key=lambda r: r["C_8_20k_db"]-r["A_5_10k_db"], reverse=True)
SEG = 8.0
def make_pool(rows, nseg=10):
    out = []
    for r in rows:
        x, s_ = h1c.load_mono(h1c.analysis_path(analysis_dir, r["sha256"]), max_s=60)
        n = int(SEG*s_); best = sorted(((float(np.mean(x[st:st+n]**2)), st) for st in range(0, max(1,len(x)-n), s_)), reverse=True)
        picked = []
        for _, st in best:
            if all(abs(st-q) >= n for q in picked): picked.append(st)
            if len(picked) == 2: break
        for st in picked:
            if len(x[st:st+n]) == n: out.append((r["sha256"][:8], x[st:st+n].copy(), s_))
    return out[:nseg]
segs = make_pool(hum[:8]); segsB = make_pool(hum[8:16]); segsC = make_pool(hum[16:24])      # B = fresh excerpts, used only to test the version-2 flutter metric
assert len(segs) == 10 and len(segsB) == 10 and len(segsC) == 10, (len(segs), len(segsB), len(segsC))

# ---------- metrics ----------
def onsets(x, s):
    n, hop = 512, 128
    f, P = h1c.stft_pow(x, s, n, hop)
    m = (f>=1000)&(f<16000); L = np.log10(P[:,m]+1e-14)
    flux = np.maximum(np.diff(L,axis=0),0).mean(axis=1); flux = np.concatenate([[0],flux])
    thr = np.median(flux) + 3.0*np.median(np.abs(flux-np.median(flux)))
    mind = int(0.06*s/hop); peaks,_ = signal.find_peaks(flux, height=thr, distance=mind)
    return (peaks*hop + n/2)/s     # onset times, s
def post_onset_P(x, s, t_on, lo_hf=8000.0, hi_hf=20000.0, win=(0.005,0.050)):
    """P = 10 log10( sum E_hf[8-20k] / sum E_mid[2-8k] ) over frames 5-50 ms after each onset."""
    n, hop = 512, 128
    f, Pw = h1c.stft_pow(x, s, n, hop); tc = (np.arange(Pw.shape[0])*hop + n/2)/s
    hi = min(hi_hf, s/2.0)
    hf = h1c.band_pow(f,Pw,lo_hf,hi); mid = h1c.band_pow(f,Pw,2000,8000)
    eh = em = 0.0; k = 0
    for t0 in t_on:
        sel = (tc>=t0+win[0])&(tc<=t0+win[1])
        if sel.sum()==0: continue
        eh += hf[sel].sum(); em += mid[sel].sum(); k += 1
    return float(h1c.db(eh)-h1c.db(em)), k, float(eh)
def mod_depth(x, s, f_mod=25.0):
    sos = signal.butter(4,[5000,min(15000,s/2*0.9)],btype="band",fs=s,output="sos"); b = signal.sosfiltfilt(sos,x)
    env = np.abs(signal.hilbert(b)); lp = signal.butter(4,200,fs=s,output="sos"); env = signal.sosfiltfilt(lp,env)
    e = env - env.mean(); w = np.hanning(len(e)); E = np.fft.rfft(e*w); A = 2*np.abs(E)/w.sum(); fr = np.fft.rfftfreq(len(e),1/s)
    k = np.argmin(np.abs(fr-f_mod)); a25 = A[max(0,k-1):k+2].max()
    nb = (((fr>=f_mod-5)&(fr<=f_mod-2))|((fr>=f_mod+2)&(fr<=f_mod+5)))
    an2 = np.mean(A[nb]**2)
    return float(np.sqrt(max(a25**2-an2,0.0))/env.mean())
def mod_depth2(x, s, f_mod=25.0):
    """Version 2. Flatten the slow loudness swings first (divide the envelope by its own 10 Hz low-pass), so hits do not smear the 25 Hz reading."""
    sos = signal.butter(4,[5000,min(15000,s/2*0.9)],btype="band",fs=s,output="sos"); b = signal.sosfiltfilt(sos,x)
    env = np.abs(signal.hilbert(b)); lp = signal.butter(4,200,fs=s,output="sos"); env = signal.sosfiltfilt(lp,env)
    slow = signal.sosfiltfilt(signal.butter(2,10,fs=s,output="sos"), env); r = np.clip(env/np.maximum(slow, 0.2*np.median(slow)) - 1.0, -1.0, 2.0)   # floor + clip: v2.1 (v2 divided by ~0 in gaps and blew up)
    w = np.hanning(len(r)); E = np.fft.rfft(r*w); A = 2*np.abs(E)/w.sum(); fr = np.fft.rfftfreq(len(r),1/s)
    k = np.argmin(np.abs(fr-f_mod)); a25 = A[max(0,k-1):k+2].max()
    nb = (((fr>=f_mod-5)&(fr<=f_mod-2))|((fr>=f_mod+2)&(fr<=f_mod+5)))
    return float(np.sqrt(max(a25**2-np.mean(A[nb]**2),0.0)))
def notch_track(x, s, nblk):
    f, P = signal.welch(x, fs=s, nperseg=2048, noverlap=1024); lt = 10*np.log10(P+1e-30)
    blk = len(x)//nblk; cen = []
    for i in range(nblk):
        f2, P2 = signal.welch(x[i*blk:(i+1)*blk], fs=s, nperseg=2048, noverlap=1024)
        d = 10*np.log10(P2+1e-30) - lt; d = h1c.smooth_db(f2, d+100, 300)-100   # smooth (shifted so dB mean stays positive)
        m = (f2>=6000)&(f2<=16000); cen.append(float(f2[m][np.argmin(d[m])]))
    return np.array(cen)
def moving_notch(x, s, f_start, f_end, g, sigma=300.0):
    fr, tt, Z = signal.stft(x, fs=s, nperseg=2048, noverlap=1536, window="hann")
    f0 = np.interp(tt, [tt[0], tt[-1]], [f_start, f_end])[None,:]
    gain = 1.0 - g*np.exp(-0.5*((fr[:,None]-f0)/sigma)**2)
    _, y = signal.istft(Z*gain, fs=s, nperseg=2048, noverlap=1536, window="hann")
    return y[:len(x)] if len(y)>=len(x) else np.pad(y,(0,len(x)-len(y)))

rng = np.random.default_rng(11)
def hf_noise(n, s, lo=16000.0):
    w = rng.standard_normal(n); sos = signal.butter(8, lo, btype="high", fs=s, output="sos")
    hi = min(20000.0, s/2*0.97); sos2 = signal.butter(8, hi, btype="low", fs=s, output="sos")
    return signal.sosfilt(sos2, signal.sosfilt(sos, w))

# ---------- injections ----------
def inject_air(x, s, dB):
    """Add steady 16-20 kHz noise so the loud-frame 16-20 kHz power rises by dB. Truth = dB."""
    m0 = h1c.ratio_metrics(x, s); n0 = hf_noise(len(x), s, 16000.0)
    fN, PN = h1c.stft_pow(n0, s); Nbar = h1c.band_pow(fN,PN,16000,min(20000.0,s/2)).mean()
    B0 = 10**(m0["B_16_20k_db"]/10); want = B0*(10**(dB/10)-1)
    y = x + n0*np.sqrt(want/Nbar)
    m1 = h1c.ratio_metrics(y, s)
    fx, Px = h1c.stft_pow(x, s); mk = h1c.loud_mask(h1c.band_pow(fx,Px,5000,16000))
    m1f = h1c.ratio_metrics(y, s, mask=mk)
    inject_air.last_fixed = m1f["B_16_20k_db"]-m0["B_16_20k_db"]
    return y, dB, m1["B_16_20k_db"]-m0["B_16_20k_db"]
def inject_smear(x, s, q, tau=0.020):
    """After each onset add a decaying 8-20 kHz burst (time constant tau). Size set so its energy in the 5-50 ms window = q x the original HF energy there."""
    t_on = onsets(x, s); P0, k0, eh0 = post_onset_P(x, s, t_on)
    comp = np.zeros_like(x); n = len(x)
    hn = rng.standard_normal(n); sos = signal.butter(8,[8000,min(19500,s/2*0.97)],btype="band",fs=s,output="sos"); hn = signal.sosfilt(sos,hn)
    for t0 in t_on:
        i0 = int(t0*s); L = min(n-i0, int(0.2*s)); comp[i0:i0+L] += hn[i0:i0+L]*np.exp(-np.arange(L)/(tau*s))
    # scale so injected energy (8-20k, window after the SAME onsets) = q * original
    _, _, eh_c = post_onset_P(comp, s, t_on)
    comp *= np.sqrt(q*eh0/eh_c)
    y = x + comp
    truth = 10*np.log10((eh0 + q*eh0)/eh0) if False else None
    # truth: energy add is incoherent, so the change in P is 10log10(1+q) only if the mid band (2-8k) is unchanged -> it is (burst is 8-20k)
    P1, k1, _ = post_onset_P(y, s, onsets(y, s))
    return y, float(10*np.log10(1+q)), P1-P0, k0, k1
def inject_flutter(x, s, m_idx):
    sos = signal.butter(4,[5000,min(15000,s/2*0.9)],btype="band",fs=s,output="sos"); b = signal.sosfiltfilt(sos,x)
    t = np.arange(len(x))/s; y = x + m_idx*np.sin(2*np.pi*25.0*t + rng.random()*6.283)*b
    return y, m_idx, mod_depth(y, s), mod_depth(x, s), mod_depth2(y, s), mod_depth2(x, s)

# ---------- CHECK 4 ----------
TOL = {"air_db":1.0, "smear_db":1.5, "flutter_rel":0.20, "notch_hz":200.0}
c4 = {"tolerances":TOL, "air":[], "air_fixed_mask":[], "flutter_v2_1_setC_fresh":[], "smear":[], "flutter":[], "flutter_v2_setA":[], "flutter_v2_setB_fresh":[], "notch":[]}
for name, x, s in segs:
    y, tr, ms = inject_air(x, s, 6.0); c4["air"].append({"seg":name,"true_db":tr,"measured_db":round(ms,2),"ok":abs(ms-tr)<=TOL["air_db"]})
    c4["air_fixed_mask"].append({"seg":name,"true_db":tr,"measured_db":round(inject_air.last_fixed,2),"ok":abs(inject_air.last_fixed-tr)<=TOL["air_db"]})
    y, tr, ms, k0, k1 = inject_smear(x, s, 0.5); c4["smear"].append({"seg":name,"true_db":round(tr,2),"measured_db":round(ms,2),"onsets_before":k0,"onsets_after":k1,"ok":abs(ms-tr)<=TOL["smear_db"]})
    y, tr, ms, base, ms2, base2 = inject_flutter(x, s, 0.30)
    c4["flutter_v2_setA"].append({"seg":name,"true_depth":tr,"measured_depth":round(ms2,3),"baseline_depth":round(base2,3),"recovered_rel_err":round(abs((ms2-base2)-tr)/tr,3),"ok":abs((ms2-base2)-tr)/tr<=TOL["flutter_rel"]})
    c4["flutter"].append({"seg":name,"true_depth":tr,"measured_depth":round(ms,3),"baseline_depth":round(base,3),"recovered_rel_err":round(abs((ms-base)-tr)/tr,3),"ok":abs((ms-base)-tr)/tr<=TOL["flutter_rel"]})
    nb = 12; y = moving_notch(x, s, 13000.0, 8000.0, 1.0)
    cen = notch_track(y, s, nb); blk = len(x)//nb
    tc = (np.arange(nb)+0.5)*blk/ s; tf = np.interp(tc/ (len(x)/s), [0,1], [13000.0, 8000.0])
    err = float(np.median(np.abs(cen-tf))); corr = float(np.corrcoef(cen, tf)[0,1]); base_cen = notch_track(x, s, nb)
    c4["notch"].append({"seg":name,"median_err_hz":round(err,0),"corr_with_truth":round(corr,3),"direction_ok":bool(np.polyfit(tc,cen,1)[0]<0),"ok":bool(err<=TOL["notch_hz"] and np.polyfit(tc,cen,1)[0]<0)})
for name, x, s_ in segsB:
    y, tr, ms, base, ms2, base2 = inject_flutter(x, s_, 0.30)
    c4["flutter_v2_setB_fresh"].append({"seg":name,"true_depth":tr,"measured_depth":round(ms2,3),"baseline_depth":round(base2,3),"recovered_rel_err":round(abs((ms2-base2)-tr)/tr,3),"ok":abs((ms2-base2)-tr)/tr<=TOL["flutter_rel"],
        "v1_ok_same_segment": abs((ms-base)-tr)/tr<=TOL["flutter_rel"]})
for name, x, s_ in segsC:
    y, tr, ms, base, ms2, base2 = inject_flutter(x, s_, 0.30)
    c4["flutter_v2_1_setC_fresh"].append({"seg":name,"true_depth":tr,"measured_depth":round(ms2,3),"baseline_depth":round(base2,3),"recovered_rel_err":round(abs((ms2-base2)-tr)/tr,3),"ok":abs((ms2-base2)-tr)/tr<=TOL["flutter_rel"],"v1_ok_same_segment":abs((ms-base)-tr)/tr<=TOL["flutter_rel"]})
for k in ("air","air_fixed_mask","smear","flutter","flutter_v2_setA","flutter_v2_setB_fresh","flutter_v2_1_setC_fresh","notch"):
    c4[k+"_pass_count"] = sum(r["ok"] for r in c4[k]); c4[k+"_pass"] = c4[k+"_pass_count"] >= 9
c4["gate_pass_with_v1_flutter"] = all(c4[k+"_pass"] for k in ("air","smear","flutter","notch"))
c4["gate_pass_with_v2_1_flutter"] = all(c4[k+"_pass"] for k in ("air","smear","flutter_v2_1_setC_fresh","notch"))
c4["note"] = "Flutter v1 failed (set A). v2 had a numeric bug (divided by ~0) and failed on sets A and B. v2.1 fixes the bug and is judged only on set C, fresh excerpts it was never run on."
json.dump(c4, open(os.path.join(out_dir,"check4.json"),"w"), indent=1)
print("CHECK4", {k:c4[k+"_pass_count"] for k in ("air","air_fixed_mask","smear","flutter","flutter_v2_setA","flutter_v2_setB_fresh","flutter_v2_1_setC_fresh","notch")}, "gate v1", c4["gate_pass_with_v1_flutter"], "gate v2.1", c4["gate_pass_with_v2_1_flutter"])

# ---------- CHECK 5: sensitivity floor ----------
JIT = (197, 411, 677, 1013, 1500)
def jitter_thr(fn):
    d = []
    for name, x, s_ in segs:
        base = fn(x, s_)
        for j in JIT: d.append(abs(fn(x[j:], s_) - base))
    return float(np.percentile(d, 95))
thr_air  = jitter_thr(lambda x, s_: h1c.ratio_metrics(x, s_)["B_16_20k_db"])
thr_smear= jitter_thr(lambda x, s_: post_onset_P(x, s_, onsets(x, s_))[0])
thr_flut1= jitter_thr(mod_depth); thr_flut2 = jitter_thr(mod_depth2)   # v2.1
c5 = {"detection_rule":"A change counts as detected when it is larger than the 95th percentile of the metric's own repeatability (how much it moves when only the analysis start is shifted by 197-1500 samples), over 10 excerpts x 5 shifts. Floor = smallest rung detected in >= 9 of 10 trials.",
      "repeatability_thresholds":{"air_16_20k_db":round(thr_air,3),"post_onset_P_db":round(thr_smear,3),"flutter_v1_depth":round(thr_flut1,4),"flutter_v2_1_depth":round(thr_flut2,4)},"ladders":{}}
def ladder(name, rungs, trial):
    rows = []
    for r in rungs:
        ok = sum(bool(trial(r, x, s_)) for _, x, s_ in segs)
        rows.append({"rung":r,"detected_of_10":ok})
    fl = next((q["rung"] for q in rows if q["detected_of_10"]>=9 and all(z["detected_of_10"]>=9 for z in rows if z["rung"]>=q["rung"])), None)
    c5["ladders"][name] = {"rungs":rows,"floor":fl}
def t_air(r, x, s_):  _, tr, ms = inject_air(x, s_, r); return ms >= thr_air
def t_air_fx(r, x, s_):  inject_air(x, s_, r); return inject_air.last_fixed >= thr_air
def t_smear(r, x, s_): _, tr, ms, k0, k1 = inject_smear(x, s_, r); return ms >= thr_smear
def t_fl1(r, x, s_): _, tr, ms, base, ms2, base2 = inject_flutter(x, s_, r); return (ms-base) >= thr_flut1
def t_fl2(r, x, s_): _, tr, ms, base, ms2, base2 = inject_flutter(x, s_, r); return (ms2-base2) >= thr_flut2
def t_notch(g, x, s_):
    y = moving_notch(x, s_, 13000.0, 8000.0, g); cen = notch_track(y, s_, 12); blk = len(x)//12
    tc = (np.arange(12)+0.5)*blk/s_; tf = np.interp(tc/(len(x)/s_), [0,1], [13000.0, 8000.0])
    return np.median(np.abs(cen-tf)) <= 200.0 and np.polyfit(tc,cen,1)[0] < 0
ladder("air_db", [0.25,0.5,1.0,2.0,3.0,6.0], t_air)
ladder("air_db_fixed_loud_mask", [0.25,0.5,1.0,2.0,3.0,6.0], t_air_fx)
ladder("smear_q_(energy fraction of the original HF energy in the 5-50 ms window)", [0.05,0.1,0.25,0.5,1.0,2.0], t_smear)
ladder("flutter_v1_depth", [0.02,0.05,0.1,0.2,0.3,0.5], t_fl1)
ladder("flutter_v2_1_depth", [0.02,0.05,0.1,0.2,0.3,0.5], t_fl2)
ladder("moving_notch_g_(1.0 = full notch at its centre)", [0.1,0.2,0.3,0.5,0.8,1.0], t_notch)
# comb floors
def fold_score(x, s_, Pn):
    ks = np.arange(int(3000/(s_/Pn)), int(20000/(s_/Pn))+1); Q = 2*Pn; M = len(x)//Q
    y = x[:M*Q].reshape(M,Q).mean(axis=0); Y = np.fft.rfft(y)/Q; pw = 2*np.abs(Y)**2
    on = pw[2*ks]; off = 0.5*(pw[2*ks-1]+pw[2*ks+1]); return float(np.median(10*np.log10((on+1e-30)/(off+1e-30))))
def comb_trial(bg, s_, Lk, crng):
    Pn = int(round(s_/25.0)); t = np.arange(len(bg))/s_; x = bg.copy(); amp = np.sqrt(2)*10**(Lk/20)
    ks = np.arange(120, 801)
    ph = crng.random(len(ks))*6.283; one = np.zeros(Pn)
    tt = np.arange(Pn)/s_
    for k, p in zip(ks, ph): one += amp*np.sin(2*np.pi*k*25*tt + p)
    reps = int(np.ceil(len(x)/Pn)); x += np.tile(one, reps)[:len(x)]
    nulls = [fold_score(x, s_, Pn+d) for d in range(13, 61, 3)]
    thr = float(np.max(nulls)); return fold_score(x, s_, Pn) > thr
crng = np.random.default_rng(5)
Ls = [-155,-150,-145,-140,-135,-130,-125,-120,-115,-110,-105,-100,-95,-90]
comb = {}
for bgname in ("silence_16bit_TPDF_floor_20s_48k","under_loud_human_cymbals_8s"):
    rows = []
    for Lk in Ls:
        ok = 0
        for i in range(10):
            if bgname.startswith("silence"):
                s_ = 48000; bg = crng.standard_normal(20*s_)*10**(-96.3/20)
            else:
                _, bg, s_ = segs[i]
            ok += comb_trial(bg, s_, Lk, crng)
        rows.append({"line_dbfs":Lk,"detected_of_10":ok})
    fl = next((q["line_dbfs"] for q in rows if q["detected_of_10"]>=9 and all(z["detected_of_10"]>=9 for z in rows if z["line_dbfs"]>=q["line_dbfs"])), None)
    comb[bgname] = {"rungs":rows,"floor_line_dbfs":fl}
c5["comb_floor"] = comb
c5["comb_note"] = "Floors scale with listening time: longer files lower the floor by about 5 dB per 10x in length (averaging gain). 20 s and 8 s are used here; a 3-minute song would do about 4 dB (vs 20 s) / 7 dB (vs 8 s) better. ASSUMED scaling, not measured."
c5["gate_pass"] = all(v["floor"] is not None for k, v in c5["ladders"].items() if "flutter_v1" not in k and "flutter_v2_1" not in k) and all(v["floor_line_dbfs"] is not None for v in comb.values())
json.dump(c5, open(os.path.join(out_dir,"check5.json"),"w"), indent=1)
print("CHECK5 floors:", {k:v["floor"] for k,v in c5["ladders"].items()}, {k:v["floor_line_dbfs"] for k,v in comb.items()}, "gate", c5["gate_pass"])

"""Blind, level-matched A/B listening pack for the comb remover.

usage: python hiss_abpack.py <orig_dir> <tool_out_dir> <pack_dir> [seed]

Writes <pack_dir>/index.html plus lossless 24-bit WAV clips (lossy files would hide the very thing being judged).
Each trial has two clips, A and B. In real trials one is the original and one is the processed file, in random order.
Catch trials play the SAME file twice: they show how often a listener "hears" a difference that is not there.
The processed clip is level-matched to the original (loudness gain applied to the processed clip).
The answer key is inside the page but hidden until the listener presses "Finish".

Quiet clips are boosted by a fixed gain (the same for A and B) so the quiet floor is audible; the page says so.
"""
import base64
import glob
import json
import os
import random
import sys

import numpy as np
import soundfile as sf

CLIP_S = 4.0
BOOST_DB = 24.0


def rms_db(x):
    return 10 * np.log10(max(float(np.mean(x ** 2)), 1e-30))


def pick_clips(orig, fs, rng, n_quiet, n_loud):
    """quiet clips: windows whose level is in the quietest 15 % of windows that are not digital silence; loud: top 30 %"""
    n = int(CLIP_S * fs)
    starts = np.arange(0, len(orig) - n, n // 2)
    lv = np.array([rms_db(orig[s:s + n]) for s in starts])
    live = lv > -80
    if live.sum() < 4:
        return [], []
    order = np.argsort(np.where(live, lv, np.inf))
    nl = int(live.sum())
    q = [starts[i] for i in order[:max(1, int(0.15 * nl))]]
    l = [starts[i] for i in order[::-1] if live[i]][:max(1, int(0.30 * nl))]
    rng.shuffle(q)
    rng.shuffle(l)
    return q[:n_quiet], l[:n_loud]


def main():
    orig_dir, out_dir, pack = sys.argv[1:4]
    seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    rng = random.Random(seed)
    os.makedirs(os.path.join(pack, "clips"), exist_ok=True)
    touched = []
    for f in sorted(glob.glob(os.path.join(orig_dir, "*.wav"))):
        name = os.path.splitext(os.path.basename(f))[0]
        res = os.path.join(out_dir, name + "_result.json")
        if os.path.exists(res) and json.load(open(res))["touched"]:
            touched.append((name, f, os.path.join(out_dir, name + "_fixed.wav")))
    trials = []
    cid = 0

    def write_clip(x, fs):
        nonlocal cid
        cid += 1
        fn = "clips/c%02d.wav" % cid
        sf.write(os.path.join(pack, fn), x.astype(np.float32), fs, format="WAV", subtype="PCM_24")
        return fn

    for name, fo, ff in touched:
        orig, fs = sf.read(fo, always_2d=True, dtype="float64")
        fixed, _ = sf.read(ff, always_2d=True, dtype="float64")
        quiet, loud = pick_clips(orig, fs, rng, 2, 1)
        for kind, starts in (("quiet", quiet), ("normal", loud)):
            for s in starts:
                n = int(CLIP_S * fs)
                a, b = orig[s:s + n].copy(), fixed[s:s + n].copy()
                # level match: processed clip gets the gain that makes it as loud as the original
                ra, rb = rms_db(a), rms_db(b)
                match = ra - rb
                b *= 10 ** (match / 20)
                gain = BOOST_DB if kind == "quiet" else 0.0
                g = 10 ** (gain / 20)
                peak = max(np.abs(a * g).max(), np.abs(b * g).max())
                if peak > 0.5:      # never louder than -6 dBFS
                    g *= 0.5 / peak
                    gain = 20 * np.log10(g)
                a, b = np.clip(a * g, -1, 1), np.clip(b * g, -1, 1)
                fade = int(0.02 * fs)
                w = np.ones(n)
                w[:fade] = np.linspace(0, 1, fade)
                w[-fade:] = np.linspace(1, 0, fade)
                a, b = a * w[:, None], b * w[:, None]
                flip = rng.random() < 0.5
                first, second = (b, a) if flip else (a, b)
                trials.append({"kind": kind, "stem": name, "a": write_clip(first, fs), "b": write_clip(second, fs),
                               "processed_is": "A" if flip else "B", "catch": False, "boost_db": round(gain, 1), "match_db": round(match, 3)})
    # catch trials: the same clip twice
    if touched:
        name, fo, ff = touched[0]
        orig, fs = sf.read(fo, always_2d=True, dtype="float64")
        quiet, _ = pick_clips(orig, fs, rng, 2, 0)
        for s in quiet:
            n = int(CLIP_S * fs)
            g = 10 ** (BOOST_DB / 20)
            a = np.clip(orig[s:s + n] * g * min(1.0, 0.5 / max(1e-9, np.abs(orig[s:s + n] * g).max())), -1, 1)
            fn1, fn2 = write_clip(a, fs), write_clip(a, fs)
            trials.append({"kind": "quiet", "stem": name, "a": fn1, "b": fn2, "processed_is": "none", "catch": True, "boost_db": BOOST_DB, "match_db": 0.0})
    rng.shuffle(trials)
    key = base64.b64encode(json.dumps([t["processed_is"] for t in trials]).encode()).decode()
    pub = [{"kind": t["kind"], "a": t["a"], "b": t["b"], "boost": t["boost_db"]} for t in trials]
    html = HTML.replace("__TRIALS__", json.dumps(pub)).replace("__KEY__", key)
    with open(os.path.join(pack, "index.html"), "w", encoding="utf-8") as fh:
        fh.write(html)
    with open(os.path.join(pack, "KEY_DO_NOT_OPEN_UNTIL_DONE.json"), "w") as fh:
        json.dump([{"trial": i + 1, **t} for i, t in enumerate(trials)], fh, indent=1)
    print("trials", len(trials), "clips", cid)


HTML = r"""<title>HEM Hiss Blind Test</title>
<meta name="claude:viewport-fit-sides" content="cover">
<style>
:root{--bg:#f3f2ee;--fg:#1b1d1f;--mut:#5f6468;--card:#fcfcfa;--line:#d4d6d2;--acc:#a1361c}
@media (prefers-color-scheme:dark){:root:not([data-theme="light"]){--bg:#15181a;--fg:#e8eaea;--mut:#9aa1a5;--card:#1e2225;--line:#33393d;--acc:#ee8a68;color-scheme:dark}}
:root[data-theme="dark"]{--bg:#15181a;--fg:#e8eaea;--mut:#9aa1a5;--card:#1e2225;--line:#33393d;--acc:#ee8a68;color-scheme:dark}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,sans-serif;padding-block:20px 60px;padding-left:max(16px,env(safe-area-inset-left,0px));padding-right:max(16px,env(safe-area-inset-right,0px))}
main{max-width:640px;margin:0 auto}
h1{font-size:1.4rem;margin:.2em 0;text-wrap:balance}.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px;margin:14px 0}
button{font:inherit;padding:10px 14px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer;margin:4px 4px 4px 0}
button:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
button.pick{border-color:var(--acc);color:var(--acc);font-weight:600}button.on{background:var(--acc);color:var(--bg)}
.mut{color:var(--mut);font-size:.9rem}audio{width:100%;margin:4px 0}.warn{border-left:4px solid var(--acc)}
</style><main>
<h1>Blind listening test</h1>
<p class="mut">For each pair, listen to A and B as many times as you like. Say which one has LESS of the thin, shimmery hiss on top - or "no difference". Your answers are checked only when you press Finish.</p>
<div class="card warn"><b>Careful with volume.</b> Start with your volume LOW. Clips marked "boosted" are quiet parts made 24 dB louder so the hiss can be heard. Both clips in a pair are boosted by the same amount and are level-matched. Use the same headphones for every pair.</div>
<div id="app"></div>
<script>
const T=__TRIALS__, KEY=JSON.parse(atob("__KEY__"));
const ans=new Array(T.length).fill(null);
const app=document.getElementById("app");
T.forEach((t,i)=>{
 const d=document.createElement("div");d.className="card";
 d.innerHTML=`<b>Pair ${i+1} of ${T.length}</b> <span class="mut">${t.kind==="quiet"?"(quiet part, boosted "+t.boost+" dB)":"(normal level)"}</span>
 <div>A</div><audio controls preload="none" src="${t.a}"></audio>
 <div>B</div><audio controls preload="none" src="${t.b}"></audio>
 <div>Less hiss:</div>`;
 ["A","B","No difference"].forEach(v=>{
  const b=document.createElement("button");b.textContent=v;b.onclick=()=>{ans[i]=v;[...d.querySelectorAll("button")].forEach(x=>x.classList.remove("on"));b.classList.add("on")};
  d.appendChild(b)});
 app.appendChild(d)});
const fin=document.createElement("button");fin.textContent="Finish and show the key";fin.className="pick";
fin.onclick=()=>{
 if(ans.includes(null)&&!confirm("Some pairs have no answer. Finish anyway?"))return;
 let real=0,prefProc=0,prefOrig=0,nodiff=0,catchN=0,catchSaw=0;
 const rows=T.map((t,i)=>{const k=KEY[i];let r="";
  if(k==="none"){catchN++;if(ans[i]==="A"||ans[i]==="B")catchSaw++;r="same clip twice - you said: "+ans[i];}
  else{real++;if(ans[i]==="No difference"||ans[i]===null)nodiff++;else if(ans[i]===k)prefProc++;else prefOrig++;
   r="processed was "+k+" - you said: "+ans[i];}
  return `<div>Pair ${i+1}: ${r}</div>`}).join("");
 document.getElementById("result").innerHTML=`<div class="card"><b>Result</b>
 <p>Real pairs: ${real}. Picked the processed one as cleaner: ${prefProc}. Picked the original: ${prefOrig}. No difference: ${nodiff}.</p>
 <p>Catch pairs (identical clips): ${catchN}. You still named one as cleaner ${catchSaw} time(s) - that is how often a difference is "heard" when there is none.</p>
 <p class="mut">With only a handful of pairs this is a rough guide, not proof. If you cannot beat the catch pairs, the hiss is probably below what you can hear on this gear.</p>${rows}</div>`;
 fin.disabled=true};
app.appendChild(fin);
</script><div id="result"></div></main>
"""

if __name__ == "__main__":
    main()

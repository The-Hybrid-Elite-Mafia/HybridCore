"""POST-HOC repair of the sensitivity table in check4b (see FAILURES.md F12). Not part of the pre-registered pass rule."""
import json, sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
import h1c, importlib.util
out_dir, analysis_dir = sys.argv[1], sys.argv[2]
src = open(os.path.join(os.path.dirname(__file__), "check4b.py")).read()
head = src[:src.index("if SYN_ONLY:")]
sys.argv = [sys.argv[0], out_dir, analysis_dir]; exec(compile(head, "c4b", "exec"))
c4b = json.load(open(os.path.join(out_dir, "check4b.json"))); names = [r["clip"] for r in c4b["clips"]]
c2 = json.load(open(os.path.join(out_dir, "check2.json")))
hum = [r for r in c2["real"] if r["group"] == "human_reference" and "C_8_20k_db" in r]
hum.sort(key=lambda r: r["C_8_20k_db"]-r["A_5_10k_db"], reverse=True)
clips = []
for r in hum:
    if r["sha256"][:8] in names:
        x, s = h1c.load_mono(h1c.analysis_path(analysis_dir, r["sha256"]), max_s=40); clips.append((r["sha256"][:8], x[:int(19.5*s)], s))
base = [flutter_depth(x, s) for _, x, s in clips]; thr = max(base)
rng = np.random.default_rng(777); lad = []
for m in (0.1, 0.2, 0.3, 0.4, 0.5, 0.7):
    rd = [flutter_depth(inject(x, s, m, rng), s) for _, x, s in clips]
    lad.append({"injected_depth": m, "mean_reading": round(float(np.mean(rd)), 3), "above_highest_clean_reading_of_10": int(sum(v > thr for v in rd))})
floor = next((q["injected_depth"] for q in lad if q["above_highest_clean_reading_of_10"] >= 9 and all(z["above_highest_clean_reading_of_10"] >= 9 for z in lad if z["injected_depth"] >= q["injected_depth"])), None)
res = {"label": "POST-HOC (after seeing the failure)", "highest_clean_reading": round(thr, 4), "clean_readings": [round(b, 3) for b in base], "ladder": lad,
       "detect_only_floor_depth": floor, "detect_only_rule_proposed": f"flag flutter as PRESENT only if depth reading > {round(thr, 3)}; below that say 'not shown'",
       "gain_note": "reading / injected depth is about 0.73 on these clips"}
json.dump(res, open(os.path.join(out_dir, "check4b_posthoc.json"), "w"), indent=1); print(json.dumps(res))

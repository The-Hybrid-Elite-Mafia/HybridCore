"""Phase 2 (small) human air table. Usage: python3 -I air_table.py <plan.json> <human_dir> <out.json>
Reads the pre-registered plan, checks every SHA-256, measures with the Phase 1 tools, writes the table."""
import sys, os, json, hashlib, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "phase1c"))
import h1c

plan = json.load(open(sys.argv[1])); base = sys.argv[2]; out = sys.argv[3]
rows = []
for it in plan["included_files"]:
    p = os.path.join(base, it["song"], it["file"])
    raw = open(p, "rb").read()
    if hashlib.sha256(raw).hexdigest() != it["sha256"]:
        raise SystemExit("HASH MISMATCH " + it["file"])
    x, sr = h1c.load_mono(p)
    row = {"song": it["song"], "file": it["file"], "role": it["role"], "rate": sr, "seconds": round(len(x)/sr, 1)}
    row["cutoff"] = h1c.cutoff_audit(x, sr)
    if row["cutoff"].get("cutoff_hz", 0) < 19000:
        row["status"] = "EXCLUDED_CUTOFF_BELOW_19K"
    else:
        m = h1c.ratio_metrics(x, sr)
        row["metrics"] = m
        row["status"] = "OK" if m and "ratio_16_20_over_5_10_db" in m else "TOO_FEW_LOUD_FRAMES"
    rows.append(row)

summary = {}
for role in sorted({r["role"] for r in rows}):
    ok = [r for r in rows if r["role"] == role and r["status"] == "OK"]
    v = np.array([r["metrics"]["ratio_16_20_over_5_10_db"] for r in ok])
    B = np.array([r["metrics"]["B_16_20k_db"] for r in ok])
    s = {"n_ok": len(ok), "n_listed": sum(1 for r in rows if r["role"] == role)}
    if len(ok):
        s.update({"ratio_median_db": float(np.median(v)), "ratio_min_db": float(v.min()), "ratio_max_db": float(v.max()),
                  "air_16_20k_median_db": float(np.median(B))})
    s["interval_95"] = list(h1c.boot_ci(v)) if len(ok) >= 5 else "no interval (n too small)"
    summary[role] = s
json.dump({"plan_written": plan["written"], "rows": rows, "summary_by_role": summary}, open(out, "w"), indent=1)
print(json.dumps(summary, indent=1))
for r in rows:
    m = r.get("metrics") or {}
    print(f'{r["role"]:13s} {r["file"]:22s} {r["rate"]} {r["seconds"]:5.1f}s {r["cutoff"]["shape"]:6s} {r["cutoff"].get("cutoff_hz")} {r["status"]:10s} A={m.get("A_5_10k_db","-")} B={m.get("B_16_20k_db","-")} ratio={m.get("ratio_16_20_over_5_10_db","-")} loud={m.get("n_loud","-")}/{m.get("n_frames","-")}')

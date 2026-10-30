"""Builds RESULTS.json from the six check files. Regenerable: python make_results.py <results_dir>"""
import json, sys, os, hashlib
d = sys.argv[1]
L = lambda n: json.load(open(os.path.join(d, n)))
c1, c2, c3, c4, c5, c6 = (L(f"check{i}.json") for i in range(1, 7))
c4b, c4p, c4c, c7 = L("check4b.json"), L("check4b_posthoc.json"), L("check4c.json"), L("check7.json")
real1 = [r for r in c1["real"] if "shape" in r]
gates = {
 "1_bandwidth_audit": {"pass": bool(c1["synthetic_all_ok"]), "synthetic_cases": len(c1["synthetic"]), "synthetic_cases_ok": sum(x["ok"] for x in c1["synthetic"]),
    "real_unique_files": c1["real_summary"]["n_unique_files"], "real_qualify_cutoff_ge_19k": c1["real_summary"]["n_qualify_ge_19k"], "by_shape": c1["real_summary"]["by_shape"]},
 "2_metric_sanity": {"pass": bool(c2["synthetic_cut_ok"]), "synthetic": c2["synthetic"][:3], "real_files": len(c2["real"])},
 "3_line_level_sanity": {"pass": bool(c3["synthetic_pass"]), "real_files": c3["summary"]["n_files"], "summary": c3["summary"], "floors": c3["format_floors_dbfs_rms_white"]},
 "4_synthetic_injection": {"pass": bool(c4["air_pass"] and c4["smear_pass"] and c4["notch_pass"] and c4c["pass"]),
    "flutter_v5_trials_ok": f'{c4c["n_ok"]}/{c4c["n"]}', "flutter_v5_rule": "SPLIT-ADDENDUM-3-2026-10-29.json, passed on 9 never-used clips x 3 depths. Versions v1-v4 FAILED first; see FAILURES.md",
    "air_of_10": c4["air_pass_count"], "air_fixed_mask_of_10": c4["air_fixed_mask_pass_count"], "smear_of_10": c4["smear_pass_count"], "notch_of_10": c4["notch_pass_count"],
    "flutter_v1_of_10": c4["flutter_pass_count"], "flutter_v2_setA_of_10_numeric_bug": c4["flutter_v2_setA_pass_count"], "flutter_v2_1_setC_fresh_of_10": c4["flutter_v2_1_setC_fresh_pass_count"],
    "failed_first": ["25 Hz flutter depth v1, v2, v4 (v5 passes)"],
    "flutter_v4_fresh_clips_of_10": c4b["pass_count"], "flutter_v4_readings_mean": c4b["readings_mean"], "flutter_v4_note": "pre-registered in SPLIT-ADDENDUM-2026-10-29.json; FAILED (my 19.5 s slip put the 25 Hz line half way between FFT bins)",
    "flutter_detect_floor_posthoc": c4p["detect_only_floor_depth"], "flutter_threshold_posthoc": c4p["highest_clean_reading"],
    "correction": "check5.json flutter floors (0.2) used a bad threshold (FAILURES.md F12). v5 was tested down to 0.15 only; below that is UNKNOWN"},
 "5_sensitivity_floor": {"pass": bool(c5["gate_pass"]), "floors": {k: v["floor"] for k, v in c5["ladders"].items()},
    "comb_floor_line_dbfs": {k: v["floor_line_dbfs"] for k, v in c5["comb_floor"].items()}, "repeatability": c5["repeatability_thresholds"]},
 "7_cpp_meters_three_way": {"pass": c7["n_ok"] == c7["n"], "files_agree": f'{c7["n_ok"]}/{c7["n"]}', "tools": ["C++23 HissMeters", "Python h1c", "libebur128 (separately written)"]},
 "6_independent_tool_check": {"pass": c6["n_ok"] == c6["n"], "files_ok": f'{c6["n_ok"]}/{c6["n"]}', "worst_abs_diff_db": c6["worst_abs_diff_db"], "tools": c6["tools"]},
}
res = {"phase": "1", "all_six_pass": all(g["pass"] for k, g in gates.items() if k[0] in "123456"), "gates": gates,
       "inputs": {f: hashlib.sha256(open(os.path.join(d, f), "rb").read()).hexdigest() for f in sorted(os.listdir(d)) if f.startswith("check")},
       "regenerate": "python hiss/python/phase1c/check1.py ... check6.py then make_results.py; needs the closed-key analysis copies, which are not in the repo"}
json.dump(res, open(os.path.join(d, "..", "RESULTS.json"), "w"), indent=1)
print(json.dumps({k: v["pass"] for k, v in gates.items()}), "all_six_pass:", res["all_six_pass"])

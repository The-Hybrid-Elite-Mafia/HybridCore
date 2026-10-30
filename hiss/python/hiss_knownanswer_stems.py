"""Known-answer test on single clean stems (the way the tool is really used: stem by stem).

usage: python hiss_knownanswer_stems.py <cli> <fingerprint_removed.wav> <outdir> <level_db> <song_dir> [<song_dir> ...]

Each clean mono stem is made stereo, the real-Suno fingerprint is added at <level_db>, the tool runs in its normal (auto) way.
Also runs every stem clean, to count false alarms. Test music is for measuring only.
"""
import glob
import json
import os
import sys

import numpy as np
import soundfile as sf

from hiss_knownanswer import band_rms_db, fingerprint, run_cli, synth


REPEAT = int(os.environ.get("KA_REPEAT", "5"))


def main():
    cli, fp_wav, outdir, level = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4])
    songs = sys.argv[5:]
    os.makedirs(outdir, exist_ok=True)
    C = fingerprint(fp_wav)
    rows = []
    for folder in songs:
        song = os.path.basename(os.path.normpath(folder))
        for f in sorted(glob.glob(os.path.join(folder, "*.wav"))):
            stem = os.path.splitext(os.path.basename(f))[0]
            x, fs = sf.read(f, always_2d=True, dtype="float64")
            x = np.tile(x.mean(axis=1), REPEAT)          # 29 s stems are too short for the tool to find enough quiet material; repeat to song length
            st = np.stack([x, np.roll(x, 7)], axis=1).astype(np.float32)     # not identical channels: a mono-doubled stem has a silent side channel, which the comb detector cannot judge
            work = os.path.join(outdir, song, stem)
            os.makedirs(work, exist_ok=True)
            cin = os.path.join(work, "clean.wav")
            sf.write(cin, st, fs, subtype="FLOAT")
            res0, fixed0 = run_cli(cli, cin, os.path.join(work, "out_clean"))
            false_alarm = bool(res0["touched"])
            A = synth(C, fs, len(st), level)
            test = (st.astype(np.float64) + A).astype(np.float32)
            tin = os.path.join(work, "with.wav")
            sf.write(tin, test, fs, subtype="FLOAT")
            res, fixed = run_cli(cli, tin, os.path.join(work, "out_with"))
            A32 = test.astype(np.float64) - st.astype(np.float64)
            resid = fixed - st.astype(np.float64)
            removed = test.astype(np.float64) - fixed
            E = removed - A32
            skip = int(1.0 * fs)
            def rms(v): return float(np.sqrt((v[skip:] ** 2).mean()))
            row = {"song": song, "stem": stem, "fs": fs, "level_db": level, "clean_false_alarm": false_alarm,
                   "clean_found": res0["found"], "found": res["found"], "touched": res["touched"], "confidence": res["confidence"],
                   "linesBefore": res["onGridBefore"], "linesAfter": res["onGridAfter"],
                   "removed_db": round(20 * np.log10(rms(A32) / (rms(resid) + 1e-30)), 2) if res["touched"] else 0.0,
                   "wrong_taken_re_added_db": round(20 * np.log10((rms(E) + 1e-30) / (rms(A32) + 1e-30)), 2) if res["touched"] else None}
            for lo, hi, tag in ((3000, 10000, "mid"), (10000, 20000, "air")):
                row["added_%s_db" % tag] = round(band_rms_db(A32[skip:], fs, lo, hi), 2)
                row["wrong_%s_db" % tag] = round(band_rms_db(E[skip:], fs, lo, hi), 2) if res["touched"] else None
            rows.append(row)
            print("%s/%s: clean false-alarm=%s | with comb: found=%s touched=%s removed=%s dB lines %s->%s" % (
                song, stem, false_alarm, row["found"], row["touched"], row["removed_db"], row["linesBefore"], row["linesAfter"]), flush=True)
            with open(os.path.join(outdir, "knownanswer_stems_%d.json" % int(-level)), "w") as fh:
                json.dump(rows, fh, indent=1)


if __name__ == "__main__":
    main()

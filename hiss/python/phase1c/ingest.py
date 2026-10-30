"""Phase 1C ingest: hash every audio asset, record what ffprobe says, tag it, and make name-stripped copies (links keyed by hash).

usage: python ingest.py <assets.json out> <analysis_dir> <key_dir>
Writes assets.json (hash, size, ffprobe facts, tag) WITHOUT the original names, plus a key file (hash -> original path) into <key_dir>.
The key file is kept closed until RESULTS.json is committed (rule from the plan).
Tags are what Steven told us or what the file itself says. Anything else is UNKNOWN.
"""
import glob
import hashlib
import json
import os
import subprocess
import sys

U = "/mnt/user-data/uploads/"
DL = U + "Downloads/"
AT = glob.glob("/mnt/user-data/attachments/*/")[0]

# group -> (date, model, source note). Model is what Steven said; UNKNOWN where he did not say.
GROUPS = {
    "june24": ("2026-06-24", "v5.5 (Steven)", "phone file manager screenshot, Jun 24 6:45 PM"),
    "twelve_stem_june": ("June 2026 (Steven, from memory, may be a cover)", "UNKNOWN", "Steven says June; date not verified"),
    "ending_swoosh": ("2026-10-28 (download day; creation date UNKNOWN)", "v6 or custom (Steven)", "downloaded Oct 28"),
    "song_4m31": ("UNKNOWN", "UNKNOWN", "stems named 111witness"),
    "no_brakes": ("2026-09-27 (zip file date on PC)", "v6 or custom (Steven: since August)", "file date, not creation date"),
    "days240": ("2026-10-23 (zip file date on PC)", "v6 or custom (Steven: since August)", "file date, not creation date"),
    "inspired_by_sound": ("2026-10-17 (folder file date)", "UNKNOWN", "file date, not creation date"),
    "theres_a_difference": ("2026-10-17 (folder file date)", "UNKNOWN", "file date, not creation date"),
    "reading_same_letter": ("2026-10-12 (zip file date)", "UNKNOWN", "file date, not creation date"),
    "room_tone_test": ("2026-10-27 (zip file date)", "v6 or custom (Steven made it for this test)", "Steven's test prompt"),
    "steady_whistle_test": ("2026-10-27 (zip file date)", "v6 or custom (Steven made it for this test)", "Steven's test prompt"),
    "circle_jerk_boys": ("2026-10-28 (zip file date)", "UNKNOWN", "stems are all zeros"),
    "osiris_flannel": ("2026-05-23 (Suno tag, Steven)", "UNKNOWN", "full mix only"),
    "death_burn_mashup": ("UNKNOWN", "UNKNOWN", "full mix"),
    "mp3_7stem": ("2026-10-28 (download day)", "UNKNOWN", "MP3 export, lossy"),
    "human_reference": ("n/a", "human-recorded", "Cambridge-MT multitracks, test use only, never ship"),
    "cosmic_abyss_mix": ("2026-09-13 (file date on PC)", "UNKNOWN", "full mix from Suno Downloads folder; file date, not creation date"),
    "unsorted": ("UNKNOWN", "UNKNOWN", "not tagged"),
}
JUNE24 = {"0_Lead_Vocals_1", "1_Drums", "2_Bass", "3_Keyboard", "4_Percussion", "5_Strings", "6_Synth", "7_Other"}
B12 = {"0_Lead_Vocals", "1_Backing_Vocals", "10_Brass", "11_Woodwinds", "2_Drums", "3_Bass", "4_Guitar", "5_Keyboard", "6_Percussion", "7_Strings", "8_Synth", "9_Other"}
W1 = {"0_Lead_Vocals_2", "1_Backing_Vocals_1", "2_Drums_1", "3_Bass_1", "4_Guitar_1", "5_Keyboard_1", "6_Percussion_1", "7_Strings_1", "8_Synth_1", "9_Other_1"}
S431 = {"10_Drums_3", "11_Backing_Vocals_3", "12_Vocals_2", "1_111witness_3", "1_Woodwinds_3", "2_111witness_2", "2_Brass_2", "3_111witness_2", "3_FX_2", "4_111witness_2", "4_Synth_2", "9_Bass_2"}


def group_of(p):
    b = os.path.splitext(os.path.basename(p))[0]
    if p.startswith("/tmp/clean/"):
        return "human_reference"
    if p.startswith(U) and not p.startswith(DL):
        if b in W1: return "ending_swoosh"
        if b in S431: return "song_4m31"
        if b in JUNE24: return "june24"
        if b in B12: return "twelve_stem_june"
    if p.startswith(DL):
        if "No Brakes" in p: return "no_brakes"
        if "Two hundred forty" in p: return "days240"
        if "Inspired by Sound" in p: return "inspired_by_sound"
        if "There's a Difference" in p: return "theres_a_difference"
        if "reading the same letter" in p: return "reading_same_letter"
        if "Suno Downloads" in p: return "cosmic_abyss_mix"
    if p.startswith("/tmp/suno_test/Room Tone"): return "room_tone_test"
    if p.startswith("/tmp/suno_test/Steady Whistle"): return "steady_whistle_test"
    if p.startswith("/tmp/suno_new/cjb/"): return "circle_jerk_boys"
    if p.startswith(AT):
        if "Osiris" in p: return "osiris_flannel"
        if "Death-Burn" in p: return "death_burn_mashup"
        if p.endswith(".mp3"): return "mp3_7stem"
    return "unsorted"


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for ch in iter(lambda: f.read(1 << 22), b""):
            h.update(ch)
    return h.hexdigest()


def probe(p):
    o = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_name,sample_rate,channels,bits_per_sample,bits_per_raw_sample,sample_fmt,bit_rate:format=duration,format_name", "-of", "json", p], capture_output=True, text=True).stdout
    j = json.loads(o)
    s = j["streams"][0]
    return {"codec": s.get("codec_name"), "rate": int(s["sample_rate"]), "channels": s["channels"], "sample_fmt": s.get("sample_fmt"),
            "bits": s.get("bits_per_raw_sample") or s.get("bits_per_sample"), "bit_rate": s.get("bit_rate"), "seconds": round(float(j["format"]["duration"]), 2)}


def main():
    out, adir, kdir = sys.argv[1:4]
    os.makedirs(adir, exist_ok=True)
    os.makedirs(kdir, exist_ok=True)
    paths = []
    paths += sorted(glob.glob(U + "*.wav"))
    paths += sorted(glob.glob(DL + "**/*.wav", recursive=True))
    paths += sorted(glob.glob(AT + "*.wav")) + sorted(glob.glob(AT + "*.mp3"))
    paths += sorted(glob.glob("/tmp/suno_test/**/*.wav", recursive=True))
    paths += sorted(glob.glob("/tmp/suno_new/cjb/*.wav"))
    paths += sorted(glob.glob("/tmp/clean/**/*.wav", recursive=True))
    seen, assets, key = set(), [], {}
    for p in paths:
        rp = os.path.realpath(p)
        if rp in seen:
            continue
        seen.add(rp)
        h = sha(rp)
        g = group_of(rp)
        info = probe(rp)
        d, m, note = GROUPS[g]
        assets.append({"sha256": h, "bytes": os.path.getsize(rp), "group": g, "date": d, "model": m, "date_note": note, **info})
        key[h] = rp
        ext = os.path.splitext(rp)[1]
        link = os.path.join(adir, h[:16] + ext)
        if not os.path.exists(link):
            os.symlink(rp, link)
        print(h[:12], g, info["rate"], info["bits"], round(info["seconds"]), flush=True)
    json.dump(assets, open(out, "w"), indent=1)
    json.dump(key, open(os.path.join(kdir, "KEY_CLOSED_UNTIL_RESULTS_COMMITTED.json"), "w"), indent=1)
    print(len(assets), "assets")


if __name__ == "__main__":
    main()

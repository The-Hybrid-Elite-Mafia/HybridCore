"""Builds HEM-156-Visual-Gallery: every rendered faceplate PNG, a small preview of each for the grid, and an
offline index.html (dark, no internet needed) with a count banner, pack filters, search, and a full-size
lightbox with next / previous.

  python build-visual-gallery.py --out HEM-156-Visual-Gallery \
      --pack "HybridCore=shots-hc=log-hc.txt" --pack "Elite Rack=shots-rack=log-rack.txt" ...

Each --pack is NAME=FOLDER=LOG. HEM WAVE is taken out of the HybridCore folder into its own pack.
The renderer logs are read for FAIL lines so a plug-in that did not render is named on the page.
Exit code 0 = all 156 rendered."""
import argparse
import base64
import io
import json
import shutil
import struct
import sys
from pathlib import Path

from PIL import Image

EXPECTED = {"HybridCore": 22, "HEM WAVE": 1, "Elite Rack": 120, "Dark Matter": 13}


def png_size(p: Path):
    with p.open("rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{p} is not a PNG")
    return struct.unpack(">II", head[16:24])


def fail_lines(log: Path):
    if not log.exists():
        return [f"renderer log {log.name} missing (the render step did not run)"]
    out = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if s.startswith("FAIL") or s.startswith("LAYOUT"):
            out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--pack", action="append", required=True)
    ap.add_argument("--note", default="")
    a = ap.parse_args()

    out = Path(a.out)
    if out.exists():
        shutil.rmtree(out)
    (out / "faceplates").mkdir(parents=True)

    cards, problems = [], []
    for spec in a.pack:
        name, folder, log = spec.split("=")
        folder, log = Path(folder), Path(log)
        problems += [f"{name}: {l}" for l in fail_lines(log)]
        pngs = sorted(p for p in folder.glob("*.png") if p.stem.startswith("HEM_") or p.stem.startswith("HEM "))
        for p in pngs:
            pack = "HEM WAVE" if (name == "HybridCore" and p.stem.replace("_", " ") == "HEM WAVE") else name
            w, h = png_size(p)
            slug = (pack.replace(" ", "-") + "__" + p.stem).replace("'", "")
            dst = out / "faceplates" / (slug + ".png")
            shutil.copyfile(p, dst)
            im = Image.open(p).convert("RGB")
            pw = 960
            buf = io.BytesIO()
            im.resize((pw, max(1, round(h * pw / w))), Image.LANCZOS).save(buf, "JPEG", quality=86)
            # the preview lives INSIDE index.html, so the page shows every faceplate even when it is opened
            # straight from the zip (Windows then copies only index.html to a temp folder)
            prev = "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode("ascii")
            cards.append({"name": p.stem.replace("_", " "), "pack": pack, "w": w, "h": h,
                          "full": f"faceplates/{dst.name}", "prev": prev,
                          "kb": round(dst.stat().st_size / 1024)})

    counts = {k: sum(1 for c in cards if c["pack"] == k) for k in EXPECTED}
    for k, want in EXPECTED.items():
        if counts[k] != want:
            problems.append(f"{k}: {counts[k]} of {want} rendered")
    total, want_total = len(cards), sum(EXPECTED.values())
    order = list(EXPECTED)
    cards.sort(key=lambda c: (order.index(c["pack"]), c["name"]))

    page = PAGE.replace("/*DATA*/", json.dumps({"cards": cards, "counts": counts, "expected": EXPECTED,
                                                 "total": total, "want": want_total, "problems": problems,
                                                 "note": a.note}))
    (out / "index.html").write_text(page, encoding="utf-8")
    print(f"gallery: {total} of {want_total} rendered; " + ", ".join(f"{k} {counts[k]}/{v}" for k, v in EXPECTED.items()))
    for p in problems:
        print("PROBLEM", p)
    sizes = sorted({(c["w"], c["h"]) for c in cards})
    print("sizes:", ", ".join(f"{w}x{h}" for w, h in sizes))
    return 0 if total == want_total and not problems else 1


PAGE = r"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>HEM 156 Visual Gallery</title>
<style>
:root{--bg:#0b0d10;--card:#14181d;--line:#262c34;--text:#e8edf2;--dim:#8a96a3;--cyan:#3fd8ff;--amber:#ffb53f;--bad:#ff5a5a;--good:#46e08a}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 system-ui,Segoe UI,Arial,sans-serif}
header{position:sticky;top:0;z-index:5;background:rgba(11,13,16,.96);border-bottom:1px solid var(--line);padding:14px 20px}
h1{margin:0 0 8px;font-size:20px;letter-spacing:.04em}
.banner{padding:10px 14px;border-radius:8px;font-weight:600;margin-bottom:10px}
.banner.ok{background:#0f2a1c;border:1px solid var(--good);color:var(--good)}
.banner.bad{background:#2c1212;border:1px solid var(--bad);color:#ffc9c9}
.banner ul{margin:6px 0 0 18px;padding:0;font-weight:400}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center}
button.f{background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:999px;padding:7px 14px;cursor:pointer;font:inherit}
button.f.on{border-color:var(--cyan);color:var(--cyan)}
input{flex:1;min-width:200px;background:var(--card);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:8px 12px;font:inherit}
.shown{color:var(--dim);margin-left:auto}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(380px,1fr));gap:16px;padding:20px}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden;cursor:zoom-in}
.card:hover{border-color:var(--cyan)}
.card img{display:block;width:100%;aspect-ratio:16/9;object-fit:contain;background:#000}
.meta{padding:10px 12px;display:flex;justify-content:space-between;gap:8px;align-items:baseline}
.nm{font-weight:700}.pk{color:var(--amber);font-size:13px}.dim{color:var(--dim);font-size:13px;white-space:nowrap}
#lb{position:fixed;inset:0;background:rgba(0,0,0,.94);display:none;z-index:10}
#lb.open{display:block}
#lbwrap{position:absolute;inset:48px 0 0 0;overflow:auto;text-align:center}
#lbimg{max-width:100%;max-height:100%;cursor:zoom-in;image-rendering:auto}
#lbimg.real{max-width:none;max-height:none;cursor:zoom-out}
#lbtop{position:absolute;top:0;left:0;right:0;height:48px;display:flex;align-items:center;gap:14px;padding:0 16px;background:#0b0d10;border-bottom:1px solid var(--line)}
#lbtop .t{font-weight:700}#lbtop .s{color:var(--dim)}
.nav{position:absolute;top:50%;transform:translateY(-50%);width:56px;height:96px;font-size:40px;background:rgba(20,24,29,.8);color:var(--text);border:1px solid var(--line);cursor:pointer;z-index:11}
.nav:hover{color:var(--cyan)}#prev{left:8px;border-radius:0 10px 10px 0}#next{right:8px;border-radius:10px 0 0 10px}
#close{margin-left:auto;background:none;border:1px solid var(--line);color:var(--text);border-radius:6px;padding:4px 12px;cursor:pointer;font:inherit}
.hint{color:var(--dim);font-size:13px}
</style></head><body>
<header>
<h1>HEM 156 VISUAL GALLERY</h1>
<div id="banner" class="banner"></div>
<div class="bar" id="filters"><input id="q" placeholder="Search plug-in name..." autocomplete="off"><span class="shown" id="shown"></span></div>
</header>
<main id="grid"></main>
<div id="lb"><div id="lbtop"><span class="t" id="lbt"></span><span class="s" id="lbs"></span><span id="lbwarn" style="display:none;color:#ffb53f">Showing the small picture: for full 3840 &times; 2160 size, right-click the zip &rarr; Extract All, then open index.html from the extracted folder.</span><span class="hint">Click picture = real size (1 pixel = 1 pixel) &middot; arrow keys = next / previous &middot; Esc = close</span><button id="close">Close</button></div>
<button class="nav" id="prev" title="Previous">&#8249;</button><button class="nav" id="next" title="Next">&#8250;</button>
<div id="lbwrap"><img id="lbimg" alt=""></div></div>
<script>
const D = /*DATA*/;
const esc = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const b = document.getElementById('banner');
const per = Object.keys(D.expected).map(k => `${esc(k)} ${D.counts[k]}/${D.expected[k]}`).join(' &middot; ');
if (D.total === D.want && D.problems.length === 0) { b.className = 'banner ok'; b.innerHTML = `ALL ${D.total}/${D.want} PLUG-INS RENDERED &mdash; ${per}`; }
else { b.className = 'banner bad'; b.innerHTML = `${D.total}/${D.want} RENDERED &mdash; ${per}<ul>` + D.problems.map(p => `<li>${esc(p)}</li>`).join('') + '</ul>'; }
if (D.note) b.innerHTML += `<div style="font-weight:400;margin-top:4px;color:var(--dim)">${esc(D.note)}</div>`;
let pack = 'All', q = '', list = [];
const f = document.getElementById('filters');
['All', 'HybridCore', 'Elite Rack', 'Dark Matter', 'HEM WAVE'].forEach((p, i) => {
  const btn = document.createElement('button'); btn.className = 'f' + (i ? '' : ' on');
  btn.textContent = p + (p === 'All' ? ` (${D.total})` : ` (${D.counts[p] || 0})`);
  btn.onclick = () => { pack = p; f.querySelectorAll('button.f').forEach(x => x.classList.remove('on')); btn.classList.add('on'); draw(); };
  f.insertBefore(btn, document.getElementById('q'));
});
document.getElementById('q').oninput = e => { q = e.target.value.trim().toLowerCase(); draw(); };
function draw() {
  list = D.cards.filter(c => (pack === 'All' || c.pack === pack) && (!q || c.name.toLowerCase().includes(q)));
  document.getElementById('grid').innerHTML = list.map((c, i) =>
    `<div class="card" data-i="${i}"><img loading="lazy" src="${esc(c.prev)}" alt="${esc(c.name)}"><div class="meta"><div><div class="nm">${esc(c.name)}</div><div class="pk">${esc(c.pack)}</div></div><div class="dim">${c.w} &times; ${c.h}</div></div></div>`).join('');
  document.getElementById('shown').textContent = `${list.length} shown`;
}
document.getElementById('grid').onclick = e => { const c = e.target.closest('.card'); if (c) open(+c.dataset.i); };
const lb = document.getElementById('lb'), img = document.getElementById('lbimg');
let cur = 0;
function open(i) {
  if (!list.length) return;
  cur = (i + list.length) % list.length; const c = list[cur];
  img.classList.remove('real'); img.src = c.prev; document.getElementById('lbwarn').style.display = 'none';
  const big = new Image();
  big.onload = () => { if (list[cur] === c) img.src = c.full; };
  big.onerror = () => { if (list[cur] === c) document.getElementById('lbwarn').style.display = 'inline'; };
  big.src = c.full;
  document.getElementById('lbt').textContent = c.name;
  document.getElementById('lbs').textContent = `${c.pack} · ${c.w} × ${c.h} · ${cur + 1} of ${list.length}`;
  lb.classList.add('open');
}
img.onclick = () => img.classList.toggle('real');
document.getElementById('prev').onclick = () => open(cur - 1);
document.getElementById('next').onclick = () => open(cur + 1);
document.getElementById('close').onclick = () => lb.classList.remove('open');
document.addEventListener('keydown', e => {
  if (!lb.classList.contains('open')) return;
  if (e.key === 'ArrowRight') open(cur + 1); else if (e.key === 'ArrowLeft') open(cur - 1); else if (e.key === 'Escape') lb.classList.remove('open');
});
draw();
</script></body></html>
"""

if __name__ == "__main__":
    sys.exit(main())

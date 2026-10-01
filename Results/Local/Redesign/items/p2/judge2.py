# P2 judge 2: modes side by side. For each scene_res given (e.g. lounge_1920x1080) and the modes that exist:
#   - final image: exposure-normalised 8x8 tile error against the mode's own f299 (as judge.py / the coordinator's judge2)
#   - GI layer: absolute (the layer is E x exposure; x 2^EV of the frame from the log) tile error and level against f299
#   - grids for the eye: rows = modes, columns = frames (final and GI layer), one gain per grid (the first mode's f299)
# usage: python judge2.py lounge_1920x1080 [mode ...]
import numpy as np, glob, os, re, sys
from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))
FR = (1, 3, 4, 15, 16, 299)


def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, -1)).astype(np.float64)[..., :3]


def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def gmean(a): return np.exp(np.log(lum(a) + 1e-6).mean())


def tiles(a, t=8):
    h, w = a.shape[0] // t * t, a.shape[1] // t * t
    return a[:h, :w].reshape(h // t, t, w // t, t).mean(axis=(1, 3))


def evs(log, layer):
    t = open(log, 'rb').read()
    try: s = t.decode('utf-16')
    except Exception: s = t.decode('utf-8', 'replace')
    s = s.replace('\r', '').replace('\n', '')
    return {int(m.group(1)): float(m.group(2)) for m in re.finditer(r'captured ' + layer + r' \d+x\d+ \(internal layer, frame (\d+), ev100 ([\d.]+)\)', s)}


def tone(a, gain):
    x = a * gain
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))


def err(a, ref):
    rt = tiles(lum(ref)); m = rt > 0.02 * rt.mean()
    e = np.abs(tiles(lum(a)) / np.maximum(rt, 1e-9) - 1)[m]
    return np.percentile(e, 50), np.percentile(e, 95)


def grid(rows, path, cell=(640, 360), crop=None):
    n = max(len(r) for r in rows)
    s = Image.new('RGB', (cell[0] * n, cell[1] * len(rows)))
    for j, r in enumerate(rows):
        for i, (label, im) in enumerate(r):
            if crop: im = im.crop(crop)
            im = im.resize(cell, Image.LANCZOS if not crop else Image.NEAREST)
            ImageDraw.Draw(im).text((6, 4), label, fill=(255, 255, 0))
            s.paste(im, (cell[0] * i, cell[1] * j))
    s.save(path)


scene = sys.argv[1]
modes = sys.argv[2:] or sorted(set(os.path.basename(p)[len('still_' + scene + '_'):-len('_f299.pfm')] for p in glob.glob(os.path.join(here, 'still_%s_*_f299.pfm' % scene))
                                   if '_gi_' not in p and '_refl_' not in p))
modes = [m for m in modes if os.path.exists(os.path.join(here, 'still_%s_%s_f299.pfm' % (scene, m)))]
print(scene, modes)
finals, gis = [], []
gainF = gainG = None
for m in modes:
    stem = os.path.join(here, 'still_%s_%s' % (scene, m))
    ref = rd(stem + '_f299.pfm')
    out = []
    row = []
    if gainF is None: gainF = 0.18 / gmean(ref)
    for f in FR:
        p = '%s_f%d.pfm' % (stem, f)
        if not os.path.exists(p): continue
        a = rd(p); a = a * (gmean(ref) / gmean(a))
        if f != 299: out.append('f%d %4.0f/%4.0f' % ((f,) + tuple(100 * x for x in err(a, ref))))
        if f in (1, 4, 16, 299): row.append(('%s f%d' % (m, f), tone(a, gainF)))
    finals.append(row)
    print('  final %-14s (P50/P95 %%) %s' % (m, '  '.join(out)))
    if os.path.exists(stem + '_gi_f299.pfm'):
        ev = evs(stem + '.log', 'gi')
        refg = rd(stem + '_gi_f299.pfm') * 2.0 ** ev[299]
        if gainG is None: gainG = 0.25 / gmean(refg)
        out = []; row = []
        for f in FR:
            p = '%s_gi_f%d.pfm' % (stem, f)
            if not os.path.exists(p): continue
            a = rd(p) * 2.0 ** ev[f]
            if f != 299: out.append('f%d %.2f %3.0f/%3.0f' % ((f, lum(a).mean() / lum(refg).mean()) + tuple(100 * x for x in err(a, refg))))
            if f in (1, 4, 16, 299): row.append(('%s gi f%d' % (m, f), tone(a, gainG)))
        gis.append(row)
        print('  gi    %-14s (level P50/P95) %s' % (m, '  '.join(out)))
if finals: grid(finals, os.path.join(here, 'grid_%s_final.png' % scene))
if gis:
    grid(gis, os.path.join(here, 'grid_%s_gi.png' % scene))
    # the f299 GI layers at full resolution, centre crop (converged blotches)
    w, h = gis[0][-1][1].size
    c = (w // 4, h // 4, w // 4 + 640, h // 4 + 360)
    grid([[r[0], r[-1]] for r in gis], os.path.join(here, 'crop_%s_gi_f1_f299.png' % scene), crop=c)
# rotation (1080p only): each frame at its own exposure
rows = []
for m in modes:
    stem = os.path.join(here, 'rot_%s_%s' % (scene.split('_')[0], m))
    row = []
    for f in (59, 63, 75, 120, 179):
        p = '%s_f%d.pfm' % (stem, f)
        if os.path.exists(p):
            a = rd(p); row.append(('%s rot f%d' % (m, f), tone(a, 0.18 / gmean(a))))
    if row: rows.append(row)
if rows and scene.endswith('1920x1080'): grid(rows, os.path.join(here, 'grid_%s_rot.png' % scene))

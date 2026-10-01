# Blotch measure of the GI layer: the band between a 4 px and a 48 px box blur of log luminance (cell-sized structure:
# cells are 9-18 px at 1080p), RMS over the image, per mode and frame. Texture and geometry are in every mode alike, so
# the measure is compared between modes (and the mode / none ratio image's band RMS removes them).
# usage: python blotch.py lounge_1920x1080 [mode ...]
import numpy as np, glob, os, sys

here = os.path.dirname(os.path.abspath(__file__))


def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, -1)).astype(np.float64)[..., :3]


def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722


def box(a, r):
    k = 2 * r + 1
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def band(l):
    g = np.log(np.maximum(l, 1e-6))
    return box(g, 2) - box(g, 24)


scene = sys.argv[1]
modes = sys.argv[2:] or sorted(set(os.path.basename(p)[len('still_' + scene + '_'):-len('_gi_f299.pfm')] for p in glob.glob(os.path.join(here, 'still_%s_*_gi_f299.pfm' % scene))))
base = {}
for f in (1, 4, 16, 299):
    p = os.path.join(here, 'still_%s_none_gi_f%d.pfm' % (scene, f))
    if os.path.exists(p): base[f] = lum(rd(p))
for m in modes:
    out = []
    for f in (1, 4, 16, 299):
        p = os.path.join(here, 'still_%s_%s_gi_f%d.pfm' % (scene, m, f))
        if not os.path.exists(p): continue
        l = lum(rd(p))
        mask = l > 0.05 * np.median(l)
        b = band(l)
        s = 'f%d band %.3f' % (f, np.sqrt((b[mask] ** 2).mean()))
        if f in base and m != 'none':
            r = band(l / np.maximum(base[f], 1e-6) * (base[f].mean() / l.mean()))
            mk = mask & (base[f] > 0.05 * np.median(base[f]))
            s += ' (vs none %.3f)' % np.sqrt((r[mk] ** 2).mean())
        out.append(s)
    print('%-16s %s' % (m, ' | '.join(out)))

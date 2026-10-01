"""GI layer level and cell pattern of judge captures (verdict item 1: gi.lumen reading the surface cache).
    python Results/Local/Refl/gi_level.py --tags G0,G1,G2 [--case bath_lobby] [--mode still] [--png out.png]
Per tag and captured frame: the GI layer's mean (rgb mean and per channel), the final image's mean, and the layer's
relative deviation from its own 33 x 33 box mean over the pixels (a measure of patches: per-pixel |x - blur| / blur, mean).
--png writes the GI layers of the last frame side by side (tone-mapped x / (1 + x) after a common scale)."""
import argparse, glob, os, re
import numpy as np

def pfm(p):
    with open(p, 'rb') as f:
        h = f.readline().strip()
        w, hh = map(int, f.readline().split())
        s = float(f.readline())
        c = 3 if h == b'PF' else 1
        a = np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4')
    return a.reshape(hh, w, c)[::-1]

def box(a, r):
    k = 2 * r + 1
    p = np.pad(a, ((r, r), (r, r), (0, 0)), mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0), (0, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)

ap = argparse.ArgumentParser()
ap.add_argument('--tags', required=True)
ap.add_argument('--case', default='bath_lobby')
ap.add_argument('--mode', default='still')
ap.add_argument('--res', default='1080')
ap.add_argument('--png')
a = ap.parse_args()
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', 'Cache', 'ReflJudge')
last = []
for tag in a.tags.split(','):
    base = os.path.join(root, tag, '%s_%s_%s' % (a.case, a.mode, a.res))
    frames = sorted(int(re.search(r'_gi_f(\d+)\.pfm$', p).group(1)) for p in glob.glob(base + '_gi_f*.pfm') if not p.endswith('_alpha.pfm'))
    for f in frames:
        gi = pfm('%s_gi_f%d.pfm' % (base, f)).astype(np.float64)
        fin = pfm('%s_f%d.pfm' % (base, f)).astype(np.float64)
        lum = gi.mean(axis=2, keepdims=True)
        blur = box(lum, 16)
        patch = float(np.mean(np.abs(lum - blur) / np.maximum(blur, 1e-6)))
        print('%-4s f%-3d gi mean %.4f (r %.4f g %.4f b %.4f, b/r %.2f) patch %.3f | final mean %.4f nan %d' % (
            tag, f, gi.mean(), gi[..., 0].mean(), gi[..., 1].mean(), gi[..., 2].mean(), gi[..., 2].mean() / max(gi[..., 0].mean(), 1e-9), patch, fin.mean(),
            int(np.isnan(gi).sum() + np.isnan(fin).sum())))
    if frames:
        last.append((tag, pfm('%s_gi_f%d.pfm' % (base, frames[-1])), pfm('%s_f%d.pfm' % (base, frames[-1]))))
if a.png and last:
    from PIL import Image
    scale = 1.0 / max(np.percentile(last[0][1], 99), 1e-6)
    rows = []
    for tag, gi, fin in last:
        g = np.clip(gi * scale, 0, None)
        g = (g / (1 + g) * 2) ** (1 / 2.2)
        f = np.clip(fin, 0, 1) ** (1 / 2.2)
        if f.shape != g.shape:
            f = np.asarray(Image.fromarray((f * 255).astype(np.uint8)).resize((g.shape[1], g.shape[0]))) / 255.0
        rows.append(np.concatenate([np.clip(g, 0, 1), f], axis=1))
    img = np.concatenate(rows, axis=0)
    Image.fromarray((img * 255).astype(np.uint8)).resize((img.shape[1] // 2, img.shape[0] // 2)).save(a.png)
    print('wrote', a.png)

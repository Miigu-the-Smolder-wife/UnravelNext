# Layer view of a still run: for each mode a row of the final image, the GI layer and the reflection layer at one frame,
# each at its own exposure (gain to 0.18 geometric mean of the first mode's image of that layer). To tell which layer
# the residual dots of the final image come from.
# usage: python layers.py lobby_1920x1080 299 alloff allon
import numpy as np, os, sys
from PIL import Image, ImageDraw

here = os.path.dirname(os.path.abspath(__file__))


def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, -1)).astype(np.float64)[..., :3]


def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722


def tone(a, gain):
    x = np.maximum(a, 0) * gain
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))


scene, frame, modes = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
cell = (960, 540)
sheet = Image.new('RGB', (cell[0] * 3, cell[1] * len(modes)))
gains = {}
for j, m in enumerate(modes):
    for i, layer in enumerate(('final', 'gi', 'refl')):
        p = os.path.join(here, 'still_%s_%s%s_f%d.pfm' % (scene, m, '' if layer == 'final' else '_' + layer, frame))
        if not os.path.exists(p): continue
        a = rd(p)
        a = np.nan_to_num(a, nan=0.0, posinf=0.0, neginf=0.0)
        l = lum(a)
        if layer not in gains: gains[layer] = 0.18 / max(np.exp(np.log(l[l > 0] + 1e-9).mean()) if (l > 0).any() else 1.0, 1e-9)
        im = tone(a, gains[layer])
        im.save(os.path.join(here, 'layer_%s_%s_%s_f%d.png' % (scene, m, layer, frame)))
        # statistics of the layer: mean, and the share of the energy carried by 8x8 tiles above 4x their 5x5 neighbourhood median
        h, w = l.shape[0] // 8 * 8, l.shape[1] // 8 * 8
        t = l[:h, :w].reshape(h // 8, 8, w // 8, 8).mean(axis=(1, 3))
        pad = np.pad(t, 2, mode='edge')
        nb = np.stack([pad[y:y + t.shape[0], x:x + t.shape[1]] for y in range(5) for x in range(5)], 0)
        med = np.median(nb, 0)
        hot = t > 2 * np.maximum(med, 1e-9)
        print('%-8s %-6s f%d mean %.4g  tiles above 2x the 5x5 median: %.2f %% of tiles, %.1f %% of the energy' % (m, layer, frame, l.mean(), 100 * hot.mean(), 100 * t[hot].sum() / max(t.sum(), 1e-12)))
        im = im.resize(cell, Image.LANCZOS)
        ImageDraw.Draw(im).text((6, 4), '%s %s f%d' % (m, layer, frame), fill=(255, 255, 0))
        sheet.paste(im, (cell[0] * i, cell[1] * j))
sheet.save(os.path.join(here, 'layers_%s_f%d.png' % (scene, frame)))

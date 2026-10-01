# P2 judge: exposure-normalised tile error of the still frames against f299 (as the coordinator's judge2: each frame scaled
# to f299's geometric-mean brightness), and strips for the eye (still f1/f4/f16/f299, rotation f59/f63/f75/f120/f179).
import numpy as np, glob, os, sys
from PIL import Image, ImageDraw

def rd(p):
    with open(p, 'rb') as f:
        k = f.readline().strip(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, 3)).astype(np.float64)

def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722

def gmean(a): return np.exp(np.log(lum(a) + 1e-6).mean())

def tiles(a, t=8):
    h, w = a.shape[0] // t * t, a.shape[1] // t * t
    return a[:h, :w].reshape(h // t, t, w // t, t).mean(axis=(1, 3))

def png(a, gain):
    x = a * gain
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))

here = os.path.dirname(os.path.abspath(__file__))
rows = []
def finals():
    return [b for b in sorted(glob.glob(os.path.join(here, 'still_*_f299.pfm'))) if not (b.endswith('_gi_f299.pfm') or b.endswith('_refl_f299.pfm'))]

for base in finals():
    stem = base[:-len('_f299.pfm')]
    name = os.path.basename(stem)[len('still_'):]
    ref = rd(base)
    g = gmean(ref)
    rt = tiles(lum(ref))
    out = []
    for f in (1, 3, 4, 15, 16):
        p = '%s_f%d.pfm' % (stem, f)
        if not os.path.exists(p): continue
        a = rd(p); a = a * (g / gmean(a))
        e = np.abs(tiles(lum(a)) / np.maximum(rt, 1e-6) - 1)
        m = rt > 0.02 * rt.mean()
        out.append('f%d %5.1f%%' % (f, 100 * np.percentile(e[m], 95)))
    rows.append('%-28s %s' % (name, '  '.join(out)))
print('\n'.join(rows))

# strips for the eye: one gain per strip (f299's)
def strip(files, path):
    imgs = []
    ref = rd(files[-1]); gain = 0.18 / max(gmean(ref), 1e-6)
    for p in files:
        a = rd(p); a = a * (gmean(ref) / gmean(a))
        im = png(a, gain).resize((640, 360))
        ImageDraw.Draw(im).text((6, 4), os.path.basename(p)[:-4][-28:], fill=(255, 255, 0))
        imgs.append(im)
    s = Image.new('RGB', (640 * len(imgs), 360))
    for i, im in enumerate(imgs): s.paste(im, (640 * i, 0))
    s.save(path)

for base in finals():
    stem = base[:-len('_f299.pfm')]
    strip(['%s_f%d.pfm' % (stem, f) for f in (1, 4, 16, 299)], stem + '_strip.png')
for base in sorted(glob.glob(os.path.join(here, 'rot_*_f179.pfm'))):
    stem = base[:-len('_f179.pfm')]
    files = ['%s_f%d.pfm' % (stem, f) for f in (59, 63, 75, 120, 179)]
    files = [f for f in files if os.path.exists(f)]
    # rotation: each frame at its own exposure (no normalisation to a single reference frame)
    imgs = []
    for p in files:
        a = rd(p); im = png(a, 0.18 / max(gmean(a), 1e-6)).resize((640, 360))
        ImageDraw.Draw(im).text((6, 4), os.path.basename(p)[:-4][-28:], fill=(255, 255, 0))
        imgs.append(im)
    s = Image.new('RGB', (640 * len(imgs), 360))
    for i, im in enumerate(imgs): s.paste(im, (640 * i, 0))
    s.save(stem + '_strip.png')

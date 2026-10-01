# shading.mega_lights against the default path (run_lobby_ml.ps1 captures): for each captured frame an "off | ml" image
# (the same gain: the off set's f299 auto exposure) and numbers:
#   noise    per-pixel high-frequency residue: |L - 5x5 box mean of L| / (box mean + eps), P50 / P95 over surface pixels
#            (a smooth, clean image is near 0; texture detail also counts, so read it as ml against off on the same frame);
#   level    mean luminance (ml / off: the default path lights 699 of the lobby's shadowed lights without shadows, so
#            ml is expected darker where those lights are blocked);
#   settle   mean |L(frame) - L(f299)| / mean L(f299) for the still captures: how far each early frame is from the
#            converged one.
#   python compare_ml.py [scene_tag e.g. lobby_1080] [mode still|motion] [x0,y0,x1,y1 crops ...]
import numpy as np, os, sys, glob, re
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
stem = sys.argv[1] if len(sys.argv) > 1 else 'lobby_1080'
mode = sys.argv[2] if len(sys.argv) > 2 else 'still'
boxes = [tuple(map(int, b.split(','))) for b in sys.argv[3:]]
sets = ['off', 'ml']

def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, 3)).astype(np.float64)
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def png(a, gain):
    x = np.clip(a * gain, 0, None)
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))
def box5(l):
    p = np.pad(l, 2, mode='edge')
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[5:, 5:] - c[:-5, 5:] - c[5:, :-5] + c[:-5, :-5]) / 25.0
def noise(a):
    l = lum(a); m = box5(l)
    r = np.abs(l - m) / (m + 1e-3 * l.mean() + 1e-9)
    return np.percentile(r, 50), np.percentile(r, 95)

frames = {}
for s in sets:
    for p in glob.glob(os.path.join(here, f'{stem}_{mode}_{s}_f*.pfm')):
        f = int(re.search(r'_f(\d+)\.pfm$', p).group(1))
        frames.setdefault(f, {})[s] = p
if not frames:
    print('no captures for', stem, mode); sys.exit(1)
last = max(frames)
ref = rd(frames[last]['off']) if 'off' in frames[last] else rd(next(iter(frames[last].values())))
gain = 0.18 / np.exp(np.log(lum(ref) + 1e-6).mean())
conv = {s: rd(frames[last][s]) for s in frames[last]}
lines = []
for f in sorted(frames):
    imgs = {s: rd(p) for s, p in frames[f].items()}
    row = [png(imgs[s], gain) for s in sets if s in imgs]
    strip = Image.new('RGB', (sum(i.width for i in row) + 8 * (len(row) - 1), row[0].height), (255, 0, 255))
    x = 0
    for i in row:
        strip.paste(i, (x, 0)); x += i.width + 8
    strip.resize((strip.width // 2, strip.height // 2), Image.LANCZOS).save(os.path.join(here, f'{stem}_{mode}_f{f}_off_ml.png'))
    for bi, (x0, y0, x1, y1) in enumerate(boxes):
        crops = [png(imgs[s][y0:y1, x0:x1], gain).resize(((x1 - x0) * 2, (y1 - y0) * 2), Image.NEAREST) for s in sets if s in imgs]
        cs = Image.new('RGB', (sum(c.width for c in crops) + 8 * (len(crops) - 1), crops[0].height), (255, 0, 255))
        x = 0
        for c in crops:
            cs.paste(c, (x, 0)); x += c.width + 8
        cs.save(os.path.join(here, f'{stem}_{mode}_f{f}_crop{bi}.png'))
    parts = []
    for s in sets:
        if s not in imgs: continue
        n50, n95 = noise(imgs[s])
        settle = np.abs(lum(imgs[s]) - lum(conv[s])).mean() / lum(conv[s]).mean() if mode == 'still' and s in conv else float('nan')
        parts.append(f'{s}: level {lum(imgs[s]).mean():.4f} noise P50 {n50:.4f} P95 {n95:.4f} settle {settle:.3f}')
    if 'off' in imgs and 'ml' in imgs:
        parts.append(f'ml/off level {lum(imgs["ml"]).mean() / lum(imgs["off"]).mean():.3f}')
    lines.append(f'{stem} {mode} f{f:<3d} ' + ' | '.join(parts))
open(os.path.join(here, f'compare_ml_{stem}_{mode}.txt'), 'w').write('\n'.join(lines) + '\n')
print('\n'.join(lines))

# Lobby defect attribution (coordinator 2026-10-01 19:35): the lobby's converged frame (f299) with one term left out at a
# time (run_lobby_diag.ps1: off = baseline, noslot = shadow-casting lights without a VSM slot add nothing, noair = no air
# volume lookup, nolocal = no local lights, noshadow = every slot lit). Writes a PNG per set (same gain: the baseline's
# auto exposure) and, for the crop boxes given on the command line (x0,y0,x1,y1 ...), strips of the crops side by side
# plus each crop's "blockiness": the mean absolute luminance step across the 8 px tile grid lines over the mean step
# across the other pixel boundaries (1 = no tile pattern).
#   python compare_lobby.py [frame] [x0,y0,x1,y1 ...]
import numpy as np, os, sys
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
sets = ['off', 'noslot', 'noair', 'nolocal', 'nogi', 'norefl', 'noshadow']

def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, 3)).astype(np.float64)
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def png(a, gain):
    x = np.clip(a * gain, 0, None)
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))
def blockiness(l, t=8):
    out = []
    for axis in (0, 1):
        d = np.abs(np.diff(l, axis=axis))
        idx = np.arange(d.shape[axis])
        on = (idx % t) == t - 1
        a = d.take(np.nonzero(on)[0], axis=axis).mean()
        b = d.take(np.nonzero(~on)[0], axis=axis).mean()
        out.append(a / max(b, 1e-12))
    return out

frame = sys.argv[1] if len(sys.argv) > 1 else '299'
boxes = [tuple(map(int, b.split(','))) for b in sys.argv[2:]]
imgs = {}
for s in sets:
    p = os.path.join(here, f'lobby_1080_{s}_f{frame}.pfm')
    if os.path.exists(p): imgs[s] = rd(p)
ref = imgs['off'] if 'off' in imgs else next(iter(imgs.values()))
gain = 0.18 / np.exp(np.log(lum(ref) + 1e-6).mean())
lines = []
for s, a in imgs.items():
    png(a, gain).save(os.path.join(here, f'lobby_{s}_f{frame}.png'))
    bx, by = blockiness(lum(a))
    lines.append(f'{s:9s} f{frame}: mean luminance {lum(a).mean():.4f} (x{lum(a).mean() / lum(ref).mean():.3f} of off)  tile-step ratio rows {bx:.3f} cols {by:.3f}')
for i, (x0, y0, x1, y1) in enumerate(boxes):
    crops = [png(a[y0:y1, x0:x1], gain).resize(((x1 - x0) * 3, (y1 - y0) * 3), Image.NEAREST) for a in imgs.values()]
    strip = Image.new('RGB', (sum(c.width for c in crops) + 6 * (len(crops) - 1), crops[0].height), (255, 0, 255))
    x = 0
    for c in crops:
        strip.paste(c, (x, 0)); x += c.width + 6
    strip.save(os.path.join(here, f'lobby_crop{i}_f{frame}.png'))
    for s, a in imgs.items():
        l = lum(a[y0:y1, x0:x1])
        bx, by = blockiness(l)
        lines.append(f'crop{i} [{x0},{y0},{x1},{y1}] {s:9s}: mean {l.mean():.4f} P99/P50 {np.percentile(l, 99) / max(np.percentile(l, 50), 1e-9):.2f}  tile-step ratio rows {bx:.3f} cols {by:.3f}')
open(os.path.join(here, f'compare_lobby_f{frame}.txt'), 'w').write('\n'.join(lines) + '\n')
print('\n'.join(lines))

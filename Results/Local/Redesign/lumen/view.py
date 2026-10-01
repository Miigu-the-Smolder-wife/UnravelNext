# python view.py <tag_scene> : final and GI layer at the captured frames, one sheet (each 640 x 360, own exposure)
import numpy as np, os, sys, glob, re
from PIL import Image, ImageDraw
here = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, -1)).astype(np.float64)[..., :3]
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def tone(a):
    a = np.nan_to_num(np.maximum(a, 0), nan=0.0, posinf=0.0)
    l = lum(a); g = 0.18 / max(np.exp(np.log(l[l > 0] + 1e-9).mean()) if (l > 0).any() else 1.0, 1e-9)
    x = a * g
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))
stem = sys.argv[1]
frames = sorted(int(m.group(1)) for m in (re.search(r'_f(\d+)\.pfm$', p) for p in glob.glob(os.path.join(here, stem + '_f*.pfm'))) if m)
sheet = Image.new('RGB', (640 * len(frames), 720))
for i, f in enumerate(frames):
    for j, layer in enumerate(('', '_gi')):
        p = os.path.join(here, '%s%s_f%d.pfm' % (stem, layer, f))
        if not os.path.exists(p): continue
        a = rd(p)
        print('%s%s f%d mean %.4g max %.4g nan %d' % (stem, layer, f, np.nanmean(lum(a)), np.nanmax(lum(a)), int(np.isnan(a).sum())))
        im = tone(a).resize((640, 360), Image.LANCZOS)
        ImageDraw.Draw(im).text((6, 4), '%s%s f%d' % (stem, layer, f), fill=(255, 255, 0))
        sheet.paste(im, (640 * i, 360 * j))
sheet.save(os.path.join(here, stem + '.png'))

# Side by side at one frame: python cmp.py lobby_1920x1080 16 allon allon_noacc ... -> cmp_<scene>_f<frame>.png (each 960x540, own exposure)
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
cols = 2 if len(modes) > 3 else len(modes)
rows = (len(modes) + cols - 1) // cols
sheet = Image.new('RGB', (960 * cols, 540 * rows))
for i, m in enumerate(modes):
    a = rd(os.path.join(here, 'still_%s_%s_f%d.pfm' % (scene, m, frame)))
    im = tone(a, 0.18 / np.exp(np.log(lum(a) + 1e-6).mean())).resize((960, 540), Image.LANCZOS)
    ImageDraw.Draw(im).text((6, 4), '%s f%d' % (m, frame), fill=(255, 255, 0))
    sheet.paste(im, (960 * (i % cols), 540 * (i // cols)))
sheet.save(os.path.join(here, 'cmp_%s_f%d.png' % (scene, frame)))

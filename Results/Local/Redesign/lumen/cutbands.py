# python cutbands.py <stem> <ref frame> <frame> [<frame> ...] : the GI layer of the captured frames against the run's
# converged frame, split by spatial scale. d = log2(L_frame / L_ref) per pixel (absolute levels: layer x 2^EV), then
#   level = mean d (the whole frame brighter or darker), low = structure wider than 64 px (large blotches),
#   mid = 8..64 px (probe-scale blotches: the probes are 16 px apart), high = finer than 8 px (per-pixel grain).
# Each band as the RMS over the surface pixels, in percent (2^rms - 1). Also writes <stem>_cut.png: the frames at the
# reference's display key (top) and the mid + low band as an image (bottom, grey = equal, +-1 stop to white / black).
import numpy as np, os, re, sys
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return np.flipud(d.reshape(h, w, -1)).astype(np.float64)[..., :3]
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def blur(a, sigma):
    h, w = a.shape
    fy = np.fft.fftfreq(h)[:, None]; fx = np.fft.rfftfreq(w)[None, :]
    g = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.fft.irfft2(np.fft.rfft2(a) * g, s=a.shape)
def masked_blur(a, m, sigma):
    return blur(a * m, sigma) / np.maximum(blur(m, sigma), 1e-6)
def tone(a, g):
    x = np.maximum(np.nan_to_num(a), 0) * g
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    return Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8))
stem, ref = sys.argv[1], int(sys.argv[2]); frames = [int(v) for v in sys.argv[3:]]
t = open(os.path.join(here, stem + '.log'), 'rb').read()
try: s = t.decode('utf-16')
except Exception: s = t.decode('utf-8', 'replace')
s = s.replace('\r', '').replace('\n', '')
ev = {int(m.group(1)): float(m.group(2)) for m in re.finditer(r'captured gi \d+x\d+ \(internal layer, frame (\d+), ev100 ([\d.]+)\)', s)}
R = rd(os.path.join(here, '%s_gi_f%d.pfm' % (stem, ref))) * 2.0 ** ev[ref]
lr = lum(R); m = (lr > 0).astype(np.float64)
key = 0.18 / np.exp(np.log(lr[lr > 0]).mean())
pct = lambda v: (2.0 ** v - 1) * 100
sheet = Image.new('RGB', (640 * (len(frames) + 1), 720))
for i, f in enumerate(frames + [ref]):
    A = rd(os.path.join(here, '%s_gi_f%d.pfm' % (stem, f))) * 2.0 ** ev[f]
    la = lum(A); mm = m * (la > 0)
    d = np.where(mm > 0, np.log2(np.maximum(la, 1e-12) / np.maximum(lr, 1e-12)), 0.0)
    level = (d * mm).sum() / mm.sum()
    d0 = (d - level) * mm
    b8, b64 = masked_blur(d0, mm, 8 / 2.355), masked_blur(d0, mm, 64 / 2.355)
    rms = lambda v: float(np.sqrt((v * v * mm).sum() / mm.sum()))
    hi, mid, low = rms(d0 - b8), rms(b8 - b64), rms(b64)
    print('%s f%d: level %+.0f %%, low (>64 px) %.0f %%, mid (8-64 px) %.0f %%, high (<8 px) %.0f %%' % (stem, f, pct(level), pct(low), pct(mid), pct(hi)))
    sheet.paste(tone(A, key).resize((640, 360), Image.LANCZOS), (640 * i, 0))
    band = np.clip(0.5 + 0.5 * b8, 0, 1) * mm
    sheet.paste(Image.fromarray((band * 255).astype(np.uint8)).convert('RGB').resize((640, 360), Image.LANCZOS), (640 * i, 360))
sheet.save(os.path.join(here, stem + '_cut.png'))

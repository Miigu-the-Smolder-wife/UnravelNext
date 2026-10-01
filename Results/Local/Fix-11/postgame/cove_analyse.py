# The lobby cove band under the end-bias sweep (run_lobby_cove.ps1): where the old path and mega_lights (5 cm) differ
# most, and how much of that difference each end bias brings back. The captures are radiance x the run's own auto
# exposure; the gate logs the captured frame's ev100, so every image is brought back to nits first.
import os
import re

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
BOM16 = bytes([0xFF, 0xFE])


def pfm(name):
    f = open(os.path.join(here, name), 'rb')
    f.readline()
    w, h = map(int, f.readline().split())
    s = float(f.readline())
    return np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4').reshape(h, w, 3)


def lum(a):
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def ev100(name):
    raw = open(os.path.join(here, name), 'rb').read()
    text = raw.decode('utf-16') if raw[:2] == BOM16 else raw.decode('utf-8', 'replace')
    text = ''.join(text.splitlines())  # (PowerShell wraps the gate's lines)
    m = re.findall(r'frame 59, ev100 ([-0-9.]+)', text)
    return float(m[-1])


names = ['off', 'b005', 'b010', 'b020', 'b040', 'b080', 'b250']
img, evs = {}, {}
for n in names:
    if os.path.exists(os.path.join(here, 'cove_%s_f59.pfm' % n)):
        evs[n] = ev100('cove_%s.log' % n)
        img[n] = lum(pfm('cove_%s_f59.pfm' % n)) * (1.2 * 2.0 ** evs[n])  # nits: exposure = 1 / (1.2 x 2^ev100)
print('ev100 of the captured frame:', evs)
off, on = img['off'], img['b005']
h, w = off.shape
print('image %d x %d; whole-image mean luminance (nits):' % (w, h), {n: round(float(v.mean()), 3) for n, v in img.items()})
# the band: pixels where the old path is brighter than mega_lights by more than 25 % of the old value (and not dark)
band = (off - on > 0.25 * np.maximum(off, 1e-6)) & (off > np.percentile(off, 50))
rows = np.where(band.any(1))[0]
print('band pixels: %d (%.2f %% of the image), rows %d..%d' % (band.sum(), 100.0 * band.mean(), rows.min() if rows.size else -1, rows.max() if rows.size else -1))
if band.any():
    base, top = float(on[band].mean()), float(off[band].mean())
    print('band mean luminance: old path %.3f nits, mega_lights 5 cm %.3f nits (%.1f %% of the old)' % (top, base, 100 * base / top))
    for n in names:
        if n in img:
            v = float(img[n][band].mean())
            print('  %-5s band mean %.3f = %5.1f %% of the old path, restored %5.1f %% of the difference' % (n, v, 100 * v / top, 100 * (v - base) / max(top - base, 1e-9)))
# the rest of the image: how much a larger bias changes what the 5 cm rays shadow (light leaking past near geometry)
rest = ~band
for n in names:
    if n in img and n != 'off':
        d = img[n][rest] - on[rest]
        print('  %-5s rest of the image: mean change against 5 cm %+.3f nits (%.2f %%), pixels brighter by > 10 %%: %.2f %%' % (
            n, float(d.mean()), 100 * float(d.mean()) / float(on[rest].mean()), 100.0 * float((d > 0.1 * np.maximum(on[rest], 1e-6)).mean())))
try:
    from PIL import Image

    def save(name, a):
        t = np.clip(a / (np.percentile(off, 99.5) + 1e-9), 0, 1) ** (1 / 2.2)
        Image.fromarray((t * 255).astype(np.uint8)).save(os.path.join(here, name))

    row = np.concatenate([img[n][::2, ::2] for n in ('off', 'b005', 'b040', 'b250') if n in img], axis=1)
    save('cove_off_b005_b040_b250.png', row)
    Image.fromarray((band * 255).astype(np.uint8)).save(os.path.join(here, 'cove_band_mask.png'))
    print('wrote cove_off_b005_b040_b250.png, cove_band_mask.png')
except Exception as e:  # noqa: BLE001
    print('no images written:', e)

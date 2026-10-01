# The first frames after a cut, mega_lights off and on (run_ml_cut.ps1): per scene and set, frames 1, 4 and 16 against
# the run's own frame 299. Captures are radiance x the frame's auto exposure; the gate logs each captured frame's ev100,
# so every frame is brought back to nits first.
#   error   relative RMS difference of luminance from frame 299 (over pixels brighter than 1 % of the image mean)
#   grain   RMS of (luminance - its 5 x 5 mean) / (5 x 5 mean + 1 % of the image mean): pixel-scale noise, whatever the
#           converged image is (frame 299's own grain is the scene's texture detail)
# Writes cut_<scene>_grid.png: rows off / ml, columns f1, f4, f16, f299, two crops.
import os
import re

import numpy as np

here = os.path.dirname(os.path.abspath(__file__))
BOM16 = bytes([0xFF, 0xFE])
FRAMES = (1, 4, 16, 299)


def pfm(name):
    f = open(os.path.join(here, name), 'rb')
    f.readline()
    w, h = map(int, f.readline().split())
    s = float(f.readline())
    return np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4').reshape(h, w, 3)[::-1]


def evs(name):
    raw = open(os.path.join(here, name), 'rb').read()
    text = raw.decode('utf-16') if raw[:2] == BOM16 else raw.decode('utf-8', 'replace')
    text = ''.join(text.splitlines())
    return {int(f): float(e) for f, e in re.findall(r'frame (\d+), ev100 ([-0-9.]+)', text)}


def lum(a):
    return a @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def box5(a):
    c = np.cumsum(np.cumsum(np.pad(a, ((3, 2), (3, 2)), mode='edge'), 0), 1)
    return (c[5:, 5:] - c[:-5, 5:] - c[5:, :-5] + c[:-5, :-5]) / 25.0


def tone(a, ref):
    return (np.clip(a / ref, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)


for scene in ('lobby', 'train'):
    rows = []
    for st in ('off', 'ml'):
        base = 'cut_%s_%s' % (scene, st)
        if not os.path.exists(os.path.join(here, base + '_f299.pfm')):
            print(base, 'missing')
            continue
        ev = evs(base + '.log')
        img = {f: pfm('%s_f%d.pfm' % (base, f)) * (1.2 * 2.0 ** ev[f]) for f in FRAMES}
        ref = lum(img[299])
        mean = float(ref.mean())
        mask = ref > 0.01 * mean
        line = '%-10s ev100 %s |' % (base, {f: ev[f] for f in FRAMES})
        for f in FRAMES:
            l = lum(img[f])
            err = float(np.sqrt((((l - ref) / (ref + 0.01 * mean))[mask] ** 2).mean()))
            m = box5(l)
            grain = float(np.sqrt((((l - m) / (m + 0.01 * mean)) ** 2).mean()))
            line += ' f%d: error %.3f grain %.3f |' % (f, err, grain)
        print(line)
        rows.append((st, img, mean))
    try:
        from PIL import Image

        if rows:
            h, w = rows[0][1][299].shape[:2]
            crops = [(h // 8, w // 2 - 240, 270, 480), (h // 2, w // 4, 270, 480)]  # (top, left, height, width): ceiling centre, mid left
            strips = []
            for (y, x, ch, cw) in crops:
                for st, img, mean in rows:
                    strips.append(np.concatenate([tone(img[f][y:y + ch, x:x + cw], 6.0 * mean) for f in FRAMES], axis=1))
            Image.fromarray(np.concatenate(strips, axis=0)).save(os.path.join(here, 'cut_%s_grid.png' % scene))
            print('wrote cut_%s_grid.png (rows: crop 1 off, crop 1 ml, crop 2 off, crop 2 ml; columns f1, f4, f16, f299)' % scene)
    except Exception as e:  # noqa: BLE001
        print('no image:', e)

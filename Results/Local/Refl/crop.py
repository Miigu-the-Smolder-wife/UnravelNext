"""1:1 crops of captures for the eye: python Results/Local/Refl/crop.py OUT.png x0,y0,w,h(fractions of the image) [--key K | --layer] FILE.pfm[=label] ...
Final images use one display key (the last file's); --layer keys by the median of the pixels with a value (alpha file)."""
import os, sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402


def tonemap(x):
    x = np.nan_to_num(np.maximum(x, 0.0), nan=0.0, posinf=64.0, neginf=0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    y = np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)
    y = np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(y, 1 / 2.4) - 0.055)
    return (y * 255 + 0.5).astype(np.uint8)


def main():
    args = sys.argv[1:]
    out = args.pop(0)
    fx, fy, fw, fh = map(float, args.pop(0).split(","))
    layer, key, scale = False, None, 1
    files = []
    while args:
        a = args.pop(0)
        if a == "--layer":
            layer = True
        elif a == "--key":
            key = float(args.pop(0))
        elif a == "--scale":
            scale = int(args.pop(0))
        else:
            files.append(a)
    imgs = []
    for f in files:
        path, _, label = f.partition("=")
        imgs.append((mm.read_pfm(path), label or os.path.basename(path)[:-4], path))
    if key is None:
        ref, _, path = imgs[-1]
        if layer:
            l = mm.luma(ref)
            a = path[:-4] + "_alpha.pfm"
            m = mm.read_pfm(a)[..., 0] > 0 if os.path.exists(a) else l > 0
            key = 0.18 / max(float(np.median(l[m & (l > 0)])), 1e-9)
        else:
            key = mm.display_key(ref)
    tiles = []
    for img, label, _ in imgs:
        h, w = img.shape[:2]
        x0, y0, cw, ch = int(fx * w), int(fy * h), int(fw * w), int(fh * h)
        im = Image.fromarray(tonemap(img[y0:y0 + ch, x0:x0 + cw] * key))
        if scale != 1:
            im = im.resize((cw * scale, ch * scale), Image.NEAREST)
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, 8 + 7 * len(label), 14], fill=(0, 0, 0))
        d.text((3, 1), label, fill=(255, 255, 0))
        tiles.append(im)
    s = Image.new("RGB", (sum(t.width for t in tiles) + 4 * (len(tiles) - 1), max(t.height for t in tiles)), (255, 0, 255))
    x = 0
    for t in tiles:
        s.paste(t, (x, 0))
        x += t.width + 4
    s.save(out)
    print(out, "key", key)


if __name__ == "__main__":
    main()

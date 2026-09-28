"""Upscaled output against the native image at the same output resolution (renderergate --capture-output vs --capture).

Usage: python UpscaleCompare.py NATIVE.pfm UPSCALED.pfm [--crop x,y,w,h ...] [--exposure E] [--json OUT.json]

Both PFMs are linear radiance x exposure (the gate's units). Measured on luminance, display-mapped with L / (1 + L)
(the regime the eye compares; --exposure scales both first):
  hf_ratio     energy of the upscaled image in the 0.25-0.5 cycles/pixel band over the native image's (radial band of
               the 2D spectrum, Hann-windowed, mean removed): < 1 = detail lost (blur), > 1 = detail added (ringing,
               aliasing, sharpening)
  ssim         mean SSIM (Gaussian window sigma 1.5 px, 11 x 11; C1 = (0.01)^2, C2 = (0.03)^2 on [0, 1])
  edge_ratio   mean gradient magnitude (central differences) of the upscaled over the native: edge contrast
  mean_diff    mean signed difference (upscaled - native) of the mapped luminance: a brightness shift
Candidate gate (RENDERER_REDESIGN 1.6, not a replacement for looking): 0.95 <= hf_ratio <= 1.05 and ssim >= 0.99.
Needs numpy. Crops (output pixels, top-left origin as the images are shown) are reported one by one after the whole
image.
"""
import json, sys

import numpy as np


def read_pfm(path):
    with open(path, "rb") as f:
        kind = f.readline().strip()
        if kind not in (b"PF", b"Pf"):
            raise ValueError(f"{path}: not a PFM")
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.fromfile(f, dtype="<f4" if scale < 0 else ">f4")
    channels = 3 if kind == b"PF" else 1
    img = data.reshape(h, w, channels)[::-1]  # PFM rows are bottom to top
    return img.astype(np.float64)


def luminance(img):
    if img.shape[2] == 1:
        return img[:, :, 0]
    return img[:, :, 0] * 0.2126 + img[:, :, 1] * 0.7152 + img[:, :, 2] * 0.0722


def mapped(lum, exposure):
    l = np.maximum(np.nan_to_num(lum * exposure, nan=0.0, posinf=1e6), 0.0)
    return l / (1.0 + l)


def band_energy(img, lo=0.25, hi=0.5):
    h, w = img.shape
    win = np.outer(np.hanning(h), np.hanning(w))
    f = np.fft.fft2((img - img.mean()) * win)
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    r = np.sqrt(fx * fx + fy * fy)
    band = (r >= lo) & (r <= hi)
    return float(np.sum(np.abs(f[band]) ** 2))


def gaussian_blur(img, sigma=1.5, radius=5):
    x = np.arange(-radius, radius + 1)
    k = np.exp(-(x * x) / (2 * sigma * sigma))
    k /= k.sum()
    pad = np.pad(img, radius, mode="reflect")
    tmp = np.zeros_like(pad)
    for i, v in enumerate(k):
        tmp[:, radius:-radius] += v * pad[:, i : i + pad.shape[1] - 2 * radius]
    out = np.zeros_like(img)
    for i, v in enumerate(k):
        out += v * tmp[i : i + img.shape[0], radius:-radius]
    return out


def ssim(a, b):
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    ma, mb = gaussian_blur(a), gaussian_blur(b)
    va = gaussian_blur(a * a) - ma * ma
    vb = gaussian_blur(b * b) - mb * mb
    cov = gaussian_blur(a * b) - ma * mb
    s = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(s.mean())


def gradient_energy(img):
    gx = 0.5 * (img[1:-1, 2:] - img[1:-1, :-2])
    gy = 0.5 * (img[2:, 1:-1] - img[:-2, 1:-1])
    return float(np.mean(np.sqrt(gx * gx + gy * gy)))


def compare(native, upscaled):
    hn, hu = band_energy(native), band_energy(upscaled)
    gn = gradient_energy(native)
    return {
        "hf_ratio": hu / hn if hn > 0 else float("nan"),
        "ssim": ssim(native, upscaled),
        "edge_ratio": gradient_energy(upscaled) / gn if gn > 0 else float("nan"),
        "mean_diff": float(np.mean(upscaled - native)),
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    native_path, up_path = argv[0], argv[1]
    crops, exposure, json_out = [], 1.0, None
    i = 2
    while i < len(argv):
        if argv[i] == "--crop":
            crops.append(tuple(int(v) for v in argv[i + 1].split(",")))
            i += 2
        elif argv[i] == "--exposure":
            exposure = float(argv[i + 1])
            i += 2
        elif argv[i] == "--json":
            json_out = argv[i + 1]
            i += 2
        else:
            raise SystemExit(f"unknown argument {argv[i]}")
    a, b = read_pfm(native_path), read_pfm(up_path)
    if a.shape != b.shape:
        raise SystemExit(f"sizes differ: {a.shape[1]}x{a.shape[0]} vs {b.shape[1]}x{b.shape[0]} (compare at the same output resolution)")
    la, lb = mapped(luminance(a), exposure), mapped(luminance(b), exposure)
    results = {"whole": compare(la, lb)}
    for (x, y, w, h) in crops:
        results[f"crop {x},{y},{w},{h}"] = compare(la[y : y + h, x : x + w], lb[y : y + h, x : x + w])
    ok = True
    for name, r in results.items():
        verdict = 0.95 <= r["hf_ratio"] <= 1.05 and r["ssim"] >= 0.99
        ok = ok and verdict
        print(f"{name:>28}: hf_ratio {r['hf_ratio']:.3f}  ssim {r['ssim']:.4f}  edge_ratio {r['edge_ratio']:.3f}  mean_diff {r['mean_diff']:+.4f}  "
              f"-> {'within' if verdict else 'OUTSIDE'} the candidate gate")
    if json_out:
        with open(json_out, "w") as f:
            json.dump(results, f, indent=2)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

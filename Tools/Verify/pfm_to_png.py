# The gate's captures (PFM: linear radiance x exposure, before the display encoding) as PNGs through the display
# rendering the renderer applies (local exposure as LocalExposure.hlsli - the final picture only, with the defaults of
# shading.toml; then ShadingCommon.hlsli shFilm: gamut expansion, blue correction, glow, red modifier, the film curve;
# then the sRGB OETF), so a capture is seen as the game shows it (without bloom, vignette and grain).
#   python Tools/Verify/pfm_to_png.py <file.pfm | directory> [--crop x,y,w,h] [--scale n] [--no-local-exposure]
# A directory converts every .pfm in it (not the _alpha / layer files unless --all). Needs numpy only.
import os
import struct
import sys
import zlib

import numpy as np


def read_pfm(path):
    with open(path, 'rb') as f:
        kind = f.readline().strip()
        if kind not in (b'PF', b'Pf'):
            raise ValueError('%s: not a PFM' % path)
        w, h = (int(v) for v in f.readline().split())
        scale = float(f.readline())
        channels = 3 if kind == b'PF' else 1
        data = np.frombuffer(f.read(w * h * channels * 4), dtype='<f4' if scale < 0 else '>f4')
    img = data.reshape(h, w, channels)[::-1]  # PFM rows run bottom to top
    return np.repeat(img, 3, axis=2) if channels == 1 else img


def local_exposure(img, highlight=0.8, shadow=0.8, detail=1.0, blend=0.6, kernel_percent=50.0):
    """LocalExposure.hlsli on a whole image: the factor per pixel."""
    depth, tile, log_min, log_max = 32, 128, -14.0, 10.0
    h, w, _ = img.shape
    lum = np.maximum(np.maximum(img, 0.0) @ np.array([0.2126, 0.7152, 0.0722]), 2.0 ** log_min)
    log_lum = np.log2(lum)
    position = np.clip((log_lum - log_min) / (log_max - log_min), 0.0, 1.0)
    gx, gy = (w + tile - 1) // tile, (h + tile - 1) // tile
    # the grid from every second pixel, each sample split between its two nearest buckets
    ys, xs = np.mgrid[0:h:2, 0:w:2]
    at = position[0:h:2, 0:w:2] * (depth - 1)
    b0 = np.minimum(at.astype(np.int64), depth - 1)
    b1 = np.minimum(b0 + 1, depth - 1)
    f = at - b0
    sums = np.zeros((gy, gx, depth))
    weights = np.zeros((gy, gx, depth))
    sample_log = log_lum[0:h:2, 0:w:2]
    for bucket, weight in ((b0, 1 - f), (b1, f)):
        np.add.at(weights, (ys // tile, xs // tile, bucket), weight)
        np.add.at(sums, (ys // tile, xs // tile, bucket), weight * sample_log)
    total = weights.sum(axis=2)
    mean = np.where(total > 0, sums.sum(axis=2) / np.maximum(total, 1e-12), np.log2(0.18))
    # the blurred tile means: exp(-16.7 (d / R)^2), R = kernel percent / 100 x width / 2 in tiles, mirrored borders
    radius = max(kernel_percent * 0.01 * 0.5 * w / tile, 0.5)
    reach = int(np.clip(np.ceil(radius * 0.6), 1, 16))
    blurred = np.zeros_like(mean)
    norm = 0.0
    for dy in range(-reach, reach + 1):
        for dx in range(-reach, reach + 1):
            yy = np.abs(np.arange(gy) + dy)
            yy = np.clip(np.minimum(yy, 2 * (gy - 1) - yy), 0, gy - 1)
            xx = np.abs(np.arange(gx) + dx)
            xx = np.clip(np.minimum(xx, 2 * (gx - 1) - xx), 0, gx - 1)
            k = np.exp(-16.7 * (dx * dx + dy * dy) / (radius * radius))
            blurred += k * mean[np.ix_(yy, xx)]
            norm += k
    blurred /= norm
    # per pixel: the grid at (pixel, its own luminance), trilinear with clamped addressing; the blurred value, bilinear
    py, px = np.mgrid[0:h, 0:w]
    u = np.clip((px + 0.5) / tile - 0.5, 0, gx - 1)
    v = np.clip((py + 0.5) / tile - 0.5, 0, gy - 1)
    z = position * (depth - 1)
    x0, y0, z0 = np.floor(u).astype(np.int64), np.floor(v).astype(np.int64), np.minimum(np.floor(z).astype(np.int64), depth - 1)
    x1, y1, z1 = np.minimum(x0 + 1, gx - 1), np.minimum(y0 + 1, gy - 1), np.minimum(z0 + 1, depth - 1)
    fx, fy, fz = u - x0, v - y0, z - z0
    cell_sum = np.zeros((h, w))
    cell_weight = np.zeros((h, w))
    blurred_at = np.zeros((h, w))
    for yi, wy in ((y0, 1 - fy), (y1, fy)):
        for xi, wx in ((x0, 1 - fx), (x1, fx)):
            blurred_at += wy * wx * blurred[yi, xi]
            for zi, wz in ((z0, 1 - fz), (z1, fz)):
                cell_sum += wy * wx * wz * sums[yi, xi, zi]
                cell_weight += wy * wx * wz * weights[yi, xi, zi]
    bilateral = np.where(cell_weight < 0.001, blurred_at, cell_sum / np.maximum(cell_weight, 1e-12))
    base = bilateral + (blurred_at - bilateral) * blend
    middle = np.log2(0.18)
    centred = base - middle
    local = middle + centred * np.where(centred > 0, highlight, shadow) + (log_lum - base) * detail
    return np.exp2(local - log_lum)


def film(color):
    to_ap1 = np.array([[0.6130973, 0.3395229, 0.0473793], [0.0701942, 0.9163556, 0.0134526], [0.0206156, 0.1095698, 0.8698151]])
    to_srgb = np.array([[1.7050510, -0.6217921, -0.0832589], [-0.1302564, 1.1408047, -0.0105483], [-0.0240033, -0.1289690, 1.1529723]])
    ap1_to_ap0 = np.array([[0.6954522, 0.1406787, 0.1638691], [0.0447946, 0.8596711, 0.0955343], [-0.0055259, 0.0040252, 1.0015007]])
    ap0_to_ap1 = np.array([[1.4514393, -0.2365107, -0.2149286], [-0.0765538, 1.1762297, -0.0996759], [0.0083161, -0.0060324, 0.9977163]])
    expand = np.array([[1.3704124, -0.3292922, -0.0636831], [-0.0834335, 1.0970927, -0.0108614], [-0.0257933, -0.0986258, 1.2036949]])
    ap1_y = np.array([0.2722287, 0.6740818, 0.0536895])
    slope, toe, shoulder, black_clip, white_clip = 0.88, 0.55, 0.26, 0.0, 0.04
    blue = 0.6
    a = np.maximum(color, 0.0) @ to_ap1.T
    luma = a @ ap1_y
    chroma = a / np.maximum(luma, 1e-10)[..., None] - 1.0
    amount = (1 - np.exp2(-4 * np.sum(chroma * chroma, axis=-1))) * (1 - np.exp2(-4 * luma * luma))
    a = a + (a @ expand.T - a) * amount[..., None]
    corrected = np.stack([0.9386394 * a[..., 0] + 0.0613606 * a[..., 2], 0.8307941 * a[..., 1] + 0.1692059 * a[..., 2], a[..., 2]], axis=-1)
    a = a + (corrected - a) * blue
    c0 = a @ ap1_to_ap0.T
    lowest, highest = c0.min(axis=-1), c0.max(axis=-1)
    saturation = (np.maximum(highest, 1e-10) - np.maximum(lowest, 1e-10)) / np.maximum(highest, 1e-2)
    r, g, b = c0[..., 0], c0[..., 1], c0[..., 2]
    yc = (r + g + b + 1.75 * np.sqrt(np.maximum(b * (b - g) + g * (g - r) + r * (r - b), 0.0))) / 3.0
    x = (saturation - 0.4) / 0.2
    t = np.maximum(1 - np.abs(0.5 * x), 0.0)
    gain = 0.05 * 0.5 * (1 + np.sign(x) * (1 - t * t))
    mid = 0.08
    glow = np.where(yc <= 2.0 / 3.0 * mid, gain, np.where(yc >= 2 * mid, 0.0, gain * (mid / np.maximum(yc, 1e-10) - 0.5)))
    c0 = c0 * (1 + glow)[..., None]
    r, g, b = c0[..., 0], c0[..., 1], c0[..., 2]
    hue = np.degrees(np.arctan2(1.7320508 * (g - b), 2 * r - g - b))
    w = np.clip(1 - np.abs(2 * hue / 135.0), 0.0, 1.0)
    w = w * w * (3 - 2 * w)
    c0 = c0.copy()
    c0[..., 0] = r + w * w * saturation * (0.03 - r) * (1 - 0.82)
    a = np.maximum(c0 @ ap0_to_ap1.T, 0.0)
    a = np.maximum((a @ ap1_y)[..., None] * (1 - 0.96) + a * 0.96, 0.0)
    toe_scale, shoulder_scale = 1 + black_clip - toe, 1 + white_clip - shoulder
    bt = (0.18 + black_clip) / toe_scale - 1
    toe_match = np.log10(0.18) - 0.5 * np.log((1 + bt) / (1 - bt)) * (toe_scale / slope)
    straight_match = (1 - toe) / slope - toe_match
    shoulder_match = shoulder / slope - straight_match
    l = np.log10(np.maximum(a, 1e-10))
    straight = slope * (l + straight_match)
    toe_color = -black_clip + 2 * toe_scale / (1 + np.exp((-2 * slope / toe_scale) * (l - toe_match)))
    shoulder_color = (1 + white_clip) - 2 * shoulder_scale / (1 + np.exp((2 * slope / shoulder_scale) * (l - shoulder_match)))
    toe_color = np.where(l < toe_match, toe_color, straight)
    shoulder_color = np.where(l > shoulder_match, shoulder_color, straight)
    t = np.clip((l - toe_match) / (shoulder_match - toe_match), 0.0, 1.0)
    if shoulder_match < toe_match:
        t = 1 - t
    t = (3 - 2 * t) * t * t
    a = toe_color + (shoulder_color - toe_color) * t
    a = np.maximum((a @ ap1_y)[..., None] * (1 - 0.93) + a * 0.93, 0.0)
    restored = np.stack([1.0653749 * a[..., 0] - 0.0653710 * a[..., 2], 1.2036635 * a[..., 1] - 0.2036677 * a[..., 2], a[..., 2]], axis=-1)
    a = a + (restored - a) * blue
    return np.clip(a @ to_srgb.T, 0.0, 1.0)


def oetf(d):
    return np.where(d <= 0.0031308, d * 12.92, 1.055 * np.power(np.maximum(d, 1e-12), 1 / 2.4) - 0.055)


def write_png(path, rgb8):
    h, w, _ = rgb8.shape
    raw = b''.join(b'\x00' + rgb8[y].tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def convert(path, crop=None, scale=1, local=True):
    img = np.nan_to_num(read_pfm(path).astype(np.float64), nan=0.0, posinf=65504.0, neginf=0.0)
    name = os.path.basename(path)
    layer = any(tag in name for tag in ('_alpha', '_gi_', '_refl_', '_shadow_', '_reflmode_', '_depth_', '_ao_', '_roughspec_', '_card'))
    if local and not layer and img.shape[0] >= 256 and img.shape[1] >= 256:
        img = img * local_exposure(img)[..., None]  # (the whole image's, before any crop)
    if crop:
        x, y, w, h = crop
        img = img[y:y + h, x:x + w]
    out = (oetf(film(img)) * 255.0 + 0.5).astype(np.uint8)
    if scale > 1:
        out = np.repeat(np.repeat(out, scale, axis=0), scale, axis=1)
    target = os.path.splitext(path)[0] + ('' if not crop else '_crop%d_%d' % (crop[0], crop[1])) + '.png'
    write_png(target, out)
    return target


def main(argv):
    if len(argv) < 2:
        print(__doc__ or 'usage: pfm_to_png.py <file.pfm | directory> [--crop x,y,w,h] [--scale n] [--all]')
        return 2
    crop, scale, every, local = None, 1, False, True
    paths = []
    i = 1
    while i < len(argv):
        if argv[i] == '--crop':
            crop = tuple(int(v) for v in argv[i + 1].split(','))
            i += 2
        elif argv[i] == '--scale':
            scale = int(argv[i + 1])
            i += 2
        elif argv[i] == '--all':
            every = True
            i += 1
        elif argv[i] == '--no-local-exposure':
            local = False
            i += 1
        else:
            paths.append(argv[i])
            i += 1
    files = []
    for p in paths:
        if os.path.isdir(p):
            for name in sorted(os.listdir(p)):
                if name.endswith('.pfm') and (every or not any(tag in name for tag in ('_alpha', '_gi_', '_refl_', '_shadow_', '_reflmode_', '_depth_', '_ao_', '_roughspec_', '_card'))):
                    files.append(os.path.join(p, name))
        else:
            files.append(p)
    for f in files:
        print(convert(f, crop, scale, local))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

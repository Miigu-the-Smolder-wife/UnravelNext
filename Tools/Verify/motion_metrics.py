"""Image metrics of RENDERER_REDESIGN_V2 3.1 (motion, first frames, lighting changes), shared by verify_analyze.py and
by hand (python motion_metrics.py compare TEST.pfm REF.pfm [--layer]).

All metrics work on linear PFM captures of renderergate (radiance x exposure; --capture, --capture-output, and the
internal layers of --capture-layers). A reference is the converged still image of the same camera pose (--path-time T,
600 frames), so a frame of a moving sequence is judged against what it should converge to.

  tile_p95      16 x 16 tile mean of |L(x) - L_ref(x)| over the tile mean of L_ref, P95 over tiles, L = key x luma / (1 +
                key x luma) at the reference's display key (the same definition as the baseline's -Phases convergence).
  layer_sigma   an unmodulated lighting layer's own high-pass noise: per 8 x 8 tile the standard deviation of L - box3(L)
                over the tile mean of L, P95 over tiles with data (reported for the test and the reference: the reference's
                value is the layer's real texture, the excess is noise).
  layer_error   per 8 x 8 tile mean |L - L_ref| over the tile mean of L_ref, P95 (the layer's distance to its converged
                self: noise and bias together).
  pattern       grid / comb index: for each period of a list (2, 3, 4, 6, 8, 12, 16 px) the spectral power along the x and
                y axes at that frequency over the median power of its ring (same radius), test over reference; the
                maximum over periods and axes. 1 = no pattern the reference does not have; the design's limit is 1.1.
  flicker       per-pixel standard deviation over still frames of the 10-bit display luma (ACES fit, sRGB, the first
                frame's key): mean, p50, p99, max - the unit of the Unity play-mode flicker map.
"""
import json, os, sys

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def read_pfm(path):
    with open(path, "rb") as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        ch = 3 if kind == b"PF" else 1
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4").reshape(h, w, ch)
    data = np.flipud(data)
    return data[..., :3] if ch == 3 else np.repeat(data, 3, axis=2)


def luma(img):
    return np.nan_to_num(img @ LUMA, nan=0.0, posinf=0.0, neginf=0.0)


def display_key(img):
    return 0.18 / float(np.exp(np.mean(np.log(np.maximum(luma(img), 0) + 1e-4))))


def mapped(img, key):
    l = np.maximum(luma(img), 0) * key
    return l / (1 + l)


def tiles(a, t):
    h, w = a.shape[:2]
    th, tw = h // t, w // t
    return a[:th * t, :tw * t].reshape(th, t, tw, t)


def tile_p95(img, ref, key=None):
    key = display_key(ref) if key is None else key
    mi, mr = mapped(img, key), mapped(ref, key)
    d = tiles(np.abs(mi - mr), 16).mean(axis=(1, 3))
    m = tiles(mr, 16).mean(axis=(1, 3))
    e = d / np.maximum(m, 1e-3)
    return float(np.percentile(e, 95)), float(np.abs(mi - mr).mean() / max(mr.mean(), 1e-9))


def box3(a):
    p = np.pad(a, 1, mode="edge")
    return sum(p[dy:dy + a.shape[0], dx:dx + a.shape[1]] for dy in range(3) for dx in range(3)) / 9.0


def layer_sigma(layer, mask=None):
    l = luma(layer).astype(np.float64)
    hp = l - box3(l)
    s = tiles(hp, 8).std(axis=(1, 3))
    m = tiles(l, 8).mean(axis=(1, 3))
    valid = m > max(np.percentile(m, 50) * 1e-3, 1e-9)
    if mask is not None:
        valid &= tiles(mask.astype(np.float64), 8).mean(axis=(1, 3)) > 0.99
    if not valid.any():
        return float("nan")
    return float(np.percentile(s[valid] / m[valid], 95))


def layer_error(layer, ref, mask=None):
    l, r = luma(layer).astype(np.float64), luma(ref).astype(np.float64)
    d = tiles(np.abs(l - r), 8).mean(axis=(1, 3))
    m = tiles(r, 8).mean(axis=(1, 3))
    valid = m > max(np.percentile(m, 50) * 1e-3, 1e-9)
    if mask is not None:
        valid &= tiles(mask.astype(np.float64), 8).mean(axis=(1, 3)) > 0.99
    if not valid.any():
        return float("nan")
    return float(np.percentile(d[valid] / m[valid], 95))


def _peaks(l, periods):
    h, w = l.shape
    win = np.outer(np.hanning(h), np.hanning(w))
    p = np.abs(np.fft.fft2((l - l.mean()) * win)) ** 2
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    r = np.sqrt(fx * fx + fy * fy)
    out = {}
    for per in periods:
        f = 1.0 / per
        ring = np.abs(r - f) < max(1.5 / min(h, w), 0.01 * f)
        med = float(np.median(p[ring])) if ring.any() else 0.0
        for axis in ("x", "y"):
            if axis == "x":
                band = (np.abs(np.abs(fx) - f) < 1.5 / w) & (np.abs(fy) < 1.5 / h)
            else:
                band = (np.abs(np.abs(fy) - f) < 1.5 / h) & (np.abs(fx) < 1.5 / w)
            out[(per, axis)] = float(p[band].max()) / max(med, 1e-30) if band.any() else 0.0
    return out


def pattern(img, ref, periods=(2, 3, 4, 6, 8, 12, 16), key=None):
    key = display_key(ref) if key is None else key
    a, b = _peaks(mapped(img, key), periods), _peaks(mapped(ref, key), periods)
    best, where = 0.0, None
    for k in a:
        v = a[k] / max(b[k], 1.0)  # a peak the reference lacks counts from the ring level (1) up
        if v > best:
            best, where = v, k
    return best, where


def display10(img, key):
    x = np.nan_to_num(np.maximum(img * key, 0.0), nan=0.0, posinf=64.0, neginf=0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    y = np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)
    y = np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(y, 1 / 2.4) - 0.055)
    return (y @ LUMA) * 1023.0


def flicker(frames):
    key = display_key(frames[0])
    stack = np.stack([display10(f, key) for f in frames])
    s = stack.std(axis=0)
    return {"mean": float(s.mean()), "p50": float(np.percentile(s, 50)), "p99": float(np.percentile(s, 99)), "max": float(s.max())}


def compare(test, ref, layer=False, mask=None):
    """Every metric of a test image against its reference (layer: an internal lighting layer, not a final image)."""
    a, b = read_pfm(test), read_pfm(ref)
    if a.shape != b.shape:
        return {"error": f"size {a.shape} vs {b.shape}"}
    r = {}
    r["tile_p95"], r["mean_rel"] = tile_p95(a, b)
    r["pattern"], where = pattern(a, b)
    r["pattern_at"] = f"{where[0]} px {where[1]}" if where else ""
    if layer:
        r["sigma"], r["sigma_ref"] = layer_sigma(a, mask), layer_sigma(b, mask)
        r["layer_error_p95"] = layer_error(a, b, mask)
    return r


if __name__ == "__main__":
    if len(sys.argv) >= 4 and sys.argv[1] == "compare":
        print(json.dumps(compare(sys.argv[2], sys.argv[3], layer="--layer" in sys.argv), indent=1))
    elif len(sys.argv) >= 3 and sys.argv[1] == "flicker":
        print(json.dumps(flicker([read_pfm(p) for p in sys.argv[2:]]), indent=1))
    else:
        print(__doc__)

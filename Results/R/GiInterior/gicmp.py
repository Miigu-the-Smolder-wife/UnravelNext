# GI interior comparison (render B, 2026-09-27): engine linear capture (RendererGate --capture, radiance x exposure) against
# the CPU reference (unx_reference render, same units). Usage:
#   python gicmp.py <engine.pfm> <reference.pfm> <out_prefix> [--ev-display EV]
# The engine image is box-downsampled to the reference size when it is an integer multiple. Reports, over pixels the
# reference sees as surfaces (finite, > 0):
#   bias   = sum(E - R) / sum(R)
#   relMSE = mean((E - R)^2 / (R^2 + 1e-2 mean(R)^2))
#   tile   = per 8 x 8 tile |mean E / mean R - 1|: P50, P95 (block structure at the tile scale)
#   refnoise = the same tile statistic between the reference halves (the floor the comparison can resolve)
# and writes <out_prefix>_eng.png, _ref.png (same exposure, ACES-like curve) and _err.png (signed tile error, red = too bright).
import sys, numpy as np
from PIL import Image

def readpfm(p):
    with open(p, 'rb') as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        d = np.fromfile(f, '<f4' if scale < 0 else '>f4')
    c = 3 if kind == b'PF' else 1
    return np.flipud(d.reshape(h, w, c)).astype(np.float64)

def down(a, h, w):
    fy, fx = a.shape[0] // h, a.shape[1] // w
    return a[:h * fy, :w * fx].reshape(h, fy, w, fx, -1).mean(axis=(1, 3))

def lum(a): return a[..., 0] * 0.2126 + a[..., 1] * 0.7152 + a[..., 2] * 0.0722

def tiles(a, t=8):
    h, w = a.shape[0] // t * t, a.shape[1] // t * t
    return a[:h, :w].reshape(h // t, t, w // t, t).mean(axis=(1, 3))

def png(a, path, gain):
    x = a * gain
    x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)
    Image.fromarray((np.clip(x, 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)).save(path)

eng, ref, out = sys.argv[1], sys.argv[2], sys.argv[3]
E, R = readpfm(eng), readpfm(ref)
if E.shape[:2] != R.shape[:2]: E = down(E, R.shape[0], R.shape[1])
le, lr = lum(E), lum(R)
m = np.isfinite(le) & np.isfinite(lr) & (lr > 0)
bias = (le[m] - lr[m]).sum() / lr[m].sum()
mr = lr[m].mean()
relmse = np.mean((le[m] - lr[m]) ** 2 / (lr[m] ** 2 + 1e-2 * mr ** 2))
te, tr = tiles(np.where(m, le, 0)), tiles(np.where(m, lr, 0))
tm = tiles(m.astype(float)) > 0.99
terr = np.abs(te[tm] / tr[tm] - 1)
line = f'bias {bias:+.4f}  relMSE {relmse:.5f}  tile|err| P50 {np.percentile(terr,50):.4f} P95 {np.percentile(terr,95):.4f}'
import os
ha, hb = ref.replace('.pfm', '.halfA.pfm'), ref.replace('.pfm', '.halfB.pfm')
if os.path.exists(ha) and os.path.exists(hb):
    A, B = lum(readpfm(ha)), lum(readpfm(hb))
    ta, tb = tiles(np.where(m, A, 0)), tiles(np.where(m, B, 0))
    rn = np.abs(ta[tm] / tb[tm] - 1) / 2  # half-vs-half difference / 2 ~ the full reference's tile noise
    line += f'  refnoise P50 {np.percentile(rn,50):.4f} P95 {np.percentile(rn,95):.4f}'
print(line)
gain = 1.0
for i, a in enumerate(sys.argv):
    if a == '--gain': gain = float(sys.argv[i + 1])
if gain == 1.0: gain = 0.18 / max(np.exp(np.mean(np.log(lr[m] + 1e-6))), 1e-9)
png(E, out + '_eng.png', gain); png(R, out + '_ref.png', gain)
s = np.zeros(te.shape); s[tm] = te[tm] / tr[tm] - 1
rgb = np.zeros(te.shape + (3,)); rgb[..., 0] = np.clip(s / 0.2, 0, 1); rgb[..., 2] = np.clip(-s / 0.2, 0, 1)
Image.fromarray((rgb * 255).astype(np.uint8)).resize((te.shape[1] * 8, te.shape[0] * 8), Image.NEAREST).save(out + '_err.png')
print(f'display gain {gain:.4g}')

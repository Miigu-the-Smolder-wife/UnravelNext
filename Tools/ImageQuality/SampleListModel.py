"""MB-B (RENDERER_REDESIGN_V2 1.4, 7): CPU model of the multi-frame sample reconstruction ("sample list") before the GPU
kernel. No output-resolution history image: the last k frames' internal samples are kept with their exact surface
positions and gathered per output pixel at their positions in the current frame (identity placement: no accumulated
resampling). Compared with the native point samples of the same frame (UpscaleCompare: hf_ratio 0.25-0.5 cycles/px,
SSIM) - the design's gate is SSIM >= 0.99 and 0.95 <= hf_ratio <= 1.05, still and in slow motion.

Scene: UpscaleModel's (high-contrast wood grain up to ~0.65 cycles/output px, thin mouldings), point-sampled.
A camera pan of V output px / frame moves the output grid over the scene; the samples keep their scene positions.

Scenario string "s,k,jitter,gather[,V]":
  s        internal scale: 2/3 or 1/2 (or a float)
  k        frames of samples kept (the current one included)
  jitter   halton (64 Halton 2, 3), lattice (output-grid aligned: every sample of a cycle lands on an output pixel
           centre or on the half-pixel lattice; cycle 4 at 1/2, 9 at 2/3), r2 (R2 sequence)
  gather   gK = exp(-K d^2) over the 3 x 3 internal samples of each kept frame, d in output px, normalised; below a
           total weight of 1e-3 the current frame's wide kernel (Upscale.hlsl's) fills the pixel;
           nK = the same but the nearest kept sample alone when the weight is below 1e-3 (no wide fill)
  V        pan in output px / frame (default 0)
Environment: FR frames (default 40), LCF / LCM a lighting change at frame LCF by factor LCM (the gather's box test drops
kept samples outside the current 3 x 3 samples' min / max widened by BOXM x extent, default 0.5), DF a disocclusion at
frame DF of the right quarter (new identity: older samples there are dropped), REGION metrics on that quarter only.
Output per scenario: the last frame's hf_ratio, ssim, p99 error; with LCF / DF the metrics of frames DF / LCF + 0..4.
Example: python SampleListModel.py 2/3,4,halton,g40 2/3,9,lattice,g40 1/2,4,lattice,g40 1/2,4,lattice,g40,0.3
"""
import os, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import UpscaleModel as um  # noqa: E402  (the scene and its point sampling)
import UpscaleCompare as uc  # noqa: E402

W, H = um.W, um.H
tm = um.tm


def parse_scale(v):
    if "/" in v:
        a, b = v.split("/")
        return float(a) / float(b)
    return float(v)


def jitter_sequence(mode, s, n=64):
    """Sub-pixel jitters (internal px, in [-0.5, 0.5)) per frame."""
    if mode == "halton":
        return [(um.halton(k, 2) - 0.5, um.halton(k, 3) - 0.5) for k in range(1, n + 1)]
    if mode == "r2":
        a1, a2 = 0.7548776662466927, 0.5698402909980532
        return [(((0.5 + a1 * k) % 1) - 0.5, ((0.5 + a2 * k) % 1) - 0.5) for k in range(1, n + 1)]
    if mode == "lattice":
        # An internal sample centre (i + 0.5 - j) / s lands on the output centre m + 0.5 when j = i + 0.5 - (m + 0.5) s;
        # per axis the distinct jitters (mod 1, in [-0.5, 0.5)): 2 at s = 1/2, 3 at s = 2/3. Every frame then has some
        # samples exactly on output centres (all of them at 1/2; one in two per axis at 2/3, the others on the half
        # lattice), and one cycle of the pairs puts a sample on every output centre.
        js = sorted({round(((1.0 - (m + 0.5) * s) % 1.0) - 0.5, 9) for m in range(12)})
        pairs = [(a, b) for a in js for b in js]
        # visit the pairs in an order that spreads any window of consecutive frames (a Latin-square walk)
        n = len(js)
        order = [pairs[((t % n) * n + ((t // n + t) % n))] for t in range(len(pairs))]
        return order
    raise ValueError(mode)


def render(frame, s, jit, pan, light):
    """This frame's internal samples: colours (tone-mapped) and scene positions."""
    iw = int(round(W * s))
    sx = W / iw
    jx, jy = jit
    iy, ix = np.mgrid[0:iw, 0:iw]
    px = (ix + 0.5 - jx) * sx + pan
    py = (iy + 0.5 - jy) * sx
    c = um.sample(px, py) * light
    return tm(c), px, py, iw, sx


def run(spec):
    parts = spec.split(",")
    s = parse_scale(parts[0])
    k = int(parts[1])
    jmode = parts[2]
    gather = parts[3]
    V = float(parts[4]) if len(parts) > 4 else 0.0
    K = float(gather[1:])
    nearest_fill = gather[0] == "n"
    FR = int(os.environ.get("FR", "40"))
    LCF, LCM = int(os.environ.get("LCF", "-1")), float(os.environ.get("LCM", "1.3"))
    DF = int(os.environ.get("DF", "-1"))
    BOXM = float(os.environ.get("BOXM", "0.5"))
    J = jitter_sequence(jmode, s)
    ox, oy = um.ox, um.oy
    kept = []  # per kept frame: colours, scene x, scene y, identity epoch map (per internal pixel), jitter, pan
    results = []
    for f in range(FR):
        pan = f * V
        light = LCM if (LCF >= 0 and f >= LCF) else 1.0
        c, px, py, iw, sx = render(f, s, J[f % len(J)], pan, light)
        # identity: the right quarter of the SCREEN becomes a new surface at DF (older samples there are another surface)
        ident = np.where((DF >= 0) & (f >= DF) & ((px - pan) > 0.75 * W), 1, 0)
        kept.append((c, px, py, ident, J[f % len(J)], pan))
        kept = kept[-k:]
        cur_ident = np.where((DF >= 0) & (f >= DF) & (ox > 0.75 * W), 1, 0)
        # current 3 x 3 box (for lighting consistency) and the wide fill, from this frame
        jx, jy = J[f % len(J)]
        x, y = (ox) / sx, oy / sx
        kcx, kcy = np.floor(x + jx).astype(int), np.floor(y + jy).astype(int)
        lo = np.full(ox.shape, 1e30)
        hi = np.full(ox.shape, -1e30)
        sw = np.zeros(ox.shape)
        ww = np.zeros(ox.shape)
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                kx, ky = np.clip(kcx + dx, 0, iw - 1), np.clip(kcy + dy, 0, iw - 1)
                v = c[ky, kx]
                lo, hi = np.minimum(lo, v), np.maximum(hi, v)
                ddx, ddy = kx + 0.5 - jx - x, ky + 0.5 - jy - y
                w2 = np.exp(-2.29 * (ddx * ddx + ddy * ddy))
                sw += w2 * v
                ww += w2
        wide = sw / ww
        ext = np.maximum(hi - lo, 1e-4)
        blo, bhi = lo - BOXM * ext, hi + BOXM * ext
        acc = np.zeros(ox.shape)
        wsum = np.zeros(ox.shape)
        best_d = np.full(ox.shape, 1e30)
        best_c = wide.copy()
        for (kc, kpx, kpy, kid, (kjx, kjy), kpan) in kept:
            # the output pixel's position in that frame's internal grid (the scene moved by pan - kpan since)
            xs = (ox + pan - kpan) / sx
            ys = oy / sx
            cx, cy = np.floor(xs + kjx).astype(int), np.floor(ys + kjy).astype(int)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    kx, ky = np.clip(cx + dx, 0, iw - 1), np.clip(cy + dy, 0, iw - 1)
                    v = kc[ky, kx]
                    # exact placement: the sample's scene position in the current output frame
                    d2 = (kpx[ky, kx] - pan - ox) ** 2 + (kpy[ky, kx] - oy) ** 2
                    ok = (kid[ky, kx] == cur_ident) & (v >= blo) & (v <= bhi)
                    w = np.where(ok, np.exp(-K * d2), 0.0)
                    acc += w * v
                    wsum += w
                    closer = ok & (d2 < best_d)
                    best_d = np.where(closer, d2, best_d)
                    best_c = np.where(closer, v, best_c)
        fill = best_c if nearest_fill else wide
        out = np.where(wsum > 1e-3, acc / np.maximum(wsum, 1e-12), fill)
        native = tm(um.sample(ox + pan, oy) * light)
        results.append((f, out, native))
    return results


def metrics(out, native, region=False):
    a, b = native[16:-16, 16:-16], out[16:-16, 16:-16]
    if region:
        a, b = native[16:-16, int(0.75 * W) + 4:-16], out[16:-16, int(0.75 * W) + 4:-16]
    r = uc.compare(a, b)
    return r["hf_ratio"], r["ssim"], float(np.percentile(np.abs(a - b), 99))


if __name__ == "__main__":
    specs = sys.argv[1:] or ["2/3,4,halton,g40", "2/3,4,lattice,g40", "2/3,9,lattice,g40", "1/2,4,lattice,g40", "1/2,4,halton,g40"]
    for spec in specs:
        res = run(spec)
        f, out, nat = res[-1]
        hf, ss, p99 = metrics(out, nat)
        line = f"{spec:28s} last frame {f}: hf {hf:.3f} ssim {ss:.4f} p99 {p99:.4f}"
        ev = int(os.environ.get("LCF", os.environ.get("DF", "-1")))
        if ev >= 0:
            region = os.environ.get("DF") is not None and os.environ.get("REGION", "1") == "1"
            seq = []
            for (g, o, n) in res:
                if ev <= g <= ev + 4:
                    h2, s2, p2 = metrics(o, n, region)
                    seq.append(f"{g - ev + 1}: hf {h2:.2f} ssim {s2:.3f}")
            line += " | after the event: " + ", ".join(seq)
        print(line, flush=True)

"""The first frame's layers (diag0.py captures) per region and reflection path:
    python Results/Local/Refl/diag0_analyze.py TAG CASE MODETAG [frame]
Per region and mode (MODETAG's reflmode capture of the still run): the means of albedo x stochastic, albedo x stochastic',
|residual|, |residual'| over the reference level (A0's converged reflection layer there), the share of pixels whose
stochastic layer is 0 before the filter, and the mean albedo."""
import os, sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402

REGIONS = {"ceiling": (0.55, 0.12, 0.85, 0.26), "pillar": (0.66, 0.20, 0.74, 0.50), "floor": (0.30, 0.75, 0.70, 0.98), "left wall": (0.02, 0.20, 0.20, 0.40)}


def main():
    tag, case, modetag = sys.argv[1:4]
    frame = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    raw = os.path.join(ROOT, "Cache", "ReflJudge")
    v = {k: mm.read_pfm(os.path.join(raw, f"{tag}v{k}", f"{case}_diag0_1080_refl_f{frame}.pfm")).astype(np.float64) for k in (1, 2, 3, 4, 5)}
    mode = np.rint(mm.read_pfm(os.path.join(raw, modetag, f"{case}_still_1080_reflmode_f{frame}.pfm"))[..., 0]).astype(int)
    ref = mm.luma(mm.read_pfm(os.path.join(raw, "A0", f"{case}_still_1080_refl_f299.pfm")))
    aS, aS1, R, R1 = mm.luma(v[5] * v[1]), mm.luma(v[5] * v[2]), mm.luma(v[3]), mm.luma(v[4])
    S = mm.luma(v[1])
    h, w = mode.shape
    sys.stdout.reconfigure(encoding="utf-8")
    for name, (x0, y0, x1, y1) in REGIONS.items():
        sl = (slice(int(y0 * h), int(y1 * h)), slice(int(x0 * w), int(x1 * w)))
        for k, label in ((1, "M"), (2, "G")):
            sel = mode[sl] == k
            if sel.mean() < 0.02:
                continue
            t = max(ref[sl][sel].mean(), 1e-12)
            print(f"{name} {label} ({100 * sel.mean():.0f} %) f{frame}: albedo x S {aS[sl][sel].mean() / t:.2f} -> albedo x S' {aS1[sl][sel].mean() / t:.2f}; "
                  f"|R| {R[sl][sel].mean() / t:.2f} -> |R'| {R1[sl][sel].mean() / t:.2f}; S = 0 in {100 * (S[sl][sel] <= 0).mean():.0f} %; "
                  f"S median / mean {np.median(S[sl][sel]) / max(S[sl][sel].mean(), 1e-12):.2f}; mean albedo {mm.luma(v[5])[sl][sel].mean():.2f} "
                  f"(P5 {np.percentile(mm.luma(v[5])[sl][sel], 5):.2f}); (reference level 1 = A0 f299)")


if __name__ == "__main__":
    main()

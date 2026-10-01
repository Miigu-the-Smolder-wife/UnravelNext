"""Reflection path shares per region from a reflmode capture (renderergate --capture-layers reflmode: r = mode 0 K,
1 M, 2 G, 3 planar; g = log2 of the G spacing; b = own-job bit), with the reflection layer's level and rms error
against a reference frame split by mode:
    python Results/Local/Refl/mode_stats.py Cache/ReflJudge/B/bath_hall_still_1080 0 299
Regions are fractions of the internal image (bath hall view): pillar, back wall, floor, left wall, whole view."""
import os, sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402

REGIONS = {"pillar": (0.66, 0.20, 0.74, 0.50), "back wall": (0.80, 0.25, 0.99, 0.45), "floor": (0.30, 0.75, 0.70, 0.98),
           "left wall": (0.02, 0.20, 0.20, 0.40), "view": (0.0, 0.0, 1.0, 1.0)}


def main():
    base, frame, ref = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    mode = np.rint(mm.read_pfm(f"{base}_reflmode_f{frame}.pfm")[..., 0]).astype(int)
    spacing = np.rint(mm.read_pfm(f"{base}_reflmode_f{frame}.pfm")[..., 1]).astype(int)
    layer = mm.luma(mm.read_pfm(f"{base}_refl_f{frame}.pfm"))
    target = mm.luma(mm.read_pfm(f"{base}_refl_f{ref}.pfm"))
    valid = (mm.read_pfm(f"{base}_refl_f{frame}_alpha.pfm")[..., 0] > 0) & (mm.read_pfm(f"{base}_refl_f{ref}_alpha.pfm")[..., 0] > 0)
    h, w = mode.shape
    sys.stdout.reconfigure(encoding="utf-8")
    for name, (x0, y0, x1, y1) in REGIONS.items():
        sl = (slice(int(y0 * h), int(y1 * h)), slice(int(x0 * w), int(x1 * w)))
        m, sp, l, t, v = mode[sl], spacing[sl], layer[sl], target[sl], valid[sl]
        n = m.size
        txt = f"{name}: K {100 * (m == 0).mean():.0f} %, M {100 * (m == 1).mean():.0f} %, G {100 * (m == 2).mean():.0f} %, planar {100 * (m == 3).mean():.0f} %"
        g = m == 2
        if g.any():
            txt += "; G spacing " + " ".join(f"{1 << k}px:{100 * (sp[g] == k).mean():.0f}%" for k in range(4))
        for k, label in ((1, "M"), (2, "G")):
            sel = (m == k) & v
            if sel.sum() > 100:
                txt += f"; {label}: level {l[sel].mean() / max(t[sel].mean(), 1e-9):.2f}, rms/mean {np.sqrt(((l[sel] - t[sel]) ** 2).mean()) / max(t[sel].mean(), 1e-9):.2f}"
        print(txt)


if __name__ == "__main__":
    main()

"""The surface cache's state at the reflection hits (captures with reflection.lumen_surface_cache_view = true and the
pipeline's denoisers off: the reflection layer's r = 16 where the hit found no lit cell, g = the cell's frames, b = live
cells / 65536):   python Results/Local/Refl/sc_view.py TAG CASE_MODE_RES frame [frame ...]"""
import os, sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402

tag, name = sys.argv[1:3]
sys.stdout.reconfigure(encoding="utf-8")
for f in sys.argv[3:]:
    base = os.path.join(ROOT, "Cache", "ReflJudge", tag, name)
    a = mm.read_pfm(f"{base}_refl_f{f}.pfm")
    m = np.rint(mm.read_pfm(f"{base}_reflmode_f{f}.pfm")[..., 0]).astype(int)
    traced = m == 1
    b = a[..., 2][traced]
    if b.size == 0:
        print(f"f{f}: no traced pixels")
        continue
    live = float(np.median(b))
    sel = traced & (np.abs(a[..., 2] - live) <= 1e-3 * live + 1e-6) & ((np.abs(a[..., 0]) < 0.1) | (np.abs(a[..., 0] - 16) < 0.1))
    r, g = a[..., 0][sel], a[..., 1][sel]
    lit = r < 8
    frames = g[lit]
    print(f"f{f}: live cells {live * 65536:.0f}; traced pixels {traced.sum()}, surface hits with the view {sel.sum()} ({100 * sel.sum() / traced.sum():.0f} %); "
          f"hits on a lit cell {100 * lit.mean():.1f} %; their cells' frames: mean {frames.mean() if frames.size else 0:.1f}, "
          f"< 4 frames {100 * (frames < 4).mean() if frames.size else 0:.0f} %, >= 12 frames {100 * (frames >= 12).mean() if frames.size else 0:.0f} %")

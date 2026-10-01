"""One table over the post-game tags (after_game.sh): the reflection layer (view.reflection, internal resolution) of each
tag's frames 0 / 3 / 15 against its own frame 299, per region of the bath hall view, and against the reference tag's
frame 299 (energy):
    python Results/Local/Refl/summary.py --tags A0,A,B,C,P,PC [--case bath_hall_still_1080] [--ref A0] [--out FILE.md]
Per tag: layer sigma (8 x 8 tiles' high-pass sd / mean, P95: motion_metrics.layer_sigma) at f0 / f3 / f15 / f299, and per
region 'level, rms' = mean(frame) / mean(own f299) and rms(frame - own f299) / mean(own f299); 'vs ref' = mean(own f299)
/ mean(ref f299) over the region. Final image: tile P95 after scaling to the f299 mean luminance.
"""
import argparse, os, sys

import numpy as np

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402

REGIONS = {"기둥": (0.66, 0.20, 0.74, 0.50), "뒷벽": (0.80, 0.25, 0.99, 0.45), "바닥": (0.30, 0.75, 0.70, 0.98), "왼쪽 벽": (0.02, 0.20, 0.20, 0.40)}
FRAMES = (0, 3, 15, 299)


def load(raw, tag, case, frame, layer):
    base = os.path.join(raw, tag, case + (f"_{layer}" if layer else "") + f"_f{frame}")
    if not os.path.exists(base + ".pfm"):
        return None, None
    img = mm.read_pfm(base + ".pfm")
    a = base + "_alpha.pfm"
    return img, (mm.read_pfm(a)[..., 0] > 0 if os.path.exists(a) else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", required=True)
    ap.add_argument("--case", default="bath_hall_still_1080")
    ap.add_argument("--ref", default="")
    ap.add_argument("--raw", default=os.path.join(ROOT, "Cache", "ReflJudge"))
    ap.add_argument("--out", default="")
    a = ap.parse_args()
    tags = a.tags.split(",")
    ref_layer, ref_mask = (load(a.raw, a.ref, a.case, 299, "refl") if a.ref else (None, None))
    lines = ["| 판 | 반사 층 σ f0 / f3 / f15 / f299 | 최종 타일 P95 f0 / f3 / f15 | " + " | ".join(f"{r} (수준, rms) f0 → f3 → f15; 기준 대비" for r in REGIONS) + " |",
             "|---|---|---|" + "---|" * len(REGIONS)]
    for tag in tags:
        layers = {f: load(a.raw, tag, a.case, f, "refl") for f in FRAMES}
        finals = {f: load(a.raw, tag, a.case, f, None)[0] for f in FRAMES}
        if layers[299][0] is None:
            lines.append(f"| {tag} | (캡처 없음) |")
            continue
        sig = " / ".join("—" if layers[f][0] is None else f"{mm.layer_sigma(layers[f][0], layers[f][1]) * 100:.0f} %" for f in FRAMES)
        end = finals[299]
        key = mm.display_key(end)
        tp = []
        for f in FRAMES[:-1]:
            if finals[f] is None:
                tp.append("—")
                continue
            norm = float(mm.luma(end).mean()) / max(float(mm.luma(finals[f]).mean()), 1e-12)
            tp.append(f"{mm.tile_p95(finals[f] * norm, end, key)[0] * 100:.0f} %")
        l299, m299 = mm.luma(layers[299][0]), layers[299][1]
        h, w = l299.shape
        cells = []
        for name, (x0, y0, x1, y1) in REGIONS.items():
            sl = (slice(int(y0 * h), int(y1 * h)), slice(int(x0 * w), int(x1 * w)))
            parts = []
            for f in FRAMES[:-1]:
                if layers[f][0] is None:
                    parts.append("—")
                    continue
                lf = mm.luma(layers[f][0])[sl]
                m = (layers[f][1][sl] if layers[f][1] is not None else True) & (m299[sl] if m299 is not None else True)
                x, r = lf[m], l299[sl][m]
                parts.append(f"{x.mean() / max(r.mean(), 1e-9):.2f}, {np.sqrt(((x - r) ** 2).mean()) / max(r.mean(), 1e-9):.2f}")
            cell = " → ".join(parts)
            if ref_layer is not None and ref_layer.shape == layers[299][0].shape:
                m = (m299[sl] if m299 is not None else True) & (ref_mask[sl] if ref_mask is not None else True)
                cell += f"; {l299[sl][m].mean() / max(mm.luma(ref_layer)[sl][m].mean(), 1e-9):.2f}"
            cells.append(cell)
        lines.append(f"| {tag} | {sig} | {' / '.join(tp)} | " + " | ".join(cells) + " |")
    text = "\n".join(lines) + "\n"
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text)
    sys.stdout.reconfigure(encoding="utf-8")
    print(text)


if __name__ == "__main__":
    main()

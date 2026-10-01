"""Metrics and eye-judgement images of the S2 judge runs (judge_run.py), RENDERER_REDESIGN_V2 3.1 metrics
(Tools/Verify/motion_metrics.py):
    python Results/Local/Refl/judge_analyze.py --tags A,B --res 1080 [--cases ...] [--modes still,rot,cut] --out Results/Local/Refl/cmp_AB
Per case, mode and tag: each captured frame against the run's last captured frame (the converged still image of that
pose for 'still' and 'cut'; for 'rot' the last frame is only another moving frame, so the moving frames get the layer
sigma and no error against a reference):
  tile P95   final image, 16 x 16 tiles, after scaling the frame to the reference's mean luminance (auto exposure is still
             settling in the first frames: the judge2 'norm' convention)
  pattern    grid / comb index of the final image against the reference
  refl sigma the reflection layer's high-pass noise (view.reflection: lobe-normalised incident radiance, 8 x 8 tiles, P95)
             over the pixels with a value, for the frame and for the reference
  refl err   the reflection layer's tile error P95 against the reference
Images: <out>/<case>_<mode>_<res>_strip.png (one row per tag: the frames at the reference's exposure, whole view) and
_crop.png (1:1 crops at the brightest-reflection region), _refl.png (the reflection layer).
"""
import argparse, json, os, sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "Verify"))
import motion_metrics as mm  # noqa: E402

FRAMES = {"still": [0, 3, 15, 299], "rot": [59, 63, 75, 120, 179], "cut": [60, 63, 75, 359], "diag": [3, 15]}


def tonemap(x):
    x = np.nan_to_num(np.maximum(x, 0.0), nan=0.0, posinf=64.0, neginf=0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14
    y = np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)
    y = np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(y, 1 / 2.4) - 0.055)
    return (y * 255 + 0.5).astype(np.uint8)


def load(raw, tag, name, frame, layer=None):
    p = os.path.join(raw, tag, name + (f"_{layer}" if layer else "") + f"_f{frame}.pfm")
    return mm.read_pfm(p) if os.path.exists(p) else None


def mask_of(raw, tag, name, frame, layer):
    p = os.path.join(raw, tag, name + f"_{layer}_f{frame}_alpha.pfm")
    return (mm.read_pfm(p)[..., 0] > 0) if os.path.exists(p) else None


def label(img, text):
    im = Image.fromarray(img)
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 8 + 7 * len(text), 14], fill=(0, 0, 0))
    d.text((3, 1), text, fill=(255, 255, 0))
    return im


def rows_image(rows, gap=4):
    w = max(sum(i.width for i in r) + gap * (len(r) - 1) for r in rows)
    h = sum(max(i.height for i in r) for r in rows) + gap * (len(rows) - 1)
    out = Image.new("RGB", (w, h), (255, 0, 255))
    y = 0
    for r in rows:
        x = 0
        for i in r:
            out.paste(i, (x, y))
            x += i.width + gap
        y += max(i.height for i in r) + gap
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tags", required=True)
    ap.add_argument("--res", default="1080")
    ap.add_argument("--cases", default="bath_lounge,bath_hall,train_lounge")
    ap.add_argument("--modes", default="still,rot")
    ap.add_argument("--out", required=True)
    ap.add_argument("--raw", default=os.path.join(ROOT, "Cache", "ReflJudge"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tags = a.tags.split(",")
    report, lines = {}, []
    for case in a.cases.split(","):
        for mode in a.modes.split(","):
            name = f"{case}_{mode}_{a.res}"
            frames = FRAMES[mode]
            strips, crops, refls = [], [], []
            crop_at = None
            for tag in tags:
                ref = load(a.raw, tag, name, frames[-1])
                if ref is None:
                    lines.append(f"- {name} [{tag}]: no captures")
                    continue
                key = mm.display_key(ref)
                ref_mean = float(mm.luma(ref).mean())
                ref_layer = load(a.raw, tag, name, frames[-1], "refl")
                ref_mask = mask_of(a.raw, tag, name, frames[-1], "refl")
                if crop_at is None:
                    # the 1:1 crop where the reflection layer carries the most light (shared by the tags)
                    h, w = ref.shape[:2]
                    cw, ch = min(480, w), min(360, h)
                    if ref_layer is not None:
                        s = ref.shape[0] / ref_layer.shape[0]
                        l = mm.luma(ref_layer) * (ref_mask if ref_mask is not None else 1)
                        t = mm.tiles(l, 16).mean(axis=(1, 3))
                        ty, tx = np.unravel_index(np.argmax(np.clip(t, 0, np.percentile(t, 99))), t.shape)
                        cx, cy = int((tx * 16 + 8) * s), int((ty * 16 + 8) * s)
                    else:
                        cx, cy = w // 2, h // 2
                    crop_at = (min(max(cx - cw // 2, 0), w - cw), min(max(cy - ch // 2, 0), h - ch), cw, ch)
                srow, crow, rrow, txt = [], [], [], []
                for f in frames:
                    img = load(a.raw, tag, name, f)
                    if img is None:
                        continue
                    norm = ref_mean / max(float(mm.luma(img).mean()), 1e-12)
                    shown = tonemap(img * norm * key)
                    x0, y0, cw, ch = crop_at
                    hh, ww = shown.shape[:2]
                    srow.append(label(np.asarray(Image.fromarray(shown).resize((640, round(hh * 640 / ww)), Image.LANCZOS)), f"{tag} f{f} x{norm:.2f}"))
                    crow.append(label(shown[y0:y0 + ch, x0:x0 + cw], f"{tag} f{f}"))
                    entry = {"norm": norm}
                    still_ref = mode != "rot" and f != frames[-1]
                    if still_ref:
                        p95, mean = mm.tile_p95(img * norm, ref, key)
                        pat, where = mm.pattern(img * norm, ref, key=key)
                        entry.update(tile_p95=p95, mean_err=mean, pattern=pat)
                    layer = load(a.raw, tag, name, f, "refl")
                    if layer is not None:
                        m = mask_of(a.raw, tag, name, f, "refl")
                        entry["refl_sigma"] = mm.layer_sigma(layer, m)
                        if still_ref and ref_layer is not None and layer.shape == ref_layer.shape:
                            both = m & ref_mask if (m is not None and ref_mask is not None) else None
                            # the layer is radiance / exposure-free; auto exposure does not scale it
                            entry["refl_err"] = mm.layer_error(layer, ref_layer, both)
                        kl = mm.luma(ref_layer if ref_layer is not None else layer)
                        km = kl > 0 if ref_mask is None or ref_layer is None else (ref_mask & (kl > 0))
                        lk = 0.18 / max(float(np.median(kl[km])) if km.any() else 1.0, 1e-9)  # the median of the pixels with a value
                        lh, lw = layer.shape[:2]
                        rrow.append(label(np.asarray(Image.fromarray(tonemap(layer * lk)).resize((640, round(lh * 640 / lw)), Image.LANCZOS)), f"{tag} refl f{f}"))
                    report.setdefault(name, {}).setdefault(tag, {})[f] = entry
                    t = f"f{f}:"
                    if "tile_p95" in entry:
                        t += f" 타일 P95 {entry['tile_p95'] * 100:.1f} %, 무늬 {entry['pattern']:.2f},"
                    if "refl_sigma" in entry:
                        t += f" 반사 층 σ {entry['refl_sigma'] * 100:.1f} %"
                    if "refl_err" in entry:
                        t += f", 층 오차 P95 {entry['refl_err'] * 100:.1f} %"
                    txt.append(t)
                lines.append(f"- {name} [{tag}]: " + "; ".join(txt))
                if srow:
                    strips.append(srow)
                    crops.append(crow)
                if rrow:
                    refls.append(rrow)
            if strips:
                rows_image(strips).save(os.path.join(a.out, name + "_strip.png"))
                rows_image(crops).save(os.path.join(a.out, name + "_crop.png"))
            if refls:
                rows_image(refls).save(os.path.join(a.out, name + "_refl.png"))
    with open(os.path.join(a.out, f"metrics_{a.res}.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    with open(os.path.join(a.out, f"metrics_{a.res}.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

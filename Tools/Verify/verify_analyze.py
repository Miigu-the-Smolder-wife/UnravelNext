"""Analysis and report for Tools/Verify/Verify-CloudBranch.ps1 (local verification of a cloud branch).

Usage: python verify_analyze.py OUT_DIR GATE_DIR
Reads what the PowerShell script left in OUT_DIR and writes OUT_DIR/SUMMARY_KO.md plus comparison PNGs:
  steps.csv                                 name,exit,d3d12msgs,seconds of every run
  caps/<scene>_<res>_<mode>_native.pfm      renderergate --capture (native resolution)
  caps/<scene>_<res>_<mode>_up.pfm          renderergate --capture-output (upscaled output)
  det/<scene>_<variant><n>.pfm              determinism pairs (variant det = debug.deterministic, def = default)
  lum/<name>.csv                            --luminance-log curves ("frame,mean")
  timing/<scene>_<res>_<round>/**/*.json    renderergate --out results
Numbers from the machine are measured values; the report labels them [실측].
"""
import csv, glob, json, os, re, subprocess, sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import motion_metrics as mm  # noqa: E402  (RENDERER_REDESIGN_V2 3.1 metrics)


def read_pfm(path):
    with open(path, "rb") as f:
        kind = f.readline().strip()
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        ch = 3 if kind == b"PF" else 1
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4").reshape(h, w, ch)
    data = np.flipud(data)
    return data[..., :3] if ch == 3 else np.repeat(data, 3, axis=2)


def tonemap(x):
    x = np.nan_to_num(np.maximum(x, 0.0), nan=0.0, posinf=64.0, neginf=0.0)
    a, b, c, d, e = 2.51, 0.03, 2.43, 0.59, 0.14  # ACES fitted (Narkowicz)
    y = np.clip((x * (a * x + b)) / (x * (c * x + d) + e), 0, 1)
    y = np.where(y <= 0.0031308, 12.92 * y, 1.055 * np.power(y, 1 / 2.4) - 0.055)
    return (y * 255 + 0.5).astype(np.uint8)


def display_key(img):
    lum = np.nan_to_num(img @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32), nan=0.0, posinf=0.0, neginf=0.0)
    return 0.18 / float(np.exp(np.mean(np.log(np.maximum(lum, 0) + 1e-4))))


def side_by_side(native, up, out_png, crop_png):
    """Whole frames side by side (960 px each) and a 1:1 centre crop (640 x 720 each); one exposure from the native."""
    key = display_key(native)
    a, b = tonemap(native * key), tonemap(up * key)
    h, w = a.shape[:2]
    vw = 960
    halves = [Image.fromarray(i).resize((vw, round(h * vw / w)), Image.LANCZOS) for i in (a, b)]
    s = Image.new("RGB", (vw * 2 + 8, halves[0].height), (255, 0, 255))
    s.paste(halves[0], (0, 0)); s.paste(halves[1], (vw + 8, 0)); s.save(out_png)
    cw, ch = min(640, w), min(720, h)
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    c = Image.new("RGB", (cw * 2 + 8, ch), (255, 0, 255))
    c.paste(Image.fromarray(a[y0:y0 + ch, x0:x0 + cw]), (0, 0)); c.paste(Image.fromarray(b[y0:y0 + ch, x0:x0 + cw]), (cw + 8, 0))
    c.save(crop_png)


def bit_compare(p, q):
    a, b = read_pfm(p).astype(np.float64), read_pfm(q).astype(np.float64)
    if a.shape != b.shape:
        return "크기 다름"
    if np.array_equal(a, b):
        return "**비트 동일**"
    d = np.abs(a - b)
    mean_rel = abs(a.mean() - b.mean()) / max(abs(b.mean()), 1e-12) * 100
    return f"다름: 평균 밝기 차 {mean_rel:.3f} %, 평균 |차| {d.mean():.4g}, 다른 화소 {(d > 0).any(axis=2).sum()}"


def luminance(csv_path, png_path, warmup_hint):
    frames, values = [], []
    with open(csv_path, newline="") as f:
        for row in csv.reader(f):
            try:
                frames.append(int(row[0])); values.append(float(row[1]))
            except (ValueError, IndexError):
                continue
    v = np.array(values)
    if len(v) < 10:
        return "기록 없음"
    start = min(warmup_hint, len(v) // 2)
    tail = v[start:]
    mean = tail.mean()
    pp = (tail.max() - tail.min()) / mean * 100
    std = tail.std() / mean * 100
    spec = np.abs(np.fft.rfft(tail - mean))
    k = int(np.argmax(spec[1:]) + 1) if len(spec) > 2 else 0
    period = len(tail) / k if k else float("nan")
    # plot
    W, H = 1200, 360
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    lo, hi = v.min(), v.max()
    rng = max(hi - lo, 1e-9)
    pts = [(int(i / (len(v) - 1) * (W - 20)) + 10, int(H - 20 - (x - lo) / rng * (H - 40))) for i, x in enumerate(v)]
    d.line(pts, fill=(30, 90, 200), width=1)
    xs = int(start / (len(v) - 1) * (W - 20)) + 10
    d.line([(xs, 10), (xs, H - 10)], fill=(200, 60, 60), width=1)
    d.text((12, 4), f"mean luminance per frame; red = analysis start (frame {start}); min {lo:.5g} max {hi:.5g}", fill=(0, 0, 0))
    img.save(png_path)
    return f"프레임 {len(v)}개, 분석 구간(프레임 {start}~) 평균 {mean:.5g}, 최대−최소 {pp:.2f} %, 표준편차 {std:.2f} %, 주된 주기 약 {period:.0f} 프레임"


def upscale_compare(gate, native, up, crop):
    tool = os.path.join(gate, "Tools", "ImageQuality", "UpscaleCompare.py")
    if not os.path.exists(tool):
        return None
    out_json = up.replace(".pfm", "_cmp.json")
    args = [sys.executable, tool, native, up, "--json", out_json]
    if crop:
        args += ["--crop", crop]
    r = subprocess.run(args, capture_output=True, text=True)
    if not os.path.exists(out_json):  # (the tool's exit code may mark "outside the gate"; the JSON is what counts)
        return {"error": (r.stderr or r.stdout)[-300:]}
    return json.load(open(out_json, encoding="utf-8"))


def fmt_cmp(c):
    if c is None:
        return "비교 도구 없음"
    if "error" in c:
        return "비교 실패: " + c["error"].replace("\n", " ")
    whole = c.get("whole", c)
    def g(k):
        v = whole.get(k)
        return f"{v:.3f}" if isinstance(v, (int, float)) else "?"
    ok = ""
    if isinstance(whole.get("hf_ratio"), (int, float)) and isinstance(whole.get("ssim"), (int, float)):
        ok = " → 후보 기준 " + ("통과" if 0.95 <= whole["hf_ratio"] <= 1.05 and whole["ssim"] >= 0.99 else "**불합격**")
    return f"hf_ratio {g('hf_ratio')}, ssim {g('ssim')}, edge_ratio {g('edge_ratio')}, mean_diff {g('mean_diff')}{ok}"


def timing(out):
    runs = {}
    for f in glob.glob(os.path.join(out, "timing", "*", "**", "*.json"), recursive=True):
        tag = os.path.relpath(f, os.path.join(out, "timing")).split(os.sep)[0]
        runs[tag] = json.load(open(f, encoding="utf-8"))
    groups = {}
    for tag, d in runs.items():
        scene, res, rnd = tag.rsplit("_", 2)
        groups.setdefault((scene, res), []).append(d)
    lines = ["| 장면 | 출력 | GPU 프레임 중앙값 (회차별) | p95 | 경합 초 | m.upscale |", "|---|---|---|---|---|---|"]
    detail = []
    for (scene, res) in sorted(groups):
        ds = groups[(scene, res)]
        g = "/".join(f"{d['gpu_frame_ms']['median']:.2f}" for d in ds)
        p = "/".join(f"{d['gpu_frame_ms']['p95']:.2f}" for d in ds)
        c = "/".join(str(d["gpu_contention"]["contended_seconds"]) for d in ds)
        up = "/".join(f"{d['passes'].get('m.upscale', {}).get('median', 0):.2f}" for d in ds)
        lines.append(f"| {scene} | {res} | {g} | {p} | {c} | {up} |")
        passes = {}
        for d in ds:
            for k, v in d["passes"].items():
                passes.setdefault(k, []).append(v["median"])
        top = sorted(((k, float(np.median(v))) for k, v in passes.items()), key=lambda x: -x[1])[:15]
        detail.append(f"- {scene} {res}: " + ", ".join(f"{k} {v:.2f}" for k, v in top))
    return lines, detail


def mapped_luma(img, key):
    lum = np.nan_to_num(img @ np.array([0.2126, 0.7152, 0.0722], dtype=np.float32), nan=0.0, posinf=0.0, neginf=0.0) * key
    lum = np.maximum(lum, 0)
    return lum / (1 + lum)


def convergence(out):
    """Cold-start curves: conv/<scene>_<res>_<k>.pfm against conv/<scene>_<res>_ref.pfm. Error on L/(1+L) at the reference's
    display exposure: mean relative error and the P95 of 16x16 tile errors; frames to clean = the first k whose tile P95
    is at most 3 % (a candidate threshold, not a replacement for looking at conv/strip_*.png)."""
    lines, groups = [], {}
    for f in glob.glob(os.path.join(out, "conv", "*.pfm")):
        base = os.path.basename(f)[:-4]
        scene_res, k = base.rsplit("_", 1)
        groups.setdefault(scene_res, {})[k] = f
    for scene_res in sorted(groups):
        g = groups[scene_res]
        if "ref" not in g:
            continue
        ref = read_pfm(g["ref"])
        key = display_key(ref)
        mr = mapped_luma(ref, key)
        h, w = mr.shape
        th, tw = h // 16, w // 16
        ks = sorted((int(k) for k in g if k != "ref"))
        row, clean = [], None
        strip = []
        cw, ch = min(480, w), min(270, h)
        x0, y0 = (w - cw) // 2, (h - ch) // 2
        for k in ks:
            img = read_pfm(g[str(k)])
            mk = mapped_luma(img, key)
            d = np.abs(mk - mr)
            rel = d.mean() / max(mr.mean(), 1e-9)
            tiles = d[:th * 16, :tw * 16].reshape(th, 16, tw, 16).mean(axis=(1, 3)) / np.maximum(mr[:th * 16, :tw * 16].reshape(th, 16, tw, 16).mean(axis=(1, 3)), 1e-3)
            p95 = float(np.percentile(tiles, 95))
            row.append(f"{k}프레임: 평균 {rel * 100:.1f} %, 타일 P95 {p95 * 100:.1f} %")
            if clean is None and p95 <= 0.03:
                clean = k
            strip.append(tonemap(img * key)[y0:y0 + ch, x0:x0 + cw])
        strip.append(tonemap(ref * key)[y0:y0 + ch, x0:x0 + cw])
        s = Image.new("RGB", (len(strip) * (cw + 4), ch), (255, 0, 255))
        for i, a in enumerate(strip):
            s.paste(Image.fromarray(a), (i * (cw + 4), 0))
        s.save(os.path.join(out, "conv", f"strip_{scene_res}.png"))
        lines.append(f"- {scene_res}: " + "; ".join(row) + f" → 타일 P95 ≤ 3 %에 닿는 프레임: {clean if clean is not None else '측정 범위 밖(더 늦음)'} (`conv/strip_{scene_res}.png`: 왼쪽부터 {', '.join(str(k) for k in ks)}프레임, 마지막이 수렴)")
    return lines


def _plan(out, phase):
    p = os.path.join(out, phase, "plan.json")
    return json.load(open(p, encoding="utf-8-sig")) if os.path.exists(p) else None


def _layer_mask(path):
    a = path[:-4] + "_alpha.pfm"
    return (mm.read_pfm(a)[..., 0] > 0.5) if os.path.exists(a) else None


def _metrics(test, ref, layers=("gi", "refl")):
    """final image and the internal layers of one capture against its reference (same pose, converged)."""
    r = mm.compare(test, ref)
    txt = f"타일 P95 {r['tile_p95'] * 100:.1f} %, 무늬 {r['pattern']:.2f}" + (f" ({r['pattern_at']})" if r["pattern"] > 1.1 else "")
    for layer in layers:
        # renderergate names a layer's capture <base>_<layer>_f<frame>.pfm beside <base>_f<frame>.pfm
        lt, lr = (re.sub(r"_f(\d+)\.pfm$", rf"_{layer}_f\1.pfm", p) for p in (test, ref))
        if lt == test or lr == ref or not os.path.exists(lt) or not os.path.exists(lr):
            continue
        a, b = mm.read_pfm(lt), mm.read_pfm(lr)
        mask = _layer_mask(lr)
        txt += f"; {layer} 층 σ {mm.layer_sigma(a, mask) * 100:.1f} % (기준 {mm.layer_sigma(b, mask) * 100:.1f} %), 층 오차 P95 {mm.layer_error(a, b, mask) * 100:.1f} %"
    return r, txt


def _strip(paths, out_png, key_from):
    ref = mm.read_pfm(key_from)
    key = mm.display_key(ref)
    imgs = [tonemap(mm.read_pfm(p) * key) for p in paths]
    h, w = imgs[0].shape[:2]
    cw, ch = min(480, w), min(270, h)
    x0, y0 = (w - cw) // 2, (h - ch) // 2
    s = Image.new("RGB", (len(imgs) * (cw + 4), ch), (255, 0, 255))
    for i, a in enumerate(imgs):
        s.paste(Image.fromarray(a[y0:y0 + ch, x0:x0 + cw]), (i * (cw + 4), 0))
    s.save(out_png)


def _frame_log_summary(csv_path, first):
    if not os.path.exists(csv_path):
        return ""
    rows = list(csv.DictReader(open(csv_path, encoding="utf-8")))
    rows = [r for r in rows if int(r["frame"]) >= first]
    if not rows:
        return ""
    d = [float(r["d"]) for r in rows]
    created = [int(r["gi_created"] or 0) for r in rows]
    resets = [int(r["gi_resets"] or 0) for r in rows]
    return f"d 평균 {np.mean(d) * 100:.2f} % 최대 {np.max(d) * 100:.2f} %, GI 새 항목/프레임 평균 {np.mean(created):.0f} 최대 {np.max(created)}, 재시작 평균 {np.mean(resets):.0f}"


def motion(out):
    plan = _plan(out, "motion")
    if not plan:
        return []
    warm, still, ks, ref_ks = plan["warm"], plan["still"], list(plan["ks"]), list(plan["refKs"])
    lines = []
    for log in sorted(glob.glob(os.path.join(out, "motion", "*_frames.csv"))):
        base = log[:-len("_frames.csv")]
        name = os.path.basename(base)
        rows = []
        for j, k in enumerate(ref_ks):
            test, ref = f"{base}_f{warm + k}.pfm", f"{base}_ref_f{(j + 1) * still - 1}.pfm"
            if os.path.exists(test) and os.path.exists(ref):
                _, txt = _metrics(test, ref)
                rows.append(f"{k}프레임: {txt}")
        caps = [f"{base}_f{warm + k}.pfm" for k in ks if os.path.exists(f"{base}_f{warm + k}.pfm")]
        last_ref = f"{base}_ref_f{len(ref_ks) * still - 1}.pfm"
        if caps and os.path.exists(last_ref):
            _strip(caps + [last_ref], os.path.join(out, "motion", f"strip_{name}.png"), last_ref)
        lines.append(f"- {name}: " + " / ".join(rows) + f" [{_frame_log_summary(log, warm)}] (`motion/strip_{name}.png`: 움직임 {', '.join(str(k) for k in ks)}프레임, 마지막이 {ref_ks[-1]}프레임 시각의 정지 수렴)")
    for first in sorted(glob.glob(os.path.join(out, "motion", "*_still_f*.pfm"))):
        if not first.endswith(f"_f{still}.pfm"):
            continue
        frames = [first.replace(f"_f{still}.pfm", f"_f{still + i}.pfm") for i in range(8)]
        if all(os.path.exists(f) for f in frames):
            fl = mm.flicker([mm.read_pfm(f) for f in frames])
            lines.append(f"- {os.path.basename(first).replace(f'_f{still}.pfm', '')} 정지 깜빡임 (10비트 휘도 σ, {still}프레임 뒤 8프레임): 평균 {fl['mean']:.2f}, p50 {fl['p50']:.2f}, p99 {fl['p99']:.2f}, 최대 {fl['max']:.1f}")
    return lines


def relight(out):
    plan = _plan(out, "relight")
    if not plan:
        return []
    warm, still, ks = plan["warm"], plan["still"], list(plan["ks"])
    lines = []
    for log in sorted(glob.glob(os.path.join(out, "relight", "*_frames.csv"))):
        base = log[:-len("_frames.csv")]
        name = os.path.basename(base)
        ref = f"{base}_ref_f{still - 1}.pfm"
        if not os.path.exists(ref):
            continue
        rows = []
        before = f"{base}_f{warm - 1}.pfm"
        if os.path.exists(before):
            r = mm.compare(before, ref)
            rows.append(f"변화 전(변화의 크기) 타일 P95 {r['tile_p95'] * 100:.1f} %")
        caps = []
        for k in ks:
            test = f"{base}_f{warm + k - 1}.pfm"
            if os.path.exists(test):
                _, txt = _metrics(test, ref)
                rows.append(f"{k}프레임: {txt}")
                caps.append(test)
        if caps:
            _strip(caps + [ref], os.path.join(out, "relight", f"strip_{name}.png"), ref)
        lines.append(f"- {name}: " + " / ".join(rows) + f" (`relight/strip_{name}.png`: {', '.join(str(k) for k in ks)}프레임, 마지막이 변화 뒤 정지 수렴)")
    return lines


def cut(out):
    plan = _plan(out, "cut")
    if not plan:
        return []
    warm, still, ks = plan["warm"], plan["still"], list(plan["ks"])
    lines = []
    for log in sorted(glob.glob(os.path.join(out, "cut", "*_frames.csv"))):
        base = log[:-len("_frames.csv")]
        name = os.path.basename(base)
        ref1, ref2 = f"{base}_ref1_f{still - 1}.pfm", f"{base}_ref2_f{still - 1}.pfm"
        if not os.path.exists(ref1):
            continue
        rows = []
        if os.path.exists(ref2):
            r = mm.compare(ref2, ref1)
            rows.append(f"기준 잡음 바닥(기준 두 번의 차) 타일 P95 {r['tile_p95'] * 100:.1f} %")
        caps = []
        for k in ks:
            test = f"{base}_f{warm + k - 1}.pfm"
            if os.path.exists(test):
                _, txt = _metrics(test, ref1)
                rows.append(f"{k}프레임: {txt}")
                caps.append(test)
        if caps:
            _strip(caps + [ref1], os.path.join(out, "cut", f"strip_{name}.png"), ref1)
        gi = []
        for tag, js in (("컷 뒤 마지막 프레임", f"{base}_gi.json"), ("정지 수렴", f"{base}_ref_gi.json")):
            if os.path.exists(js):
                g = json.load(open(js, encoding="utf-8"))
                l1 = g.get("gi_parent_l1", {})
                gi.append(f"{tag}: 살아 있는 항목 {g.get('gi_cache_live')}, 16회 이상 {g.get('gi_cache_converged16')}, 지난 프레임에 읽힌 항목 {g.get('gi_cache_read_last_frame')} 중 갱신 < 4회 {g.get('gi_cache_read_young4')}·< 16회 {g.get('gi_cache_read_young16')}; "
                          f"부모(l+1)–자식 수렴값 차 {l1.get('pairs')}쌍 p50 {l1.get('p50', 0) * 100:.1f} % p95 {l1.get('p95', 0) * 100:.1f} %, ≤ 3 % {l1.get('share_le_3pct', 0) * 100:.0f} %")
        lines.append(f"- {name}: " + " / ".join(rows) + f" [{_frame_log_summary(log, warm)}] (`cut/strip_{name}.png`: 컷 뒤 {', '.join(str(k) for k in ks)}프레임, 마지막이 정지 수렴)")
        lines += [f"  - GI 캐시 {x}" for x in gi]
    return lines


def unity(out):
    lines = []
    for size_dir in sorted(glob.glob(os.path.join(out, "unity", "*"))):
        if not os.path.isdir(size_dir):
            continue
        size = os.path.basename(size_dir)
        rep = os.path.join(size_dir, "play_report.txt")
        if os.path.exists(rep):
            for l in open(rep, encoding="utf-8", errors="replace"):
                if "flicker" in l:
                    lines.append(f"- {size}: {l.strip()}")
        names = [n for n in ("first1", "first4", "first16", "first64", "still0", "walk3")
                 for _ in [0] if glob.glob(os.path.join(size_dir, f"play_*_{n}.png"))]
        imgs = [Image.open(glob.glob(os.path.join(size_dir, f"play_*_{n}.png"))[0]).convert("RGB") for n in names]
        if imgs:
            tw = 480
            th = round(imgs[0].height * tw / imgs[0].width)
            s = Image.new("RGB", (len(imgs) * (tw + 4), th), (255, 0, 255))
            for i, im in enumerate(imgs):
                s.paste(im.resize((tw, th), Image.LANCZOS), (i * (tw + 4), 0))
            s.save(os.path.join(out, "unity", f"strip_{size}.png"))
            lines.append(f"- {size}: `unity/strip_{size}.png` = {', '.join(names)} (전체 프레임은 `unity/{size}/`)")
        if not os.path.exists(rep) and not imgs:
            lines.append(f"- {size}: 결과 없음 (`unity_{size}.log` 확인)")
    return lines


def main():
    out, gate = sys.argv[1], sys.argv[2]
    md = ["# 로컬 검증 결과", ""]
    sha = open(os.path.join(out, "gate_sha.txt")).read().strip() if os.path.exists(os.path.join(out, "gate_sha.txt")) else "?"
    md += [f"- 빌드한 커밋: `{sha}` (게이트 작업 폴더 `{gate}`)", "- 모든 수치는 이 PC(RTX 4080)에서 잰 [실측] 값이다.", ""]
    # steps
    md += ["## 실행 기록 (종료 코드, D3D12 메시지 수)", "", "| 실행 | 종료 코드 | d3d12 메시지 | 초 |", "|---|---|---|---|"]
    fails = []
    if os.path.exists(os.path.join(out, "steps.csv")):
        for row in csv.reader(open(os.path.join(out, "steps.csv"), encoding="utf-8")):
            if len(row) >= 4:
                md.append(f"| {row[0]} | {row[1]} | {row[2]} | {row[3]} |")
                if row[1] != "0" or row[2] != "0":
                    fails.append(row[0])
    md += ["", "종료 코드가 0이 아니거나 d3d12 메시지가 있는 실행: " + (", ".join(fails) if fails else "없음"), ""]
    # upscale
    md += ["## 업스케일 (원래 해상도 대 업스케일, 같은 출력 해상도)", "",
           "비교 PNG: `caps/cmp_*.png`(전체), `caps/crop_*.png`(가운데 1:1). 왼쪽이 원래 해상도, 오른쪽이 업스케일이다. **숫자보다 눈으로 보는 판정이 먼저다.**", ""]
    for native in sorted(glob.glob(os.path.join(out, "caps", "*_native.pfm"))):
        up = native.replace("_native.pfm", "_up.pfm")
        if not os.path.exists(up):
            continue
        name = os.path.basename(native).replace("_native.pfm", "")
        side_by_side(read_pfm(native), read_pfm(up), os.path.join(out, "caps", f"cmp_{name}.png"), os.path.join(out, "caps", f"crop_{name}.png"))
        md.append(f"- {name}: {fmt_cmp(upscale_compare(gate, native, up, None))}")
    md.append("")
    # convergence and the Unity game view (user requirement 2026-09-30: clean in every frame while moving)
    conv = convergence(out)
    if conv:
        md += ["## 수렴 곡선 (장면 첫 프레임부터, 업스케일 출력) — 움직임 화질의 대리 지표", "",
               "장면을 연 직후는 가장 심한 가림 해제다(조명 편집 뒤 재빌드도 지금은 같다). 사용자 요구는 몇 프레임 안에 깨끗해지는 것이다.", ""] + conv + [""]
    for title, fn, note in (
            ("움직임 (Q-B) — 같은 경로 시각의 정지 수렴 화면 대비", motion,
             "회전 90·180°/s, 걷기 1.5 m/s, 달리기 4 m/s. 층 σ = 변조를 푼 층의 고역 잡음(기준 값은 층 자체의 무늬), 층 오차 = 수렴한 층과의 거리. 무늬 ≤ 1.1이 목표."),
            ("조명 변화 (Q-C) — 변화 뒤 정지 수렴 화면 대비", relight, "light = 국소광 하나 끔, sun = 해 10° 이동."),
            ("컷 (Q-A) — 180° 돌아선 시점으로 순간 이동, 그 시점의 정지 수렴 화면 대비", cut, "")):
        sec = fn(out)
        if sec:
            md += [f"## {title}", ""] + ([note, ""] if note else []) + sec + [""]
    uni = unity(out)
    if uni:
        md += ["## Unity 게임 뷰 (플레이 모드, 배포된 렌더러)", "",
               "first1~64 = 레벨 빌드 뒤 그 프레임, still = 가만히 선 8프레임의 깜빡임(루마 표준편차, 10-bit 코드), walk = 걷는 중. **눈으로 볼 것.**", ""] + uni + [""]
    # determinism
    md += ["## 결정론 (같은 설정 두 번)", ""]
    for p1 in sorted(glob.glob(os.path.join(out, "det", "*1.pfm"))):
        p2 = p1[:-5] + "2.pfm"
        if os.path.exists(p2):
            md.append(f"- {os.path.basename(p1)[:-5]}: {bit_compare(p1, p2)}")
    md += ["", "`det` = `debug.deterministic=true`, `def` = 기본 모드. 기본 모드의 이전 값은 평균 밝기 차 4.41 %(84e789f, 욕탕 1080p).", ""]
    # luminance
    md += ["## 밝기 흔들림 (--luminance-log)", ""]
    for c in sorted(glob.glob(os.path.join(out, "lum", "*.csv"))):
        png = c[:-4] + ".png"
        md.append(f"- {os.path.basename(c)}: {luminance(c, png, 300)} (그래프 `lum/{os.path.basename(png)}`)")
    md += ["", "수백 프레임 주기로 수 % 흔들리면 클라우드의 모델(GI 캐시의 긴 Jacobi 단계)과 맞는다.", ""]
    # timing
    lines, detail = timing(out)
    md += ["## 성능 [실측] (비동기 기본 꺼짐, 정지 카메라 600프레임, GpuLock timing)", ""] + lines + ["",
           "이전 값(84e789f, 직렬): 기차 1440p 5.33, 기차 1080p 3.79, 욕탕 1440p 6.10, 욕탕 1080p 4.40 ms.", "", "상위 패스 (ms, 회차 중앙값):", ""] + detail + [""]
    open(os.path.join(out, "SUMMARY_KO.md"), "w", encoding="utf-8").write("\n".join(md))
    print("\n".join(md))


if __name__ == "__main__":
    main()

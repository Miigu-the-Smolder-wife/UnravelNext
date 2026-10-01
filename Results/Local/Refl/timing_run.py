"""S2 timing A/B (GpuLock -Kind timing): renderergate's standard measurement (still camera, 600 frames after its warm-up)
for two or more configurations, alternated A/B/A/B, then the per-pass medians of the reflection and probe passes.
    powershell -File Tools/CI/GpuLock.ps1 -Track S2 -Kind timing -- python Results/Local/Refl/timing_run.py
        --tag tm1 --res 2560x1440 --cases bath_hall,train_lounge --configs "A:reflection.layers=false" "B:reflection.layers=true"
JSON results: Results/Local/Refl/timing/<tag>/<case>_<res>_<config>_<round>/. The table goes to stdout and timing.md there.
"""
import argparse, glob, json, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from judge_run import CASES  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--res", default="2560x1440")
    ap.add_argument("--cases", default="bath_hall,train_lounge")
    ap.add_argument("--configs", nargs="+", required=True)  # NAME:key=value,key=value
    ap.add_argument("--rounds", type=int, default=2)
    ap.add_argument("--frames", default="")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "all", "bin", "unx_gate_shadow_renderergate.exe"))
    a = ap.parse_args()
    out = os.path.join(ROOT, "Results", "Local", "Refl", "timing", a.tag)
    os.makedirs(out, exist_ok=True)
    configs = []
    for c in a.configs:
        name, _, sets = c.partition(":")
        configs.append((name, [s for s in sets.split(",") if s]))
    for case in a.cases.split(","):
        scene, camera = CASES[case]
        for rnd in range(a.rounds):
            for name, sets in configs:
                d = os.path.join(out, f"{case}_{a.res}_{name}_{rnd}")
                cmd = [a.exe, "--scene", scene, "--resolution", a.res, "--auto-exposure", "--out", d] + camera
                if a.frames:
                    cmd += ["--frames", a.frames]
                for s in sets:
                    cmd += ["--set", s]
                t0 = time.time()
                with open(d + ".log", "w", encoding="utf-8", errors="replace") as f:
                    f.write(" ".join(cmd) + "\n")
                    f.flush()
                    rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
                text = open(d + ".log", encoding="utf-8", errors="replace").read()
                tdr = any(k in text for k in ("DEVICE_HUNG", "DEVICE_REMOVED", "0x887A0006", "0x887A0005"))
                print(f"{case} {name} round {rnd}: exit {rc}, {time.time() - t0:.0f} s" + (" TDR" if tdr else ""), flush=True)
                if tdr or rc == 87:
                    sys.exit(87)
    # table
    lines = []
    for case in a.cases.split(","):
        rows = {}
        for name, _ in configs:
            for rnd in range(a.rounds):
                for f in glob.glob(os.path.join(out, f"{case}_{a.res}_{name}_{rnd}", "**", "*.json"), recursive=True):
                    rows.setdefault(name, []).append(json.load(open(f, encoding="utf-8")))
        if not rows:
            continue
        keys = sorted({k for ds in rows.values() for d in ds for k in d["passes"] if k.startswith("r.refl") or k.startswith("r.gi.probe") or k.startswith("r.gi.screen")})
        lines.append(f"### {case} {a.res}")
        lines.append("| 항목 | " + " | ".join(n for n, _ in configs) + " |")
        lines.append("|---|" + "---|" * len(configs))
        def cell(ds, fn):
            return " / ".join(f"{fn(d):.3f}" for d in ds)
        lines.append("| GPU 프레임 중앙값 | " + " | ".join(cell(rows.get(n, []), lambda d: d["gpu_frame_ms"]["median"]) for n, _ in configs) + " |")
        lines.append("| 경합 초 | " + " | ".join(" / ".join(str(d.get("gpu_contention", {}).get("contended_seconds", "?")) for d in rows.get(n, [])) for n, _ in configs) + " |")
        lines.append("| r.refl.* 합 | " + " | ".join(cell(rows.get(n, []), lambda d: sum(v["median"] for k, v in d["passes"].items() if k.startswith("r.refl"))) for n, _ in configs) + " |")
        for k in keys:
            lines.append(f"| {k} | " + " | ".join(cell(rows.get(n, []), lambda d: d["passes"].get(k, {}).get("median", 0.0)) for n, _ in configs) + " |")
    with open(os.path.join(out, "timing.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(lines))


if __name__ == "__main__":
    main()

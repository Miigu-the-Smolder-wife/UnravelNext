"""S2 judge runs (reflection / occlusion layers, RENDERER_REDESIGN_V2 P2): renderergate captures of the game judge views
(the same commands as Results/Local/Verify-0ac2900/judge2) for a configuration tag, run one after another inside one
GpuLock:
    powershell -File Tools/CI/GpuLock.ps1 -Track S2 -Kind correctness -- python Results/Local/Refl/judge_run.py
        --tag B --res 1920x1080 --cases bath_hall,train_lounge --modes still,rot,cut --set reflection.layers=true
Raw PFM captures go to Cache/ReflJudge/<tag>/ (not committed); logs to Results/Local/Refl/<tag>/. Stops at the first run
whose log shows a device removal (TDR) and exits 87.
"""
import argparse, os, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
# Frozen copies of the game projects' look scenes (Cache/ReflJudge/scenes, not committed): the projects' own files are
# rewritten whenever a game session runs its look tests (bath_reference.unxscene changed between two runs of one
# comparison on 2026-10-01 18:01: 6668 -> 8440 instances), so a comparison reads copies taken once.
SCENES = os.path.join(ROOT, "Cache", "ReflJudge", "scenes")
BATH = os.path.join(SCENES, "bt_bath_20261001_1801.unxscene")      # BathhouseTycoon Artifacts/Look/bath_reference.unxscene
LOUNGE = os.path.join(SCENES, "bt_lounge_20261001_0500.unxscene")  # BathhouseTycoon Artifacts/Look/lounge_reference.unxscene
LOBBY = os.path.join(SCENES, "bt_lobby_20261001_1802.unxscene")    # BathhouseTycoon Artifacts/Look/lobby.unxscene
TRAIN = os.path.join(SCENES, "te_lounge_20261001_0735.unxscene")   # TrainExorcist Artifacts/Look/lounge_reference.unxscene
CASES = {
    # name: (scene, extra camera arguments)
    "bath_lounge": (LOUNGE, []),
    "bath_hall": (BATH, []),
    "bath_lobby": (LOBBY, []),  # the glossy lobby floor (the user's "game impossible" view, 2026-10-01)
    "train_lounge": (TRAIN, []),
    # reflection-heavy views (the level look tests' shots): the showers' mirrors and wet floor, the train's windows
    "bath_mirror": (BATH, ["--camera-at", "-43.2,-2.93,-8.4,-45,-3.05,-8.2"]),
    "bath_showers": (BATH, ["--camera-at", "-40,-2.85,-10,-44.5,-3.2,-2"]),
    "train_window": (TRAIN, ["--camera-at", "1.8,1.55,-1.5,8,1.45,3.5"]),
}
MODES = {
    "diag": ["--frames", "16", "--capture-frames", "3,15"],  # layer diagnostics (reflection.layer_view)
    "diag0": ["--frames", "4", "--capture-frames", "0,3"],   # the first frame's layers
    "still": ["--frames", "300", "--capture-frames", "0,3,15,299"],
    "smoke": ["--frames", "8", "--capture-frames", "7"],  # a first run of new dispatches (device alive, error bits, image)
    "long": ["--frames", "1800", "--capture-frames", "15,299,899,1799"],  # convergence of slow stores (surface cache radiosity)
    "rot": ["--frames", "180", "--path-rotate", "90", "--motion-start", "60", "--capture-frames", "59,63,75,120,179"],
    # a cut to the view turned by 90 degrees at frame 60, still before and after (frames 1, 4, 16 after the cut and its converged self)
    "cut": ["--frames", "360", "--path-rotate", "90", "--path-time", "0", "--cut-at", "60:1.0", "--capture-frames", "60,63,75,359"],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--res", default="1920x1080")
    ap.add_argument("--cases", default="bath_lounge,bath_hall,train_lounge")
    ap.add_argument("--modes", default="still,rot")
    ap.add_argument("--set", action="append", default=[])
    ap.add_argument("--layers", default="final,refl")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "all", "bin", "unx_gate_shadow_renderergate.exe"))
    ap.add_argument("--extra", default="")
    a = ap.parse_args()
    short = a.res.split("x")[1]
    raw = os.path.join(ROOT, "Cache", "ReflJudge", a.tag)
    logs = os.path.join(ROOT, "Results", "Local", "Refl", a.tag)
    os.makedirs(raw, exist_ok=True)
    os.makedirs(logs, exist_ok=True)
    for case in a.cases.split(","):
        scene, camera = CASES[case]
        for mode in a.modes.split(","):
            name = f"{case}_{mode}_{short}"
            cmd = [a.exe, "--scene", scene, "--resolution", a.res, "--auto-exposure", "--warmup-frames", "0"] + camera + MODES[mode]
            cmd += ["--capture-output", os.path.join(raw, name + ".pfm"), "--capture-layers", a.layers]
            for s in a.set:
                cmd += ["--set", s]
            if a.extra:
                cmd += a.extra.split()
            log = os.path.join(logs, name + ".log")
            t0 = time.time()
            with open(log, "w", encoding="utf-8", errors="replace") as f:
                f.write(" ".join(cmd) + "\n")
                f.flush()
                rc = subprocess.call(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT)
            text = open(log, encoding="utf-8", errors="replace").read()
            tdr = any(k in text for k in ("DEVICE_HUNG", "DEVICE_REMOVED", "device removed", "0x887A0006", "0x887A0005"))
            print(f"{a.tag} {name}: exit {rc}, {time.time() - t0:.0f} s" + (" TDR" if tdr else ""), flush=True)
            if tdr or rc == 87:
                print("device removal: stopping (report to the coordination session)", flush=True)
                sys.exit(87)
    sys.exit(0)


if __name__ == "__main__":
    main()

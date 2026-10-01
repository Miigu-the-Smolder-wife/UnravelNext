"""The first frames' layers of one configuration, in one lock hold: five short runs (frames 0 and 3 captured) of
reflection.layer_view 1 stochastic, 2 stochastic', 3 residual, 4 residual', 5 albedo (LayerCompose.hlsl):
    python Results/Local/Refl/diag0.py TAG CASE [--set key=value ...]
Captures: Cache/ReflJudge/<TAG>v<view>/<CASE>_diag0_1080_refl_f{0,3}.pfm. Exit 87 at a device removal."""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    tag, case, rest = sys.argv[1], sys.argv[2], sys.argv[3:]
    for view in (1, 2, 3, 4, 5):
        rc = subprocess.call([sys.executable, os.path.join(HERE, "judge_run.py"), "--tag", f"{tag}v{view}", "--cases", case, "--modes", "diag0", "--layers", "refl",
                              "--set", "reflection.layers=true", "--set", "reflection.layer_mirror_lobe=true", "--set", f"reflection.layer_view={view}"] + rest)
        if rc != 0:
            sys.exit(rc)


if __name__ == "__main__":
    main()

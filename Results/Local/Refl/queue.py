"""Runs a list of S2 GPU pieces, each inside its own GpuLock hold (GpuLock v1.85: one hold is at most about 10 minutes;
longer batches queue again between pieces). Stops at a device removal (exit 87).
    python Results/Local/Refl/queue.py NAME [--kind correctness|timing] "piece command" "piece command" ...
Log: Results/Local/Refl/NAME.queue.log (one line per piece: exit code, seconds with the wait).
"""
import os, shlex, subprocess, sys, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def main():
    args = sys.argv[1:]
    name = args.pop(0)
    kind = "correctness"
    if args and args[0] == "--kind":
        kind = args[1]
        args = args[2:]
    log = os.path.join(ROOT, "Results", "Local", "Refl", name + ".queue.log")
    with open(log, "w", encoding="utf-8") as f:
        f.write(f"queue {name}: {len(args)} pieces\n")
    for piece in args:
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", os.path.join(ROOT, "Tools", "CI", "GpuLock.ps1"), "-Track", "S2", "-Kind", kind,
               "--"] + shlex.split(piece, posix=False)
        t0 = time.time()
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
        out = [l for l in (r.stdout + r.stderr).splitlines() if l.strip() and "waiting for the GPU measurement lock" not in l]
        with open(log, "a", encoding="utf-8") as f:
            f.write("\n".join(out) + f"\n== piece exit {r.returncode} after {time.time() - t0:.0f} s (wait included): {piece}\n")
        print(f"piece exit {r.returncode} after {time.time() - t0:.0f} s: {piece}", flush=True)
        if r.returncode == 87:
            print("device removal: queue stopped", flush=True)
            sys.exit(87)
    sys.exit(0)


if __name__ == "__main__":
    main()

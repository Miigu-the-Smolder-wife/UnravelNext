"""Several judge_run.py invocations inside one lock hold (a verification bundle: at most about ten minutes):
    python Results/Local/Refl/bundle.py "ARGS of judge_run" "ARGS of judge_run" ...
Stops at the first device removal (exit 87)."""
import os, shlex, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
for args in sys.argv[1:]:
    rc = subprocess.call([sys.executable, os.path.join(HERE, "judge_run.py")] + shlex.split(args.strip().strip('"'), posix=False))  # (queue.py hands the quotes through)
    if rc == 87:
        sys.exit(87)
sys.exit(0)

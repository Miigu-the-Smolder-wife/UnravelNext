"""Merge all Results/microbench_*.json into one floors table (latest value per measurement).

Usage: python Summarize.py [--out Results/FLOORS.md]
Later runs override earlier ones for the same (section, name); the run timestamp is kept per row so a
reader can trace every number to its log.
"""
import glob, json, os, sys

here = os.path.dirname(os.path.abspath(__file__))
results = os.path.join(here, "Results")
out = os.path.join(results, "FLOORS.md")
if "--out" in sys.argv:
    out = sys.argv[sys.argv.index("--out") + 1]

rows = {}
meta = {}
for path in sorted(glob.glob(os.path.join(results, "microbench_*.json"))):
    with open(path, encoding="utf-8") as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError:
            continue
    stamp = data.get("timestamp", os.path.basename(path))
    meta = {k: data.get(k) for k in ("adapter", "driver", "agility_sdk", "dxc")}
    for r in data.get("results", []):
        key = (r["section"], r["name"])
        rows[key] = (r["value"], r["unit"], r.get("note", ""), stamp)

order = ["env", "compute", "memory", "texture", "atomics", "dispatch", "raster", "coverage", "edges", "vsm", "shade", "rtas", "rays", "mpm", "pso", "async", "experiment"]
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("# Measured hardware floors (RTX 4080)\n\n")
    f.write(f"Adapter: {meta.get('adapter')}, driver {meta.get('driver')}, Agility SDK {meta.get('agility_sdk')}, DXC {meta.get('dxc')}\n\n")
    f.write("Every row is a GPU-timestamp measurement (median of repeated runs after a 1.5 s warm-up) unless the note says cpu. "
            "The run column is the JSON/log stamp the number came from.\n\n")
    f.write("| section | measurement | value | unit | note | run |\n|---|---|---:|---|---|---|\n")
    for sec in order:
        for (s, n), (v, u, note, stamp) in sorted(rows.items(), key=lambda kv: kv[0][1]):
            if s != sec:
                continue
            vs = f"{v:.4g}" if isinstance(v, float) else str(v)
            f.write(f"| {s} | {n} | {vs} | {u} | {note} | {stamp} |\n")
json.dump({"meta": meta, "rows": [{"section": s, "name": n, "value": v, "unit": u, "note": note, "run": stamp} for (s, n), (v, u, note, stamp) in rows.items()]},
          open(os.path.splitext(out)[0] + ".json", "w", encoding="utf-8"), indent=1, ensure_ascii=False)
print(f"wrote {out} ({len(rows)} rows)")

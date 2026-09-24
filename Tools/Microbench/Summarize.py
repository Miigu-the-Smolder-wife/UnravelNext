"""Merge Results/microbench_*.json into one floors table.

Usage: python Summarize.py [--since STAMP] [--out Results/FLOORS.md]

Without --since, the latest run wins for each (section, name), as in P0a.
With --since STAMP, every run at or after STAMP contributes and the table shows the median, the run count and
the min..max range, next to the P0a value (FLOORS_P0A.json) and the ratio. Single runs of ray and async
measurements scatter by about +-10 % on this machine, so gates compare medians, not single runs.
Rows listed in EXCLUDE are measurement defects kept on disk as evidence but never summarised.
"""
import glob, json, os, statistics, sys

here = os.path.dirname(os.path.abspath(__file__))
results = os.path.join(here, "Results")
out = os.path.join(results, "FLOORS.md")
since = None
args = sys.argv[1:]
if "--out" in args:
    out = args[args.index("--out") + 1]
if "--since" in args:
    since = args[args.index("--since") + 1]

# (run stamp, section) -> reason
EXCLUDE = {
    ("20260925_033921", "pso"): "compute PSO kernels used a fixed seed, so 'cold' hit the driver disk cache from P0a runs; re-measured with a per-run nonce in 20260925_034232",
}

p0a_path = os.path.join(results, "FLOORS_P0A.json")
p0a = {}
if os.path.exists(p0a_path):
    with open(p0a_path, encoding="utf-8") as f:
        p0a = {(r["section"], r["name"]): r["value"] for r in json.load(f)["rows"]}

samples = {}  # key -> list of (value, unit, note, stamp)
meta = {}
for path in sorted(glob.glob(os.path.join(results, "microbench_*.json"))):
    with open(path, encoding="utf-8") as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError:
            continue
    stamp = data.get("timestamp", os.path.basename(path))
    if since and stamp < since:
        continue
    meta = {k: data.get(k) for k in ("adapter", "driver", "agility_sdk", "dxc")}
    for r in data.get("results", []):
        if (stamp, r["section"]) in EXCLUDE:
            continue
        key = (r["section"], r["name"])
        entry = (r["value"], r["unit"], r.get("note", ""), stamp)
        if since:
            samples.setdefault(key, []).append(entry)
        else:
            samples[key] = [entry]

def fmt(v):
    return f"{v:.4g}" if isinstance(v, float) else str(v)

def cell(text):
    return str(text).replace("|", "\\|")

order = ["env", "compute", "memory", "texture", "atomics", "dispatch", "raster", "coverage", "edges", "vsm", "shade", "rtas", "rays", "mpm", "pso", "async", "experiment"]
rows_json = []
with open(out, "w", encoding="utf-8", newline="\n") as f:
    f.write("# Measured hardware floors (RTX 4080)\n\n")
    f.write(f"Adapter: {meta.get('adapter')}, driver {meta.get('driver')}, Agility SDK {meta.get('agility_sdk')}, DXC {meta.get('dxc')}\n\n")
    f.write("Every row is a GPU-timestamp measurement (median of repeated runs after a 1.5 s warm-up) unless the note says cpu. ")
    if since:
        f.write(f"Runs since {since}; value = median over runs, n = run count, range = min..max, P0a = FLOORS_P0A.json.\n\n")
        for (stamp, section), reason in EXCLUDE.items():
            f.write(f"Excluded: run {stamp} section {section}: {reason}.\n\n")
        f.write("| section | measurement | value | unit | n | range | P0a | ratio | note | runs |\n|---|---|---:|---|---:|---|---:|---:|---|---|\n")
    else:
        f.write("The run column is the JSON/log stamp the number came from.\n\n")
        f.write("| section | measurement | value | unit | note | run |\n|---|---|---:|---|---|---|\n")
    for sec in order:
        for (s, n), entries in sorted(samples.items(), key=lambda kv: kv[0][1]):
            if s != sec:
                continue
            values = [e[0] for e in entries]
            unit, note, last = entries[-1][1], entries[-1][2], entries[-1][3]
            numeric = all(isinstance(v, (int, float)) for v in values)
            value = statistics.median(values) if numeric else values[-1]
            stamps = ",".join(e[3] for e in entries)
            if since:
                rng = f"{fmt(min(values))}..{fmt(max(values))}" if numeric and len(values) > 1 else ""
                # P0a named card-foliage edge rows without the tag the census added later.
                old = p0a.get((s, n), p0a.get((s, n.replace(" [card foliage]", ""))))
                ratio = f"{value / old:.3f}" if numeric and isinstance(old, (int, float)) and old else ""
                f.write(f"| {s} | {cell(n)} | {fmt(value)} | {unit} | {len(values)} | {rng} | {fmt(old) if old is not None else ''} | {ratio} | {cell(note)} | {stamps} |\n")
            else:
                f.write(f"| {s} | {cell(n)} | {fmt(value)} | {unit} | {cell(note)} | {last} |\n")
            rows_json.append({"section": s, "name": n, "value": value, "unit": unit, "note": note, "n": len(values),
                              "min": min(values) if numeric else None, "max": max(values) if numeric else None, "runs": stamps})
with open(os.path.splitext(out)[0] + ".json", "w", encoding="utf-8") as f:
    json.dump({"meta": meta, "since": since, "rows": rows_json}, f, indent=1, ensure_ascii=False)
print(f"wrote {out} ({len(rows_json)} rows)")

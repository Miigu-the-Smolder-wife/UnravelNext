# Per-pass GPU times of a gate run (the harness' result JSON, --out DIR): the passes grouped by the first parts of
# their names, largest first, and the largest single passes.
#   python Tools/Verify/pass_times.py <result.json | directory> [--depth 2] [--top 40]
import glob
import json
import os
import sys


def main(argv):
    depth, top, paths = 2, 40, []
    i = 1
    while i < len(argv):
        if argv[i] == '--depth':
            depth = int(argv[i + 1]); i += 2
        elif argv[i] == '--top':
            top = int(argv[i + 1]); i += 2
        else:
            paths.append(argv[i]); i += 1
    for p in paths:
        files = sorted(glob.glob(os.path.join(p, '*.json'))) if os.path.isdir(p) else [p]
        for f in files:
            j = json.load(open(f, encoding='utf-8'))
            passes = j['passes']
            rows = [(q['name'], q.get('gpu_ms', q).get('median', 0.0) if isinstance(q.get('gpu_ms', 0), dict) else q.get('median_ms', q.get('gpu_ms', 0.0))) for q in passes] if isinstance(passes, list) else [(k, v.get('median', 0.0) if isinstance(v, dict) else v) for k, v in passes.items()]
            total = sum(ms for _, ms in rows)
            frame = j.get('gpu_frame_ms')
            print('%s\n  resolution %s, gpu frame %s, passes %d, sum of pass medians %.3f ms' % (f, j.get('resolution'), json.dumps(frame), len(rows), total))
            groups = {}
            for name, ms in rows:
                key = '.'.join(name.split('.')[:depth])
                g = groups.setdefault(key, [0.0, 0])
                g[0] += ms; g[1] += 1
            print('  groups (depth %d):' % depth)
            for key, (ms, n) in sorted(groups.items(), key=lambda kv: -kv[1][0])[:top]:
                print('    %-34s %8.3f ms  (%d passes)' % (key, ms, n))
            print('  largest passes:')
            for name, ms in sorted(rows, key=lambda r: -r[1])[:top]:
                print('    %-46s %8.3f ms' % (name, ms))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

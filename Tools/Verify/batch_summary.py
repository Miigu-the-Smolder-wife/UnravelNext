# The batch's summary sheet (Run-Ue6Batch.ps1): the furnace sheets, every timing run's GPU frame with its largest pass
# groups, the frames' error bits, the gates that failed.
#   python Tools/Verify/batch_summary.py <batch directory> <diag directory (Run-Ue6Still's output: batch_furnace*)>
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def groups_of(passes, depth):
    out = {}
    for name, v in passes.items():
        ms = v.get('median', 0.0) if isinstance(v, dict) else float(v)
        key = '.'.join(name.split('.')[:depth])
        g = out.setdefault(key, [0.0, 0])
        g[0] += ms
        g[1] += 1
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__ or 'usage: batch_summary.py <batch directory> <diag directory>')
        return 2
    batch, diag = argv[1], argv[2]
    print('== furnace room (measured / expected per stage; the day room over the night room = light through the walls)')
    for name in ('batch_furnace', 'batch_furnace_day', 'batch_furnace_day_noocc'):
        d = os.path.join(diag, name)
        if not os.path.isdir(d):
            continue
        print('-- ' + name)
        r = subprocess.run([sys.executable, os.path.join(HERE, 'furnace.py'), d], capture_output=True, text=True)
        print((r.stdout or '') + (r.stderr or ''))
    print('== timings: GPU frame median / p95 (ms), passes, then the largest groups (ms)')
    rows = []
    for f in sorted(glob.glob(os.path.join(batch, '**', 'timing_*', '*.json'), recursive=True)):
        j = json.load(open(f, encoding='utf-8'))
        rel = os.path.relpath(os.path.dirname(f), batch).replace('\\', '/')
        frame = j.get('gpu_frame_ms') or {}
        passes = j.get('passes') or {}
        top = sorted(groups_of(passes, 2).items(), key=lambda kv: -kv[1][0])[:10]
        coarse = sorted(groups_of(passes, 1).items(), key=lambda kv: -kv[1][0])
        rows.append(rel)
        print('%-52s %7.2f / %7.2f   %4d passes   render %s' % (rel, frame.get('median', 0.0), frame.get('p95', 0.0), len(passes), j.get('resolution')))
        print('      tracks: ' + '  '.join('%s %.2f' % (k, v[0]) for k, v in coarse))
        print('      groups: ' + '  '.join('%s %.2f' % (k, v[0]) for k, v in top))
    print('== logs: error bits, failures')
    for f in sorted(glob.glob(os.path.join(batch, '**', '*.log'), recursive=True)):
        if os.path.basename(f) == 'batch.log':
            continue
        text = open(f, encoding='utf-8-sig', errors='replace').read()
        notes = []
        for m in re.finditer(r'(?im)^.*(error bits?|FAILED|fail:|exceeds|budget).*$', text):
            line = m.group(0).strip()
            if 'error bits 0' in line.replace('0x0', '0') and 'FAIL' not in line:
                continue
            notes.append(line[:200])
        if notes:
            print(os.path.relpath(f, batch).replace('\\', '/'))
            for n in sorted(set(notes))[:8]:
                print('    ' + n)
    log = os.path.join(batch, 'batch.log')
    if os.path.exists(log):
        text = open(log, encoding='utf-8-sig', errors='replace').read()
        print('== batch log: gates that ended with a failure')
        for m in re.finditer(r'(?m)^\s*GATE FAILED.*$', text):
            print('  ' + m.group(0).strip())
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

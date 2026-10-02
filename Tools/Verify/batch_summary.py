# The batch's summary sheet (Run-Ue6Batch.ps1): the furnace sheets, every timing run's GPU frame with its largest pass
# groups, the A/B sheet (each variant's picture difference and GPU frame against its base's), the frames' error bits,
# the gates that failed.
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


def timing_of(directory):
    """(GPU frame median, pass groups by two name parts) of a run's timing_turning, or None."""
    files = sorted(glob.glob(os.path.join(directory, 'timing_turning', '*.json')))
    if not files:
        return None
    j = json.load(open(files[0], encoding='utf-8'))
    return (j.get('gpu_frame_ms') or {}).get('median', 0.0), {k: v[0] for k, v in groups_of(j.get('passes') or {}, 2).items()}, j.get('graph') or {}


def ab_sheet(batch):
    """The A/B group: the picture differences as the batch wrote them, then each variant's GPU frame against its base's."""
    manifest = os.path.join(batch, 'ab', 'manifest.json')
    if not os.path.exists(manifest):
        return
    print('== A/B of the switches: pictures (in units of the base picture\'s mean)')
    sheet = os.path.join(batch, 'ab', 'pictures.txt')
    if os.path.exists(sheet):
        print(open(sheet, encoding='utf-8-sig', errors='replace').read().rstrip())
    rows = json.load(open(manifest, encoding='utf-8-sig'))
    if isinstance(rows, dict):
        rows = [rows]
    print('== A/B of the switches: GPU frame median turning (ms), variant - base, then the pass groups that moved most (ms)')
    bases = {}
    for r in rows:
        if r.get('row') == 'base':
            bases[(r.get('group'), r.get('scene'))] = timing_of(os.path.join(batch, r.get('dir', '')))
    for r in rows:
        if r.get('row') == 'base':
            continue
        base, t = bases.get((r.get('group'), r.get('scene'))), timing_of(os.path.join(batch, r.get('dir', '')))
        label = '%s / %s / %s' % (r.get('group'), r.get('scene'), r.get('row'))
        if not base or not t:
            if base is None and t is None:
                continue  # (a group without timing runs)
            print('%-56s no timing (%s)' % (label, 'variant' if base else 'base'))
            continue
        moved = sorted(((k, t[1].get(k, 0.0) - base[1].get(k, 0.0)) for k in set(t[1]) | set(base[1])), key=lambda kv: -abs(kv[1]))[:4]
        print('%-56s %7.2f -> %7.2f (%+.2f)   scopes %s -> %s   %s' % (label, base[0], t[0], t[0] - base[0], base[2].get('pass_scopes', '?'), t[2].get('pass_scopes', '?'),
                                                                  '  '.join('%s %+.2f' % kv for kv in moved)))
        print('      set: %s' % r.get('set'))


def main(argv):
    if len(argv) < 3:
        print(__doc__ or 'usage: batch_summary.py <batch directory> <diag directory>')
        return 2
    batch, diag = argv[1], argv[2]
    print('== furnace room (measured / expected per stage; the day room over the night room = light through the walls)')
    for name in ('batch_furnace', 'batch_furnace_day', 'batch_furnace_day_noocc', 'batch_furnace_day_nofog', 'batch_furnace_day_fognoamb', 'batch_furnace_day_noclip', 'batch_furnace_day_ltvref'):
        d = os.path.join(diag, name)
        if not os.path.isdir(d):
            continue
        print('-- ' + name)
        r = subprocess.run([sys.executable, os.path.join(HERE, 'furnace.py'), d], capture_output=True, text=True)
        print((r.stdout or '') + (r.stderr or ''))
    # The sum of the passes' medians beside the frame's median: a stall from outside the frame (another process on the GPU,
    # a late submission under CPU load) lengthens whichever pass it lands in and so the frame, but moves a pass's median
    # only when it hits that pass in half the frames - the two agree on a quiet machine [measured 2026-10-02: lobby 4K
    # 11.86 / 11.79 quiet, 12.95 / 11.88 with builds running].
    print('== timings: GPU frame median / p95 (ms), the sum of the pass medians (ms), passes, then the largest groups (ms)')
    rows = []
    for f in sorted(glob.glob(os.path.join(batch, '**', 'timing_*', '*.json'), recursive=True)):
        j = json.load(open(f, encoding='utf-8'))
        rel = os.path.relpath(os.path.dirname(f), batch).replace('\\', '/')
        frame = j.get('gpu_frame_ms') or {}
        passes = j.get('passes') or {}
        top = sorted(groups_of(passes, 2).items(), key=lambda kv: -kv[1][0])[:10]
        coarse = sorted(groups_of(passes, 1).items(), key=lambda kv: -kv[1][0])
        rows.append(rel)
        total = sum((v.get('median', 0.0) if isinstance(v, dict) else float(v)) for v in passes.values())
        print('%-52s %7.2f / %7.2f   sum %7.2f   %4d passes   render %s' % (rel, frame.get('median', 0.0), frame.get('p95', 0.0), total, len(passes), j.get('resolution')))
        print('      tracks: ' + '  '.join('%s %.2f' % (k, v[0]) for k, v in coarse))
        print('      groups: ' + '  '.join('%s %.2f' % (k, v[0]) for k, v in top))
    ab_sheet(batch)
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

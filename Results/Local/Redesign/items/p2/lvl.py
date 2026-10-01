# absolute GI layer mean (x 2^EV of the frame) per mode and frame: python lvl.py lounge_1920x1080 alloff allon deploy
import numpy as np, os, sys, re
here = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return d.reshape(h, w, -1).astype(np.float64)[..., :3]
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
def evs(log, layer):
    t = open(log, 'rb').read()
    try: s = t.decode('utf-16')
    except Exception: s = t.decode('utf-8', 'replace')
    s = s.replace('\r', '').replace('\n', '')
    return {int(m.group(1)): float(m.group(2)) for m in re.finditer(r'captured ' + layer + r' \d+x\d+ \(internal layer, frame (\d+), ev100 ([\d.]+)\)', s)}
scene = sys.argv[1]
for m in sys.argv[2:]:
    stem = os.path.join(here, 'still_%s_%s' % (scene, m))
    ev = evs(stem + '.log', 'gi')
    out = []
    for f in (1, 4, 16, 299):
        p = '%s_gi_f%d.pfm' % (stem, f)
        if os.path.exists(p):
            l = lum(rd(p)) * 2.0 ** ev[f]
            out.append('f%d mean %.4g median %.4g (ev %.2f)' % (f, l.mean(), np.median(l), ev[f]))
    print('%-16s %s' % (m, '  '.join(out)))

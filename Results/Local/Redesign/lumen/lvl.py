# Absolute GI layer level of the lumen first runs: layer mean x 2^EV of the frame (the layer is E x exposure). CPU only.
import numpy as np, os, re, sys, glob
here = os.path.dirname(os.path.abspath(__file__))
def rd(p):
    with open(p, 'rb') as f:
        f.readline(); w, h = map(int, f.readline().split()); s = float(f.readline()); d = np.fromfile(f, '<f4' if s < 0 else '>f4')
    return d.reshape(h, w, -1).astype(np.float64)[..., :3]
def lum(a): return a[..., 0] * .2126 + a[..., 1] * .7152 + a[..., 2] * .0722
for stem in sys.argv[1:]:
    t = open(os.path.join(here, stem + '.log'), 'rb').read()
    try: s = t.decode('utf-16')
    except Exception: s = t.decode('utf-8', 'replace')
    s = s.replace('\r', '').replace('\n', '')
    ev = {int(m.group(1)): float(m.group(2)) for m in re.finditer(r'captured gi \d+x\d+ \(internal layer, frame (\d+), ev100 ([\d.]+)\)', s)}
    out = []
    for f in sorted(ev):
        p = os.path.join(here, '%s_gi_f%d.pfm' % (stem, f))
        if not os.path.exists(p): continue
        a = rd(p); l = lum(a) * 2.0 ** ev[f]
        m = a.reshape(-1, 3).mean(0)
        out.append('f%d %.1f (ev %.2f, r/b %.2f)' % (f, l.mean(), ev[f], m[0] / max(m[2], 1e-9)))
    print('%-12s %s' % (stem, '  '.join(out)))

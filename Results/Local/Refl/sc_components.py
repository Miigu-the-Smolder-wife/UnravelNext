"""Means of the surface cache component views (reflection.lumen_surface_cache_view_component 1..4) over the traced (M)
pixels of the judge captures: the cell's direct and indirect irradiance against the world GI cache's irradiance and the
hit shading's local-light sample at the same hits (each as E / pi, nits; 0 where the hit has no lit cell).
    python Results/Local/Refl/sc_components.py --tags C1,C2,C3,C4 [--case bath_lobby] [--mode still] [--mmode 2]"""
import argparse, glob, os, re
import numpy as np

def pfm(p):
    with open(p, 'rb') as f:
        h = f.readline().strip()
        w, hh = map(int, f.readline().split())
        s = float(f.readline())
        c = 3 if h == b'PF' else 1
        a = np.frombuffer(f.read(), dtype='<f4' if s < 0 else '>f4')
    return a.reshape(hh, w, c)

ap = argparse.ArgumentParser()
ap.add_argument('--tags', required=True)
ap.add_argument('--case', default='bath_lobby')
ap.add_argument('--mode', default='still')
ap.add_argument('--res', default='1080')
ap.add_argument('--mmode', type=int, default=-1, help='value of the mode layer for traced pixels (default: the most common non-zero one)')
a = ap.parse_args()
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..', '..', '..', 'Cache', 'ReflJudge')
for tag in a.tags.split(','):
    base = os.path.join(root, tag, '%s_%s_%s' % (a.case, a.mode, a.res))
    log = open(os.path.join(here, tag, '%s_%s_%s.log' % (a.case, a.mode, a.res)), errors='ignore').read()
    frames = sorted(int(re.search(r'_refl_f(\d+)\.pfm$', p).group(1)) for p in glob.glob(base + '_refl_f*.pfm') if not p.endswith('_alpha.pfm'))
    for f in frames:
        refl = pfm('%s_refl_f%d.pfm' % (base, f)).astype(np.float64)
        mode = pfm('%s_reflmode_f%d.pfm' % (base, f))[..., 0]
        h = min(refl.shape[0], mode.shape[0])
        refl, mode = refl[:h], mode[:h]
        mm = a.mmode
        if mm < 0:
            values, counts = np.unique(mode[mode > 0], return_counts=True)
            mm = int(values[np.argmax(counts)]) if len(values) else 0
        mask = mode == mm
        ev = re.search(r'captured refl .*?frame %d, ev100 ([0-9.]+)' % f, log)
        v = refl[mask]
        print('%-4s f%-3d M pixels %7d (mode %d) mean rgb %s lum %.3f ev100 %s zero share %.3f' % (
            tag, f, int(mask.sum()), mm, np.round(v.mean(axis=0), 3), float(v.mean()), ev.group(1) if ev else '?', float((v.sum(axis=1) == 0).mean())))

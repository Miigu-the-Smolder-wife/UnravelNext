# An engine capture against a path-traced reference of the same view, by blocks: the image is cut into columns x rows
# blocks and each block's mean luminance ratio engine / reference is printed, for the blocks where the reference's two
# halves agree (fireflies make the other blocks' means useless).
#   python Tools/Verify/ref_blocks.py <engine.pfm> <engine ev100> <reference.pfm> [--halves A.pfm B.pfm] [--blocks 4x3]
#                                     [--tolerance 0.03] [--reference-ev100 EV]
# The engine capture is exposed radiance (the gate's --capture-output): nits = value x 1.2 x 2^ev100. The reference is in
# nits, or - with --reference-ev100 (unx_reference --ev100) - exposed at that EV. The engine image is box-filtered to
# the reference's size (an integer ratio).
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
from pfm_to_png import read_pfm  # noqa: E402


def luminance(img):
    return img @ np.array([0.2126, 0.7152, 0.0722])


def main(argv):
    if len(argv) < 4:
        print(__doc__ or 'usage: ref_blocks.py <engine.pfm> <ev100> <reference.pfm> [--halves A B] [--blocks 4x3]')
        return 2
    engine = read_pfm(argv[1]).astype(np.float64)
    ev100 = float(argv[2])
    reference = read_pfm(argv[3]).astype(np.float64)
    halves, cols, rows, tolerance, reference_ev = None, 4, 3, 0.03, None
    i = 4
    while i < len(argv):
        if argv[i] == '--halves':
            halves = (read_pfm(argv[i + 1]).astype(np.float64), read_pfm(argv[i + 2]).astype(np.float64)); i += 3
        elif argv[i] == '--blocks':
            cols, rows = (int(v) for v in argv[i + 1].split('x')); i += 2
        elif argv[i] == '--tolerance':
            tolerance = float(argv[i + 1]); i += 2
        elif argv[i] == '--reference-ev100':
            reference_ev = float(argv[i + 1]); i += 2
        else:
            raise SystemExit('unknown option ' + argv[i])
    h, w, _ = reference.shape
    fy, fx = engine.shape[0] // h, engine.shape[1] // w
    if fy * h != engine.shape[0] or fx * w != engine.shape[1]:
        raise SystemExit('the engine image %dx%d is not an integer multiple of the reference %dx%d' % (engine.shape[1], engine.shape[0], w, h))
    engine = np.nan_to_num(engine, nan=0.0, posinf=0.0).reshape(h, fy, w, fx, 3).mean(axis=(1, 3))
    engine = engine * (1.2 * 2.0 ** ev100)
    if reference_ev is not None:
        reference = reference * (1.2 * 2.0 ** reference_ev)
    print('blocks %dx%d, ratio engine / reference (luminance; red, green, blue); "-" = the reference halves differ by more than %.0f %%' % (cols, rows, tolerance * 100))
    for r in range(rows):
        line = []
        for c in range(cols):
            ys, xs = slice(r * h // rows, (r + 1) * h // rows), slice(c * w // cols, (c + 1) * w // cols)
            if halves is not None:
                a, b = luminance(halves[0][ys, xs]).mean(), luminance(halves[1][ys, xs]).mean()
                if abs(a - b) > tolerance * 0.5 * (a + b):
                    line.append('      -                 ')
                    continue
            e, f = engine[ys, xs].reshape(-1, 3).mean(0), reference[ys, xs].reshape(-1, 3).mean(0)
            line.append('%5.2f (%4.2f %4.2f %4.2f)' % (luminance(e) / luminance(f), e[0] / f[0], e[1] / f[1], e[2] / f[2]))
        print('  ' + ' | '.join(line))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

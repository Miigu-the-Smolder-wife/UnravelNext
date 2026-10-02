# One A/B picture pair of the batch (Run-Ue6Batch.ps1, the ab group): how far a variant's frame is from its base's.
#   python Tools/Verify/ab_compare.py <base.pfm> <variant.pfm> <label>
# Prints one line: the label, then - in units of the base picture's mean (linear radiance x exposure) - the mean of
# |variant - base|, the share of pixels that differ by more than 2 % of that mean, and the largest difference. Two
# pictures of different sizes are reported as such (a layer whose layout a switch changes). Needs numpy only.
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pfm_to_png import read_pfm  # noqa: E402


def main(argv):
    if len(argv) < 4:
        print(__doc__ or 'usage: ab_compare.py <base.pfm> <variant.pfm> <label>')
        return 2
    base_path, variant_path, label = argv[1], argv[2], argv[3]
    for p in (base_path, variant_path):
        if not os.path.exists(p):
            print('%-72s no picture (%s)' % (label, os.path.basename(p)))
            return 0
    a = np.nan_to_num(read_pfm(base_path).astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    b = np.nan_to_num(read_pfm(variant_path).astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0)
    if a.shape != b.shape:
        print('%-72s sizes differ: base %dx%d, variant %dx%d' % (label, a.shape[1], a.shape[0], b.shape[1], b.shape[0]))
        return 0
    mean = max(float(np.mean(np.abs(a))), 1e-12)
    d = np.max(np.abs(a - b), axis=2)
    print('%-72s mean %.5f   over 2%%: %6.2f %% of pixels   max %.3f   (base mean %.4g)' %
          (label, float(np.mean(d)) / mean, 100.0 * float(np.mean(d > 0.02 * mean)), float(np.max(d)) / mean, mean))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

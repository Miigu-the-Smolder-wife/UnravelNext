# The furnace room's energy balance (scenegen furnace_room: a closed Lambert room, albedo 0.5, one point light of 100 cd
# at its centre). In the steady state the room's inner surface A = 256 m2 holds, as means over the surface,
#   direct irradiance        Phi / A              = 4.909 lux   (Phi = 4 pi x 100 lm)
#   first bounce             rho Phi / A          = 2.454 lux
#   all bounces              rho / (1 - rho) Phi / A = 4.909 lux
#   radiance leaving a wall  rho (direct + all bounces) / pi = 1.5625 nits
# and each stage of the indirect lighting must hold its part. From one run's captures (Run-Ue6Still.ps1 -Scene
# furnace_room -Layers gi,carddirect,cardindirect) this prints, stage by stage, measured / expected:
#   cards direct     the mesh cards' direct light (their lit texels' mean)         against the direct irradiance
#   cards indirect   the cards' radiosity                                          against all bounces
#   gather           the final gather's irradiance on the visible pixels           against all bounces
#   picture          the final picture's radiance on the visible pixels            against the leaving radiance
#   direct on screen what the picture leaves for the direct light (MegaLights)     against the direct irradiance
# The card means are over lit texels (the walls' inner faces: one texel density at this range); the screen means are over
# the view's pixels, which weigh the room's surfaces by how they are seen, not by area - within about 20 % for the direct
# light, closer for the bounces. A stage near 1 holds its light; a stage at 0.3 is where it is lost.
#   python Tools/Verify/furnace.py <capture directory> [--frame N]
import glob
import os
import re
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pfm_to_png import read_pfm  # noqa: E402

RHO, CANDELA, AREA = 0.5, 100.0, 256.0
FLUX = 4.0 * np.pi * CANDELA
DIRECT = FLUX / AREA
BOUNCES = RHO / (1.0 - RHO) * DIRECT
LEAVING = RHO * (DIRECT + BOUNCES) / np.pi


def luminance(img):
    return img @ np.array([0.2126, 0.7152, 0.0722])


def main(argv):
    if len(argv) < 2:
        print(__doc__ or 'usage: furnace.py <capture directory> [--frame N]')
        return 2
    directory, frame = argv[1], None
    if '--frame' in argv:
        frame = int(argv[argv.index('--frame') + 1])
    log = open(os.path.join(directory, 'run.log'), encoding='utf-8-sig', errors='replace').read()
    frames = sorted({int(m.group(1)) for m in re.finditer(r'captured final .*?frame (\d+), ev100', log)})
    if not frames:
        raise SystemExit('no captured frame in ' + directory)
    print('expected: direct %.3f lux, all bounces %.3f lux, leaving radiance %.4f nits' % (DIRECT, BOUNCES, LEAVING))
    for f in ([frame] if frame is not None else frames):
        ev = float(re.search(r'captured final .*?frame %d, ev100 ([-0-9.]+)' % f, log).group(1))
        to_nits = 1.2 * 2.0 ** ev

        def layer(name):
            found = glob.glob(os.path.join(directory, '*%s_f%d.pfm' % (name, f)))
            found = [p for p in found if '_alpha' not in p]
            return read_pfm(found[0]).astype(np.float64) if found else None

        print('frame %d (ev100 %.2f)' % (f, ev))
        direct, indirect = layer('_carddirect'), layer('_cardindirect')
        if direct is not None:
            d = luminance(direct) * 64.0
            lit = (d > 0.05) & (d < 200.0)  # (the room's inner faces hold 0.9 lux and more: a sunlit outer face is thousands of lux, one under the night's sun a thousandth)
            print('  cards direct     %7.3f lux   x %.2f   (%d lit texels)' % (d[lit].mean(), d[lit].mean() / DIRECT, int(lit.sum())))
            if indirect is not None:
                i = luminance(indirect) * 64.0
                print('  cards indirect   %7.3f lux   x %.2f   (first bounce alone would be x %.2f)' % (i[lit].mean(), i[lit].mean() / BOUNCES, RHO * DIRECT / BOUNCES))
        gi = layer('_gi')
        gather = None
        if gi is not None:
            gather = luminance(gi).mean() * to_nits
            print('  gather           %7.3f lux   x %.2f' % (gather, gather / BOUNCES))
        final = [p for p in glob.glob(os.path.join(directory, '*_f%d.pfm' % f)) if not re.search(r'_(alpha|gi|refl|shadow|reflmode|depth|ao|roughspec|card\w+)_f\d+\.pfm$', p) and '_alpha' not in p]
        if final:
            picture = luminance(read_pfm(final[0]).astype(np.float64)).mean() * to_nits
            print('  picture          %7.4f nits  x %.2f' % (picture, picture / LEAVING))
            if gather is not None:
                on_screen = picture * np.pi / RHO - gather
                print('  direct on screen %7.3f lux   x %.2f' % (on_screen, on_screen / DIRECT))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

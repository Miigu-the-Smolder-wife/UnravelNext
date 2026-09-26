"""Anisotropic GGX on area lights: render A's stretch rule against the lobe's quadrature (render C, 2026-09-27).

Rule (AnisoShading.hlsli shAnisoLtc): directions are mapped by S = diag(alpha_i / alpha_t, alpha_i / alpha_b, 1) in the
lobe frame (t, b, n), alpha_i = sqrt(alpha_t alpha_b); the stretched view v_s = normalize(S v); the light's polygon is
integrated against the isotropic lobe at alpha_i and v_s (the LTC stands for that lobe); the magnitude is the
anisotropic albedo. This study measures the rule's own error - the isotropic lobe itself is integrated exactly here, so
the LTC fit's error (A's isotropic LTC measurements) comes on top:
  exact  = integral over the light of f_a(v, l) cos(l) dl                             (single scattering, F = 1)
  rule   = E_a(v) x integral over S(light) of f_i(v_s, l') cos(l') dl' / E_i(v_s)
Both by dense quadrature over the light's area (dl = cos_L dA / d^2); S(light) is the image of the light's directions
under l -> normalize(S l), whose solid angle element is det(S) / |S l|^3 dl.
Run: python aniso_area_stretch_study.py > Results/C/Aniso/area_stretch_study.txt
"""
import numpy as np

NQ = 160  # quadrature points per side of the light
N_ALB = 512


def lam(w, at, ab):
    s2 = (at * at * w[0] ** 2 + ab * ab * w[1] ** 2) / np.maximum(w[2] ** 2, 1e-30)
    return 0.5 * (-1 + np.sqrt(1 + s2))


def lobe(v, l, at, ab):
    """f cos with F = 1 (single scattering); v (3,), l (3, N) unit, local frame (t, b, n)."""
    h = v[:, None] + l
    h = h / np.linalg.norm(h, axis=0)
    x, y, z = h[0] / at, h[1] / ab, h[2]
    d = x * x + y * y + z * z
    D = 1 / (np.pi * at * ab * d * d)
    G2 = 1 / (1 + lam(v, at, ab) + lam(l, at, ab))
    f = D * G2 / (4 * v[2] * np.maximum(l[2], 1e-12))
    return np.where((l[2] > 0) & (h[2] > 0), f * l[2], 0)


def albedo(v, at, ab):
    # hemisphere quadrature (cos-theta, phi midpoint grid, refined near the lobe by density: plain dense grid)
    n = N_ALB
    c = (np.arange(n) + 0.5) / n
    ph = (np.arange(2 * n) + 0.5) / (2 * n) * 2 * np.pi
    C, P = np.meshgrid(c, ph)
    s = np.sqrt(1 - C * C)
    l = np.stack([s * np.cos(P), s * np.sin(P), C]).reshape(3, -1)
    return lobe(v, l, at, ab).sum() * (1 / n) * (2 * np.pi / (2 * n))


def light_dirs(center, u, w, size):
    a = (np.arange(NQ) + 0.5) / NQ - 0.5
    A, B = np.meshgrid(a * size[0], a * size[1])
    p = center[:, None] + u[:, None] * A.ravel() + w[:, None] * B.ravel()
    d = np.linalg.norm(p, axis=0)
    l = p / d
    nl = np.cross(u, w)
    cosL = np.abs(nl @ l)
    dA = size[0] * size[1] / (NQ * NQ)
    return l, cosL * dA / (d * d)


def main():
    print("# anisotropic area light: stretch rule vs quadrature (rule error only; the isotropic LTC fit error adds on top)")
    print("# r s  alpha_t alpha_b  mu  | worst relative error over light placements with >= 5 % of the peak, and the mean")
    for r, s in [(0.4, 0.0), (0.5, 0.5), (0.3, 0.8), (0.3, 0.5), (0.2, 0.9), (0.6, 0.9), (0.1, 0.7)]:
        a = max(r * r, 1e-4)
        at, ab = a + (1 - a) * s * s, a
        ai = np.sqrt(at * ab)
        S = np.diag([ai / at, ai / ab, 1.0])
        for mu in [0.9, 0.5, 0.2]:
            sn = np.sqrt(1 - mu * mu)
            results = []
            for phi_v in [0.0, 0.7, 1.5708]:
                v = np.array([sn * np.cos(phi_v), sn * np.sin(phi_v), mu])
                vs = S @ v
                vs /= np.linalg.norm(vs)
                Ea = albedo(v, at, ab)
                Ei = albedo(vs, ai, ai)
                refl = np.array([-v[0], -v[1], v[2]])
                for off_az in [0.0, 0.4, -0.8, 1.6]:
                    for off_el in [0.0, 0.25, -0.2]:
                        for dist, size in [(2.0, (1.0, 0.5)), (4.0, (0.6, 0.6)), (1.0, (2.0, 0.3))]:
                            # light centre: the mirror direction rotated in azimuth and elevation
                            th = np.arccos(np.clip(refl[2], -1, 1)) + off_el
                            ph = np.arctan2(refl[1], refl[0]) + off_az
                            if th >= 1.5:
                                continue
                            dirc = np.array([np.sin(th) * np.cos(ph), np.sin(th) * np.sin(ph), np.cos(th)])
                            center = dirc * dist
                            u = np.cross(dirc, [0, 0, 1.0])
                            if np.linalg.norm(u) < 1e-6:
                                u = np.array([1.0, 0, 0])
                            u /= np.linalg.norm(u)
                            w = np.cross(dirc, u)
                            rot = 0.3 * off_az  # the light turned about its axis too
                            u, w = u * np.cos(rot) + w * np.sin(rot), w * np.cos(rot) - u * np.sin(rot)
                            l, dw = light_dirs(center, u, w, size)
                            keep = l[2] > 0
                            exact = (lobe(v, l, at, ab) * dw)[keep].sum()
                            ls = S @ l
                            nls = np.linalg.norm(ls, axis=0)
                            lsn = ls / nls
                            jac = np.linalg.det(S) / nls ** 3
                            rule = Ea / Ei * (lobe(vs, lsn, ai, ai) * jac * dw)[keep].sum()
                            results.append((exact, rule))
            ex = np.array([e for e, _ in results])
            ru = np.array([q for _, q in results])
            sel = ex >= 0.05 * ex.max()
            rel = np.abs(ru - ex)[sel] / ex[sel]
            print(f"{r:.1f} {s:.1f}  {at:6.4f} {ab:6.4f}  {mu:.1f} | worst {rel.max():.3f}  mean {rel.mean():.3f}  ({sel.sum()} placements)", flush=True)


if __name__ == "__main__":
    main()

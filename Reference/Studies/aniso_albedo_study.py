"""Anisotropic GGX directional albedo study (render C, A9 anisotropy, 2026-09-27).

The Standard model's multiple-scattering compensation is 1 + f0 (1/E - 1) with E(n.v, r) the isotropic single-scattering
albedo table. An anisotropic lobe needs an E for (n.v, azimuth, alpha_t, alpha_b). Candidates map the view to one
isotropic roughness alpha_e and read the isotropic E:
  (a) geometric mean       alpha_e = sqrt(alpha_t alpha_b)
  (b) view projection      alpha_e = sqrt(alpha_t^2 cos^2 phi + alpha_b^2 sin^2 phi)   (the roughness in Lambda(v))
  (c) blend                alpha_e^2 = mu^2 alpha_t alpha_b + (1 - mu^2) (b)^2         (continuous at the pole)
The true E is integrated here with visible-normal sampling (Heitz 2018, stretched): E(v) = mean over h ~ VNDF of
G2(v, l) / G1(v) with l = reflect(-v, h), 0 where n.l <= 0. Deterministic stratified grid of N x N samples.
Run: python aniso_albedo_study.py > Results/C/Aniso/albedo_study.txt
"""
import numpy as np

N = 384
u1, u2 = np.meshgrid((np.arange(N) + 0.5) / N, (np.arange(N) + 0.5) / N)
u1 = u1.ravel(); u2 = u2.ravel()


def lam(w, at, ab):
    # Smith Lambda of the anisotropic GGX for local direction w = (t, b, n)
    s2 = (at * at * w[0] ** 2 + ab * ab * w[1] ** 2) / np.maximum(w[2] ** 2, 1e-30)
    return 0.5 * (-1 + np.sqrt(1 + s2))


def albedo(mu, phi, at, ab):
    st = np.sqrt(1 - mu * mu)
    v = np.array([st * np.cos(phi), st * np.sin(phi), mu])
    vh = np.array([at * v[0], ab * v[1], v[2]]); vh /= np.linalg.norm(vh)
    lensq = vh[0] ** 2 + vh[1] ** 2
    t1 = np.array([-vh[1], vh[0], 0]) / np.sqrt(lensq) if lensq > 0 else np.array([1.0, 0, 0])
    t2 = np.cross(vh, t1)
    r = np.sqrt(u1); ph = 2 * np.pi * u2
    p1 = r * np.cos(ph); s = 0.5 * (1 + vh[2])
    p2 = (1 - s) * np.sqrt(np.maximum(0, 1 - p1 * p1)) + s * r * np.sin(ph)
    p3 = np.sqrt(np.maximum(0, 1 - p1 * p1 - p2 * p2))
    nh = t1[:, None] * p1 + t2[:, None] * p2 + vh[:, None] * p3
    h = np.stack([at * nh[0], ab * nh[1], np.maximum(0, nh[2])]); h /= np.linalg.norm(h, axis=0)
    vdh = v @ h
    l = 2 * vdh * h - v[:, None]
    ok = l[2] > 0
    lv = lam(v, at, ab); ll = lam(l, at, ab)
    g2_over_g1 = np.where(ok, (1 + lv) / (1 + lv + ll), 0)
    return g2_over_g1.mean()


def eiso(mu, a):
    return albedo(mu, 0.0, a, a)


def main():
    rows = []
    print("# anisotropic GGX single-scattering albedo: true E vs isotropic E at alpha_e (F = 1)")
    print("# columns: alpha_t alpha_b mu phi E_true  err_a err_b err_c   (err = E_iso(alpha_e) - E_true)")
    worst = {"a": 0, "b": 0, "c": 0}
    worstc = {"a": 0, "b": 0, "c": 0}  # error of the compensation factor 1/E for f0 = 1 (metals), relative
    for at, ab in [(0.2, 0.05), (0.4, 0.1), (0.6, 0.15), (0.8, 0.2), (1.0, 0.25), (0.5, 0.05), (1.0, 0.1), (0.3, 0.2), (0.64, 0.01)]:
        for mu in [1.0, 0.9, 0.7, 0.5, 0.3, 0.15, 0.05]:
            for phi in [0.0, np.pi / 8, np.pi / 4, 3 * np.pi / 8, np.pi / 2]:
                e = albedo(mu, phi, at, ab)
                av = np.sqrt(at * at * np.cos(phi) ** 2 + ab * ab * np.sin(phi) ** 2)
                cand = {"a": np.sqrt(at * ab), "b": av, "c": np.sqrt(mu * mu * at * ab + (1 - mu * mu) * av * av)}
                errs = {}
                for k, a in cand.items():
                    ei = eiso(mu, a)
                    errs[k] = ei - e
                    worst[k] = max(worst[k], abs(ei - e))
                    worstc[k] = max(worstc[k], abs(e / ei - 1))
                print(f"{at:5.2f} {ab:5.2f} {mu:4.2f} {phi:5.3f} {e:8.5f}  {errs['a']:+.5f} {errs['b']:+.5f} {errs['c']:+.5f}")
    print("# worst |E_iso(alpha_e) - E_true|:", {k: round(float(v), 5) for k, v in worst.items()})
    print("# worst relative error of 1/E (the metal compensation factor):", {k: round(float(v), 5) for k, v in worstc.items()})


if __name__ == "__main__":
    main()

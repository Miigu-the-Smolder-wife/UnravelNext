# Clearcoat R1 + thin film under the coat vs physical model [measured]

Coat r_c 0.12 over a filmed base (outer index 1.5). Physical: layer model (coat with microsurface multiple scattering) with the exact spectral film reflectance in the base specular; definition: R1 with film method (a). Same metrics and criteria as clearcoat_r1.md.

| case | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | criteria |
|---|---|---|---|---|---|---|---|---|---|
| film 2.4 / 300 nm on design default (1.5, 2), base r 0.3 | 0.12 | R1 + film (a) | 5.6% @88° (-0.044) | +1.7 / +2.8 / +4.4 / +2.9 / -0.2 % | 0.088 @90° | 0.008 | 2.47 / 3.41 | 2.15 / 4.95 | FAIL |
| film 1.5 / 500 nm on fitted gold, base r 0.2 | 0.12 | R1 + film (a) | 8.0% @76° (+0.052) | -0.2 / +0.1 / +3.2 / +7.9 / +4.8 % | 0.319 @76° | 0.007 | 3.29 / 9.42 | 6.72 / 18.55 | FAIL |
| film 2.4 / 150 nm on red paint dielectric (1.5, 0), base r 0.5 | 0.12 | R1 + film (a) | 6.4% @90° (-0.052) | +0.5 / +0.8 / +0.5 / +0.2 / -1.0 % | 0.085 @90° | 0.012 | 0.85 / 1.23 | 0.96 / 2.26 | FAIL |

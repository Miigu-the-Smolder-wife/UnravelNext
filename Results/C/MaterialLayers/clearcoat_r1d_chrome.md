# Clearcoat R1 vs physical layer model [measured]

`unx_study_material_layers clearcoat_r1d`. Physical coat: microsurface multiple scattering (Heitz et al. 2016, energy conserving). Criteria (MATERIAL_LAYERS 3): albedo rel <= 2 % (or abs <= 0.005), L1 <= 0.05, render dE76 mean <= 1.0 and P99 <= 2.3 (white furnace / sun + sky sphere, 64 x 64). Photons per incidence bin: 1048576 (physical), 1048576 (definition). 'L1 noise' = physical half vs full (MC floor).

| base | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | render noise P99 furnace / sky | criteria |
|---|---|---|---|---|---|---|---|---|---|---|
| chrome r 0.1 | 0.05 | R1 + A2 | 8.2% @82° (+0.067) | -0.2 / -0.5 / -0.5 / +3.7 / +6.9 % | 0.442 @80° | 0.007 | 1.37 / 5.72 | 4.17 / 10.39 | 0.08 / 0.47 | FAIL |
| chrome r 0.1 | 0.05 | R1 + A2 + B3 | 1.2% @84° (+0.010) | -0.2 / +0.1 / +0.4 / +0.7 / +0.8 % | 0.379 @80° | 0.007 | 0.08 / 0.29 | 3.29 / 14.18 | 0.08 / 0.47 | FAIL |
| chrome r 0.1 | 0.12 | R1 + A2 | 12.2% @82° (+0.097) | -0.2 / -0.3 / +0.5 / +8.9 / +10.2 % | 0.412 @80° | 0.006 | 1.19 / 4.25 | 4.21 / 10.54 | 0.11 / 0.47 | FAIL |
| chrome r 0.1 | 0.12 | R1 + A2 + B3 | 2.0% @78° (+0.016) | -0.1 / +0.1 / +0.3 / +0.9 / +1.4 % | 0.391 @80° | 0.006 | 0.27 / 1.10 | 3.45 / 16.11 | 0.11 / 0.47 | FAIL |
| chrome r 0.1 | 0.30 | R1 + A2 | 29.0% @90° (+0.208) | -0.0 / +0.7 / +7.4 / +14.8 / +25.2 % | 0.627 @76° | 0.010 | 1.85 / 3.39 | 4.43 / 17.93 | 0.09 / 0.78 | FAIL |
| chrome r 0.1 | 0.30 | R1 + A2 + B3 | 1.3% @90° (+0.010) | -0.0 / -0.2 / +0.2 / +0.1 / -0.2 % | 0.525 @72° | 0.010 | 0.13 / 0.67 | 4.82 / 23.50 | 0.09 / 0.78 | FAIL |

## Equivalent roughness of the base lobe outside (alpha_eq ~ eta alpha'_b)

| base | r_c | alpha'_b | eta alpha'_b | physical 75 % half-angle | GGX(eta alpha'_b) 75 % half-angle |
|---|---|---|---|---|---|
| chrome r 0.1 | 0.05 | 0.0100 | 0.0150 | 4.0° | 4.0° |
| chrome r 0.1 | 0.12 | 0.0106 | 0.0158 | 4.0° | 4.0° |
| chrome r 0.1 | 0.30 | 0.0235 | 0.0352 | 9.0° | 7.0° |

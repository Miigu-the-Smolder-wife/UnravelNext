# Clearcoat R1 vs physical layer model [measured]

`unx_study_material_layers clearcoat_r1d`. Physical coat: microsurface multiple scattering (Heitz et al. 2016, energy conserving). Criteria (MATERIAL_LAYERS 3): albedo rel <= 2 % (or abs <= 0.005), L1 <= 0.05, render dE76 mean <= 1.0 and P99 <= 2.3 (white furnace / sun + sky sphere, 64 x 64). Photons per incidence bin: 1048576 (physical), 1048576 (definition). 'L1 noise' = physical half vs full (MC floor).

| base | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | render noise P99 furnace / sky | criteria |
|---|---|---|---|---|---|---|---|---|---|---|
| metal flake r 0.3 | 0.05 | R1 + A2 | 6.9% @72° (+0.038) | +0.5 / +1.1 / +4.9 / +6.6 / +2.7 % | 0.237 @72° | 0.012 | 2.99 / 4.84 | 6.14 / 17.00 | 0.10 / 1.27 | FAIL |
| metal flake r 0.3 | 0.05 | R1 + A2 + B3 | 0.0% @0° (+0.000) | +0.0 / +0.2 / +0.7 / +0.3 / +0.1 % | 0.184 @72° | 0.012 | 0.22 / 0.42 | 4.06 / 18.27 | 0.10 / 1.27 | FAIL |
| metal flake r 0.3 | 0.12 | R1 + A2 | 7.1% @76° (+0.041) | +0.6 / +1.1 / +5.5 / +7.1 / +3.8 % | 0.232 @72° | 0.012 | 3.02 / 4.69 | 6.10 / 16.94 | 0.12 / 1.19 | FAIL |
| metal flake r 0.3 | 0.12 | R1 + A2 + B3 | 1.3% @88° (-0.011) | +0.1 / +0.0 / +0.5 / +0.2 / +0.0 % | 0.185 @70° | 0.012 | 0.20 / 0.43 | 4.10 / 18.57 | 0.12 / 1.19 | FAIL |
| metal flake r 0.3 | 0.30 | R1 + A2 | 18.3% @90° (+0.108) | +1.4 / +3.0 / +11.2 / +13.9 / +14.8 % | 0.269 @70° | 0.013 | 3.84 / 6.23 | 5.68 / 15.39 | 0.10 / 1.27 | FAIL |
| metal flake r 0.3 | 0.30 | R1 + A2 + B3 | 1.1% @90° (+0.006) | +0.3 / -0.1 / +0.1 / -0.1 / -0.9 % | 0.230 @68° | 0.013 | 0.13 / 0.30 | 4.71 / 19.61 | 0.10 / 1.27 | FAIL |

## Equivalent roughness of the base lobe outside (alpha_eq ~ eta alpha'_b)

| base | r_c | alpha'_b | eta alpha'_b | physical 75 % half-angle | GGX(eta alpha'_b) 75 % half-angle |
|---|---|---|---|---|---|
| metal flake r 0.3 | 0.05 | 0.0900 | 0.1350 | 26.0° | 26.0° |
| metal flake r 0.3 | 0.12 | 0.0901 | 0.1351 | 26.0° | 26.0° |
| metal flake r 0.3 | 0.30 | 0.0925 | 0.1387 | 28.0° | 27.0° |

## forest_combat at 3840x2160, w_cap 6.0 px

Lights: sun on, local lights 0 (0 casting shadows).

Band of a visible triangle (COVERAGE_REDESIGN 14.9): w_px = smallest altitude x instance scale x focal / distance to the triangle centroid; C below 0.25 px; solid B below 1.5 px; flat (Foliage or two-sided) B only when |cos theta| x w_px < 1.5 px (theta between the view ray and the sheet normal) or w_px < w_cap, else A. Visible = hit by one of 16 stratified sub-samples per pixel (no wind). P_A / P_B / P_C = pixels whose sub-samples include a triangle of that band (ARCHITECTURE 2 table: P_B, P_C); T_A / T_B / T_C = distinct visible triangles per band (before any LOD).

| camera | surface px (centre ray) | surface % | P_A / P_B / P_C (M px) | P_B or P_C | T_A / T_B / T_C (visible) | instances in frustum |
|---|---|---|---|---|---|---|
| eye | 8.29 M | 100.0 % | 7.54 / 1.02 / 0.02 | 1.02 M | 0.01 M / 0.31 M / 0.00 M | 152783 |
| up | 8.03 M | 96.8 % | 4.17 / 4.58 / 0.00 | 4.58 M | 0.03 M / 0.45 M / 0.00 M | 234 |
| edge | 7.44 M | 89.7 % | 5.87 / 2.52 / 0.14 | 2.58 M | 0.01 M / 2.73 M / 0.02 M | 208021 |
| vista | 5.07 M | 61.2 % | 1.00 / 3.93 / 1.93 | 5.07 M | 0.10 M / 15.20 M / 21.37 M | 166930 |

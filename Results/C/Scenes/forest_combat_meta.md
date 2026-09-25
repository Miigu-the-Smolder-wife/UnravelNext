## forest_combat at 3840x2160

Lights: sun on, local lights 0 (0 casting shadows).

Band of a triangle: w_px = smallest altitude x instance scale x focal / distance to the instance centre; flat (Foliage or two-sided) B below 8 px, solid B below 1.5 px, C below 0.25 px. Visible = hit by one of 16 stratified sub-samples per pixel (no wind). P_A / P_B / P_C = pixels whose sub-samples include a triangle of that band (ARCHITECTURE 2 table: P_B, P_C). Frustum counts include occluded triangles.

| camera | surface px (centre ray) | surface % | P_A / P_B / P_C (M px) | P_B or P_C | visible triangles A / B / C | instances in frustum | frustum triangles A / B / C |
|---|---|---|---|---|---|---|---|
| eye | 8.29 M | 100.0 % | 7.48 / 1.17 / 0.05 | 1.20 M | 0.01 M / 0.36 M / 0.00 M | 151985 | 2.6 M / 102.0 M / 1117.6 M |
| up | 8.03 M | 96.8 % | 2.79 / 5.79 / 0.00 | 5.79 M | 0.02 M / 0.47 M / 0.00 M | 171 | 2.2 M / 13.5 M / 0.0 M |
| edge | 7.44 M | 89.7 % | 5.87 / 2.52 / 0.15 | 2.58 M | 0.01 M / 2.73 M / 0.02 M | 206997 | 2.5 M / 99.8 M / 1686.6 M |

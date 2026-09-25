## forest_thin at 3840x2160

Lights: sun on, local lights 0 (0 casting shadows).

Band of a triangle: w_px = smallest altitude x instance scale x focal / distance to the instance centre; flat (Foliage or two-sided) B below 8 px, solid B below 1.5 px, C below 0.25 px. Visible = hit by one of 16 stratified sub-samples per pixel (no wind). P_A / P_B / P_C = pixels whose sub-samples include a triangle of that band (ARCHITECTURE 2 table: P_B, P_C). Frustum counts include occluded triangles.

| camera | surface px (centre ray) | surface % | P_A / P_B / P_C (M px) | P_B or P_C | visible triangles A / B / C | instances in frustum | frustum triangles A / B / C |
|---|---|---|---|---|---|---|---|
| forest | 7.21 M | 86.9 % | 5.15 / 3.14 / 0.18 | 3.19 M | 0.01 M / 3.13 M / 0.15 M | 45714 | 2.2 M / 24.2 M / 328.9 M |
| meadow | 7.97 M | 96.1 % | 6.53 / 2.33 / 0.14 | 2.37 M | 0.01 M / 2.29 M / 0.01 M | 335933 | 2.2 M / 26.7 M / 2517.1 M |
| canopy | 3.52 M | 42.5 % | 1.48 / 2.83 / 0.00 | 2.83 M | 0.02 M / 0.31 M / 0.00 M | 43 | 2.2 M / 3.3 M / 0.0 M |

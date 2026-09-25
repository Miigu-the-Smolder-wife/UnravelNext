# Metal presets: fitted RGB n, k [measured]

`unx_study_material_layers metals`. Per channel, (n, k) minimising the squared reflectance error against the spectral reference (CIE 1931, E, Rec.709) over incidence 0-89 deg in air; degenerate channels take the smallest k within 0.1 % of the best error. dE76 of the bare metal (d = 0) over incidence 0-89 deg in air and under a 1.5 coat: mean / max.

| metal | n (R, G, B) | k (R, G, B) | fit max abs dR (R, G, B) | dE air mean / max | dE coat mean / max | point samples 650/550/450: dE air mean / max |
|---|---|---|---|---|---|---|
| gold | 0.150, 0.492, 1.500 | 12.100, 2.270, 1.764 | 0.0161, 0.0008, 0.0026 | 0.46 / 0.79 | 0.61 / 1.05 | 5.15 / 6.32 |
| copper | 0.260, 1.050, 1.300 | 3.196, 2.634, 2.344 | 0.0017, 0.0011, 0.0006 | 0.05 / 0.10 | 0.08 / 0.11 | 1.52 / 1.81 |
| silver | 0.052, 0.054, 0.048 | 4.192, 3.500, 2.738 | 0.0005, 0.0002, 0.0011 | 0.03 / 0.09 | 0.04 / 0.08 | 0.18 / 0.20 |
| aluminium | 1.470, 0.988, 0.594 | 7.600, 6.558, 5.304 | 0.0010, 0.0002, 0.0004 | 0.03 / 0.06 | 0.02 / 0.04 | 0.13 / 0.30 |
| iron | 2.950, 2.900, 2.500 | 3.072, 2.936, 2.734 | 0.0021, 0.0014, 0.0022 | 0.07 / 0.22 | 0.10 / 0.24 | 0.30 / 0.35 |

```cpp
// Metal presets (substrateIor, substrateExtinction), fitted per channel; unx_study_material_layers metals.
// gold
{ { 0.1500f, 0.4920f, 1.5000f }, { 12.1000f, 2.2700f, 1.7640f } },
// copper
{ { 0.2600f, 1.0500f, 1.3000f }, { 3.1960f, 2.6340f, 2.3440f } },
// silver
{ { 0.0520f, 0.0540f, 0.0480f }, { 4.1920f, 3.5000f, 2.7380f } },
// aluminium
{ { 1.4700f, 0.9880f, 0.5940f }, { 7.6000f, 6.5580f, 5.3040f } },
// iron
{ { 2.9500f, 2.9000f, 2.5000f }, { 3.0720f, 2.9360f, 2.7340f } },
```

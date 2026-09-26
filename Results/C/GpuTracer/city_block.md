## city_block / street 2560x1440: GPU vs CPU reference [measured]

CPU: `street_2560x1440_4096_ee15d94a67cda9b3_d653e0243d6dcb9b.pfm`, GPU: `street_2560x1440_4096_ee15d94a67cda9b3_cad623af73f8d1c2.pfm`. Luminance; noise of each image from its halves.

Tile z-scores (16 x 16 px, 14400 tiles): mean -0.004 (standard error 0.008), deviation 1.021, |z| > 3: 0.28 % (normal 0.27 %), |z| > 5: 0

| mask | pixels | CPU mean Y | GPU - CPU (relative) | standard error | bias / error |
|---|---|---|---|---|---|
| all | 3686400 | 0.08745 | -0.0013 % | 0.0037 % | -0.34 |
| sky | 168634 | 0.073467 | -0.0010 % | 0.0037 % | -0.28 |
| edge | 145299 | 0.059633 | +0.0000 % | 0.0127 % | +0.00 |
| leaf (Foliage) | 11209 | 0.012544 | +0.0483 % | 0.0525 % | +0.92 |
| metal (metallic >= 0.5) | 138 | 0.038786 | +0.0234 % | 0.7059 % | +0.03 |
| ground (terrain / street) | 1410821 | 0.015265 | -0.0367 % | 0.0376 % | -0.98 |
| other surfaces | 1950299 | 0.14338 | +0.0014 % | 0.0031 % | +0.46 |

relMSE(CPU, GPU) 0.0005721; expected from the two noises (h_CPU + h_GPU) / 4 = 0.0005814 (h_CPU 0.001354, h_GPU 0.0009717): ratio 0.984. HDR-FLIP(CPU, GPU) mean 0.0308, P99 0.1911; CPU halves (floor) mean 0.0341, P99 0.1931.


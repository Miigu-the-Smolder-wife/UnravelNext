## forest_thin / forest 2560x1440: GPU vs CPU reference [measured]

CPU: `forest_2560x1440_4096_2896af93d793e32a_1087ac4dcbdc4a20.pfm`, GPU: `forest_2560x1440_4096_2896af93d793e32a_418fe92eadb86da6.pfm`. Luminance; noise of each image from its halves.

Tile z-scores (16 x 16 px, 14400 tiles): mean 0.043 (standard error 0.008), deviation 1.617, |z| > 3: 1.61 % (normal 0.27 %), |z| > 5: 71

| mask | pixels | CPU mean Y | GPU - CPU (relative) | standard error | bias / error |
|---|---|---|---|---|---|
| all | 3686400 | 0.090046 | +0.0023 % | 0.0007 % | +3.31 |
| sky | 229718 | 0.14545 | +0.0016 % | 0.0029 % | +0.54 |
| edge | 1580317 | 0.080936 | +0.0041 % | 0.0014 % | +2.97 |
| leaf (Foliage) | 48705 | 0.054183 | +0.0137 % | 0.0056 % | +2.44 |
| ground (terrain / street) | 1450285 | 0.092487 | -0.0001 % | 0.0007 % | -0.19 |
| other surfaces | 377375 | 0.089721 | +0.0048 % | 0.0012 % | +4.12 |

relMSE(CPU, GPU) 0.0001272; expected from the two noises (h_CPU + h_GPU) / 4 = 0.0001226 (h_CPU 0.0002464, h_GPU 0.0002439): ratio 1.038. HDR-FLIP(CPU, GPU) mean 0.0159, P99 0.0461; CPU halves (floor) mean 0.0199, P99 0.0575.


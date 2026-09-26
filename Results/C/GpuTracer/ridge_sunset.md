## ridge_sunset / ridge 2560x1440: GPU vs CPU reference [measured]

CPU: `ridge_2560x1440_4096_c96b17668d6d6466_1087ac4dcbdc4a20.pfm`, GPU: `ridge_2560x1440_4096_c96b17668d6d6466_418fe92eadb86da6.pfm`. Luminance; noise of each image from its halves.

Tile z-scores (16 x 16 px, 14400 tiles): mean -0.037 (standard error 0.008), deviation 1.023, |z| > 3: 0.42 % (normal 0.27 %), |z| > 5: 0

| mask | pixels | CPU mean Y | GPU - CPU (relative) | standard error | bias / error |
|---|---|---|---|---|---|
| all | 3686400 | 2.0312 | +0.0264 % | 0.0438 % | +0.60 |
| sky | 2020818 | 3.5938 | +0.0272 % | 0.0452 % | +0.60 |
| edge | 176259 | 0.098496 | +0.0013 % | 0.0022 % | +0.57 |
| ground (terrain / street) | 1466030 | 0.14026 | -0.0018 % | 0.0003 % | -7.19 |
| other surfaces | 23293 | 0.099862 | +0.0074 % | 0.0071 % | +1.03 |

relMSE(CPU, GPU) 0.0001284; expected from the two noises (h_CPU + h_GPU) / 4 = 0.0001272 (h_CPU 0.000255, h_GPU 0.0002539): ratio 1.009. HDR-FLIP(CPU, GPU) mean 0.0110, P99 0.0387; CPU halves (floor) mean 0.0140, P99 0.0496.


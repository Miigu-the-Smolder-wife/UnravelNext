# DesignBench 20260925_144659 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| coverage | slivers 0.5 px x 20 px: count pass (conservative raster, tile atomics only) | 61.5111 | ms | 0.009704 M fragments, max/tile 50, tiles > 1024: 0 |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 71.1613 | ms | 0.019408 M appended, 0 overflow, 3666.596249 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 66.2893 | ms | 0.009704 M appended, 0 overflow, 6831.131492 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 69.8858 | ms | 0.009704 M appended, 0 overflow, 7201.751855 ns/fragment |
| coverage | slivers 0.5 px x 20 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 2.2355 | ms | 230.368920 ns/fragment, 60.641819 ns/pixel (all pixels) |
| coverage | cards 4 px: count pass (conservative raster, tile atomics only) | 5.6301 | ms | 0.015996 M fragments, max/tile 93, tiles > 1024: 0 |
| coverage | cards 4 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 7.3344 | ms | 0.031992 M appended, 0 overflow, 229.257314 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 7.3102 | ms | 0.015996 M appended, 0 overflow, 457.001750 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 9.3909 | ms | 0.015996 M appended, 0 overflow, 587.078020 ns/fragment |
| coverage | cards 4 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 1.034 | ms | 64.641160 ns/fragment, 28.049045 ns/pixel (all pixels) |

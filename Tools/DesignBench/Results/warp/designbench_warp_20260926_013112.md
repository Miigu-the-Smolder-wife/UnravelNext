# DesignBench 20260926_013112 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| coverage | slivers 0.5 px x 20 px: count pass (conservative raster, tile atomics only) | 45.986 | ms | 0.009704 M fragments, max/tile 50, tiles > 1024: 0 |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 53.7793 | ms | 0.009704 M appended, 0 overflow, 5541.972383 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 52.424 | ms | 0.009704 M appended, 0 overflow, 5402.308326 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 55.0086 | ms | 0.009704 M appended, 0 overflow, 5668.652102 ns/fragment |
| coverage | slivers 0.5 px x 20 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 2.6104 | ms | 269.002473 ns/fragment, 70.811632 ns/pixel (all pixels) |
| coverage | cards 4 px: count pass (conservative raster, tile atomics only) | 5.1279 | ms | 0.015996 M fragments, max/tile 93, tiles > 1024: 0 |
| coverage | cards 4 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 5.5992 | ms | 0.015996 M appended, 0 overflow, 350.037509 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 5.7058 | ms | 0.015996 M appended, 0 overflow, 356.701675 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 5.8818 | ms | 0.015996 M appended, 0 overflow, 367.704426 ns/fragment |
| coverage | cards 4 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 0.8586 | ms | 53.675919 ns/fragment, 23.291016 ns/pixel (all pixels) |

# DesignBench 20260925_144649 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| coverage | slivers 0.5 px x 20 px: count pass (conservative raster, tile atomics only) | 55.3078 | ms | 0.009704 M fragments, max/tile 50, tiles > 1024: 0 |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 77.0459 | ms | 0.019408 M appended, 0 overflow, 3969.801113 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 77.1988 | ms | 0.009704 M appended, 0 overflow, 7955.358615 ns/fragment |
| coverage | slivers 0.5 px x 20 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 80.7529 | ms | 0.009704 M appended, 0 overflow, 8321.609646 ns/fragment |
| coverage | slivers 0.5 px x 20 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 2.6677 | ms | 274.907255 ns/fragment, 72.365994 ns/pixel (all pixels) |
| coverage | cards 4 px: count pass (conservative raster, tile atomics only) | 8.0351 | ms | 0.015996 M fragments, max/tile 93, tiles > 1024: 0 |
| coverage | cards 4 px: raster + area + mask + 24 B append (global list, one atomic per wave) | 10.4156 | ms | 0.031992 M appended, 0 overflow, 325.568892 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per fragment) | 9.9372 | ms | 0.015996 M appended, 0 overflow, 621.230308 ns/fragment |
| coverage | cards 4 px: raster + area + mask + 24 B append (tile segments, one atomic per (wave, tile)) | 7.5539 | ms | 0.015996 M appended, 0 overflow, 472.236809 ns/fragment |
| coverage | cards 4 px: tile composite (groupshared sort by pixel/depth, mask union, 2 taps/fragment) | 1.3933 | ms | 87.103026 ns/fragment, 37.795681 ns/pixel (all pixels) |

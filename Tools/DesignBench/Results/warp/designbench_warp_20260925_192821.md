# DesignBench 20260925_192821 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| bricks | camera march, 1 B voxels (1 MB), 16 steps, 32 B record | 3.6833 | ms | 99.915907 ns/pixel, 6.244744 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 1 B voxels (1 MB), 16 steps, 32 B record | 12.5141 | ms | 339.466688 ns/pixel, 21.216668 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 1 B voxels, 16 steps, 4 B T only | 2.553 | ms | 69.254557 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 1 B voxels, 16 steps, 4 B T only | 1.8497 | ms | 50.176324 ns/pixel (receiver) |
| bricks | camera march, 1 B voxels (1 MB), 32 steps, 32 B record | 5.4911 | ms | 148.955621 ns/pixel, 4.654863 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 1 B voxels (1 MB), 32 steps, 32 B record | 12.4009 | ms | 336.395942 ns/pixel, 10.512373 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 1 B voxels, 32 steps, 4 B T only | 13.2358 | ms | 359.044054 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 1 B voxels, 32 steps, 4 B T only | 12.0121 | ms | 325.849067 ns/pixel (receiver) |
| bricks | camera march, 1 B voxels (1 MB), 48 steps, 32 B record | 10.741 | ms | 291.368273 ns/pixel, 6.070172 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 1 B voxels (1 MB), 48 steps, 32 B record | 12.6838 | ms | 344.070095 ns/pixel, 7.168127 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 1 B voxels, 48 steps, 4 B T only | 11.2118 | ms | 304.139540 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 1 B voxels, 48 steps, 4 B T only | 13.6612 | ms | 370.583767 ns/pixel (receiver) |
| bricks | entry maps: 144 bricks x 256 cells x 16 steps, 1 B voxels | 1.278 | ms | 8875.000000 ns/brick |
| bricks | camera march, 8 B voxels (4 MB), 16 steps, 32 B record | 5.1488 | ms | 139.670139 ns/pixel, 8.729384 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 8 B voxels (4 MB), 16 steps, 32 B record | 1.5333 | ms | 41.593424 ns/pixel, 2.599589 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 8 B voxels, 16 steps, 4 B T only | 3.9971 | ms | 108.428277 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 8 B voxels, 16 steps, 4 B T only | 1.5916 | ms | 43.174913 ns/pixel (receiver) |
| bricks | camera march, 8 B voxels (4 MB), 32 steps, 32 B record | 9.0168 | ms | 244.596354 ns/pixel, 7.643636 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 8 B voxels (4 MB), 32 steps, 32 B record | 10.4513 | ms | 283.509657 ns/pixel, 8.859677 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 8 B voxels, 32 steps, 4 B T only | 6.8307 | ms | 185.294596 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 8 B voxels, 32 steps, 4 B T only | 12.517 | ms | 339.545356 ns/pixel (receiver) |
| bricks | camera march, 8 B voxels (4 MB), 48 steps, 32 B record | 7.4222 | ms | 201.340061 ns/pixel, 4.194585 ns/step (upper bound: early exits) |
| bricks | sun (orthographic) march, 8 B voxels (4 MB), 48 steps, 32 B record | 5.1303 | ms | 139.168294 ns/pixel, 2.899339 ns/step (upper bound: early exits) |
| bricks | sun texel march (top face down), 8 B voxels, 48 steps, 4 B T only | 11.6904 | ms | 317.122396 ns/pixel (receiver) |
| bricks | receiver sun march (ground up toward the sun), 8 B voxels, 48 steps, 4 B T only | 11.4209 | ms | 309.811740 ns/pixel (receiver) |
| bricks | entry maps: 144 bricks x 256 cells x 16 steps, 8 B voxels | 2.8496 | ms | 19788.888889 ns/brick |

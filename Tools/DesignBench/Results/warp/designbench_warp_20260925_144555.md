# DesignBench 20260925_144555 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| shade | 4K-class shading kernel: full: tile SH + air 3 + K + 8 lights | 4.5729 | ms | 124.047852 ns/pixel |
| shade | 4K-class shading kernel: per-pixel 4 probe loads instead of tile SH | 7.9577 | ms | 215.866428 ns/pixel |
| shade | 4K-class shading kernel: no air fetches | 14.0304 | ms | 380.598958 ns/pixel |
| shade | 4K-class shading kernel: no K tap | 12.7165 | ms | 344.957140 ns/pixel |
| shade | 4K-class shading kernel: no local lights | 10.5516 | ms | 286.230469 ns/pixel |
| shade | 4K-class shading kernel: inputs and sun only (no SH, air, K, lights) | 3.5756 | ms | 96.994358 ns/pixel |
| shade | 4K-class shading kernel: full + 32 extra live values | 16.391 | ms | 444.634332 ns/pixel |
| shade | 4K-class shading kernel: full + 64 extra live values | 15.1827 | ms | 411.857096 ns/pixel |
| shade | 4K-class shading kernel: full + 128 extra live values | 21.2416 | ms | 576.215278 ns/pixel |

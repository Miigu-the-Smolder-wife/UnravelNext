# Measured hardware floors (RTX 4080)

Adapter: NVIDIA GeForce RTX 4080, driver 32.0.15.9186, Agility SDK 618, DXC C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64

Every row is a GPU-timestamp measurement (median of repeated runs after a 1.5 s warm-up) unless the note says cpu. Runs since 20260925_033921; value = median over runs, n = run count, range = min..max, P0a = FLOORS_P0A.json.

Excluded: run 20260925_033921 section pso: compute PSO kernels used a fixed seed, so 'cold' hit the driver disk cache from P0a runs; re-measured with a per-run nonce in 20260925_034232.

| section | measurement | value | unit | n | range | P0a | ratio | note | runs |
|---|---|---:|---|---:|---|---:|---:|---|---|
| env | highest shader model | 104 | hex | 11 | 104..104 | 104 | 1.000 |  | 20260925_033921,20260925_034232,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| env | mesh shader tier | 10 | tier | 11 | 10..10 | 10 | 1.000 |  | 20260925_033921,20260925_034232,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| env | raytracing tier | 11 | tier | 11 | 11..11 | 11 | 1.000 |  | 20260925_033921,20260925_034232,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| env | timestamp frequency | 1e+09 | Hz | 11 | 1e+09..1e+09 | 1e+09 | 1.000 |  | 20260925_033921,20260925_034232,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| compute | fp32 fma throughput (2048 iters, 8 float4 chains, unroll 8) | 46.21 | TFLOPS | 1 |  | 44.11 | 1.048 | median of 9, best 47.632944, peak 48.7 at 2.5 GHz | 20260925_033921 |
| compute | fp32 fma throughput (512 iters, 8 float4 chains, unroll 8) | 46.2 | TFLOPS | 1 |  | 49.2 | 0.939 | median of 9, best 49.038264, peak 48.7 at 2.5 GHz | 20260925_033921 |
| memory | cached loads, each group re-reads its own 4 KB (L1-resident) | 1.295e+04 | GB/s | 1 |  | 1.311e+04 | 0.988 | SM load-path ceiling | 20260925_033921 |
| memory | copy 512 MiB read + 512 MiB write | 621.9 | GB/s | 1 |  | 608.6 | 1.022 | read+write bytes summed | 20260925_033921 |
| memory | dependent load latency, 262144 KiB working set | 238.3 | ns/load | 1 |  | 243.1 | 0.980 | single thread pointer chase | 20260925_033921 |
| memory | dependent load latency, 4096 KiB working set | 116.1 | ns/load | 1 |  | 123.4 | 0.941 | single thread pointer chase | 20260925_033921 |
| memory | dependent load latency, 64 KiB working set | 35.53 | ns/load | 1 |  | 35.44 | 1.003 | single thread pointer chase | 20260925_033921 |
| memory | random 4 B loads, 1024 MiB window | 5.14 | Gloads/s | 1 |  | 4.974 | 1.033 | 164.482510 GB/s at 32 B sectors | 20260925_033921 |
| memory | random 4 B loads, 2 MiB window | 66.33 | Gloads/s | 1 |  | 66.33 | 1.000 | 2122.623482 GB/s at 32 B sectors | 20260925_033921 |
| memory | random 4 B loads, 32 MiB window | 64.69 | Gloads/s | 1 |  | 64.76 | 0.999 | 2070.238894 GB/s at 32 B sectors | 20260925_033921 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 128 MiB window | 1391 | GB/s | 1 |  | 1385 | 1.004 | beyond L2 | 20260925_033921 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 32 MiB window | 3987 | GB/s | 1 |  | 3987 | 1.000 | L2 resident (64 MB L2) | 20260925_033921 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 512 MiB window | 793.8 | GB/s | 1 |  | 789.6 | 1.005 | beyond L2 | 20260925_033921 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 64 MiB window | 3972 | GB/s | 1 |  | 3972 | 1.000 | beyond L2 | 20260925_033921 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 8 MiB window | 4033 | GB/s | 1 |  | 4033 | 1.000 | L2 resident (64 MB L2) | 20260925_033921 |
| memory | random 64 B loads, 1024 MiB window | 5.138 | Gloads/s | 1 |  | 4.965 | 1.035 | 328.861847 GB/s useful | 20260925_033921 |
| memory | random 64 B loads, 2 MiB window | 57.04 | Gloads/s | 1 |  | 57.04 | 1.000 | 3650.395126 GB/s useful | 20260925_033921 |
| memory | random 64 B loads, 32 MiB window | 55.26 | Gloads/s | 1 |  | 55.35 | 0.998 | 3536.512648 GB/s useful | 20260925_033921 |
| memory | sequential read 1 GiB of hashed data | 697.7 | GB/s | 1 |  | 688.5 | 1.013 | true DRAM read floor | 20260925_033921 |
| memory | sequential read 1 GiB of structured data | 697.7 | GB/s | 1 |  | 694.9 | 1.004 | compression inflated | 20260925_033921 |
| memory | sequential write 1 GiB, hashed (incompressible) data | 675.2 | GB/s | 1 |  | 639 | 1.057 |  | 20260925_033921 |
| memory | sequential write 1 GiB, structured (compressible) data | 662.4 | GB/s | 1 |  | 622.7 | 1.064 |  | 20260925_033921 |
| texture | RGBA8 4096^2 coherent bilinear lod0 | 811.6 | Gsamples/s | 1 |  | 814.1 | 0.997 |  | 20260925_033921 |
| texture | RGBA8 4096^2 coherent point lod0 | 652.1 | Gsamples/s | 1 |  | 652.1 | 1.000 |  | 20260925_033921 |
| texture | RGBA8 4096^2 coherent trilinear SampleGrad | 361.6 | Gsamples/s | 1 |  | 361.6 | 1.000 |  | 20260925_033921 |
| texture | RGBA8 4096^2 incoherent bilinear lod0 | 26.4 | Gsamples/s | 1 |  | 24.79 | 1.065 |  | 20260925_033921 |
| atomics | InterlockedAdd, 1048576 addresses | 63.44 | Gatomics/s | 1 |  | 63.38 | 1.001 |  | 20260925_033921 |
| atomics | InterlockedAdd, 256 addresses | 3.171 | Gatomics/s | 1 |  | 3.036 | 1.045 |  | 20260925_033921 |
| atomics | InterlockedAdd, single address | 2.254 | Gatomics/s | 1 |  | 2.181 | 1.033 |  | 20260925_033921 |
| dispatch | 1000 x (PSO switch + constants + Dispatch(1) + UAV barrier) | 0.8858 | us/dispatch | 1 |  | 0.8858 | 1.000 |  | 20260925_033921 |
| dispatch | 1000 x Dispatch(1) + UAV barrier | 0.8858 | us/dispatch | 1 |  | 0.8847 | 1.001 | fixed cost floor of one dependent pass | 20260925_033921 |
| dispatch | 1000 x Dispatch(1) no barrier | 0.03994 | us/dispatch | 1 |  | 0.03891 | 1.026 |  | 20260925_033921 |
| dispatch | 1000 x Dispatch(1024 groups of 64) + UAV barrier | 1.417 | us/dispatch | 1 |  | 1.417 | 1.000 | 65k-thread pass with real work | 20260925_033921 |
| raster | 16.7M tris, edge 0.5 px, depth only | 32.92 | Gtris/s | 1 |  | 32.93 | 1.000 | 0.518144 ms | 20260925_033921 |
| raster | 16.7M tris, edge 0.5 px, vis id + depth | 28.38 | Gtris/s | 1 |  | 28.43 | 0.998 | 0.601056 ms | 20260925_033921 |
| raster | 16.7M tris, edge 1.0 px, depth only | 22.95 | Gtris/s | 1 |  | 23.07 | 0.994 | 0.743424 ms | 20260925_033921 |
| raster | 16.7M tris, edge 1.0 px, vis id + depth | 22.36 | Gtris/s | 1 |  | 22.51 | 0.993 | 0.762816 ms | 20260925_033921 |
| raster | 16.7M tris, edge 16.0 px, depth only | 3.417 | Gtris/s | 1 |  | 3.42 | 0.999 | 4.993024 ms | 20260925_033921 |
| raster | 16.7M tris, edge 16.0 px, vis id + depth | 3.366 | Gtris/s | 1 |  | 3.368 | 1.000 | 5.067776 ms | 20260925_033921 |
| raster | 16.7M tris, edge 2 px, 50% culled by SV_CullPrimitive | 21.61 | Gtris/s (emitted) | 1 |  | 21.58 | 1.001 | 0.789504 ms | 20260925_033921 |
| raster | 16.7M tris, edge 2.0 px, depth only | 12.89 | Gtris/s | 1 |  | 12.92 | 0.998 | 1.323008 ms | 20260925_033921 |
| raster | 16.7M tris, edge 2.0 px, vis id + depth | 12.84 | Gtris/s | 1 |  | 12.9 | 0.995 | 1.329152 ms | 20260925_033921 |
| raster | 16.7M tris, edge 32.0 px, depth only | 2.4 | Gtris/s | 1 |  | 2.402 | 0.999 | 7.107584 ms | 20260925_033921 |
| raster | 16.7M tris, edge 32.0 px, vis id + depth | 2.358 | Gtris/s | 1 |  | 2.361 | 0.999 | 7.235584 ms | 20260925_033921 |
| raster | 16.7M tris, edge 4.0 px, depth only | 9.318 | Gtris/s | 1 |  | 9.339 | 0.998 | 1.830912 ms | 20260925_033921 |
| raster | 16.7M tris, edge 4.0 px, vis id + depth | 9.095 | Gtris/s | 1 |  | 9.119 | 0.997 | 1.875840 ms | 20260925_033921 |
| raster | 16.7M tris, edge 8.0 px, depth only | 6.058 | Gtris/s | 1 |  | 6.071 | 0.998 | 2.816000 ms | 20260925_033921 |
| raster | 16.7M tris, edge 8.0 px, vis id + depth | 5.961 | Gtris/s | 1 |  | 5.969 | 0.999 | 2.862080 ms | 20260925_033921 |
| raster | 174k meshlets x 1 triangle (edge 2 px) | 1149 | Mtris/s | 1 |  | 1149 | 1.000 | per-meshlet overhead bound | 20260925_033921 |
| raster | 32 full-screen layers back-to-front (all pass, vis id + depth write) | 279.9 | Gpix/s | 1 |  | 279.9 | 1.000 | 0.948224 ms | 20260925_033921 |
| raster | 32 full-screen layers back-to-front, depth only | 1112 | Gpix/s | 1 |  | 1112 | 1.000 | 0.238592 ms | 20260925_033921 |
| raster | 32 full-screen layers front-to-back (early-z reject) | 3014 | Gpix/s | 1 |  | 3016 | 0.999 | 0.088064 ms | 20260925_033921 |
| raster | 32 full-screen layers front-to-back, depth only (early-z reject) | 3869 | Gpix/s | 1 |  | 3869 | 1.000 | 0.068608 ms | 20260925_033921 |
| raster | 4K depth clear alone | 0.004096 | ms | 1 |  | 0.004096 | 1.000 |  | 20260925_033921 |
| raster | meshlet launch only (174k meshlets, zero output) | 1149 | Mmeshlets/s | 1 |  | 1149 | 1.000 | 0.151552 ms incl. depth clear | 20260925_033921 |
| coverage | 0.5M tris 0.25 px: conservative raster, trivial PS | 0.6079 | ms | 1 |  | 0.6062 | 1.003 |  | 20260925_033921 |
| coverage | 0.5M tris 0.25 px: standard raster + area + append (centre-sample pixels only) | 0.3369 | ms | 1 |  | 0.3348 | 1.006 | 1.304510 M pixels touched by centre sampling vs 15.230712 M true fragments | 20260925_033921 |
| coverage | 0.5M tris 0.50 px: conservative raster, trivial PS | 0.6308 | ms | 1 |  | 0.6328 | 0.997 |  | 20260925_033921 |
| coverage | 0.5M tris 0.50 px: standard raster + area + append (centre-sample pixels only) | 0.3991 | ms | 1 |  | 0.3983 | 1.002 | 2.609021 M pixels touched by centre sampling vs 16.620044 M true fragments | 20260925_033921 |
| coverage | 0.5M tris 1.00 px: conservative raster, trivial PS | 0.6758 | ms | 1 |  | 0.6786 | 0.996 |  | 20260925_033921 |
| coverage | 0.5M tris 1.00 px: standard raster + area + append (centre-sample pixels only) | 0.511 | ms | 1 |  | 0.51 | 1.002 | 5.217903 M pixels touched by centre sampling vs 19.400160 M true fragments | 20260925_033921 |
| coverage | 1049k slivers 0.25 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.062 | ms | 1 |  | 3.053 | 1.003 | 60.912056 M fragments (29.045132 per tri), 19.894458 G frags/s, area check 0.976702 | 20260925_033921 |
| coverage | 1049k slivers 0.50 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.198 | ms | 1 |  | 3.202 | 0.999 | 66.470106 M fragments (31.695416 per tri), 20.787499 G frags/s, area check 0.987319 | 20260925_033921 |
| coverage | 1049k slivers 1.00 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.458 | ms | 1 |  | 3.457 | 1.000 | 77.593619 M fragments (36.999521 per tri), 22.438768 G frags/s, area check 0.991564 | 20260925_033921 |
| coverage | 2.1M tris 0.25 px: conservative raster, trivial PS | 2.448 | ms | 1 |  | 2.425 | 1.010 |  | 20260925_033921 |
| coverage | 2.1M tris 0.25 px: standard raster + area + append (centre-sample pixels only) | 1.329 | ms | 1 |  | 1.326 | 1.002 | 5.220250 M pixels touched by centre sampling vs 60.912056 M true fragments | 20260925_033921 |
| coverage | 2.1M tris 0.50 px: conservative raster, trivial PS | 2.595 | ms | 1 |  | 2.585 | 1.004 |  | 20260925_033921 |
| coverage | 2.1M tris 0.50 px: standard raster + area + append (centre-sample pixels only) | 1.58 | ms | 1 |  | 1.613 | 0.980 | 10.439807 M pixels touched by centre sampling vs 66.470106 M true fragments | 20260925_033921 |
| coverage | 2.1M tris 1.00 px: conservative raster, trivial PS | 2.7 | ms | 1 |  | 2.694 | 1.002 |  | 20260925_033921 |
| coverage | 2.1M tris 1.00 px: standard raster + area + append (centre-sample pixels only) | 2.018 | ms | 1 |  | 2.011 | 1.004 | 20.873505 M pixels touched by centre sampling vs 77.593619 M true fragments | 20260925_033921 |
| coverage | 262k slivers 0.25 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.7752 | ms | 1 |  | 0.7731 | 1.003 | 15.230712 M fragments (29.050278 per tri), 19.648272 G frags/s, area check 0.976637 | 20260925_033921 |
| coverage | 262k slivers 0.50 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.8079 | ms | 1 |  | 0.809 | 0.999 | 16.620044 M fragments (31.700218 per tri), 20.570991 G frags/s, area check 0.987257 | 20260925_033921 |
| coverage | 262k slivers 1.00 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.8735 | ms | 1 |  | 0.8745 | 0.999 | 19.400160 M fragments (37.002869 per tri), 22.210397 G frags/s, area check 0.991489 | 20260925_033921 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: >=3 identities | 1.869e-05 | fraction | 1 |  | 7.113e-06 | 2.627 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: >=5 identities | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: >=9 identities | 0 | fraction | 1 |  | 0 |  |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: missed >=4 of 16 (>=25% pixel area) | 3.581e-05 | fraction | 1 |  | 4.581e-06 | 7.816 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: missed >=8 of 16 (>=50% pixel area) | 9.645e-06 | fraction | 1 |  | 4.823e-07 | 20.000 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: pixels with >=2 identities in 16 sub-samples | 0.001813 | fraction | 1 |  | 0.001595 | 1.137 | 50.980864 ms for 133M rays | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [card foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 4.232e-05 | fraction | 1 |  | 7.475e-06 | 5.661 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=3 identities | 0.002063 | fraction | 1 |  | 0.002063 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=5 identities | 1.085e-05 | fraction | 1 |  | 1.085e-05 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=9 identities | 0 | fraction | 1 |  | 0 |  |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.001163 | fraction | 1 |  | 0.001163 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.0001365 | fraction | 1 |  | 0.0001365 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.009397 | fraction | 1 |  | 0.009397 | 1.000 | 42.329088 ms for 133M rays | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.002237 | fraction | 1 |  | 0.002237 | 1.000 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [card foliage]: >=3 identities | 9.645e-07 | fraction | 1 |  | 9.645e-07 | 1.000 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: >=5 identities | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: >=9 identities | 0 | fraction | 1 |  | 0 |  |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: missed >=4 of 16 (>=25% pixel area) | 0 | fraction | 1 |  | 0 |  |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: missed >=8 of 16 (>=50% pixel area) | 0 | fraction | 1 |  | 0 |  |  | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: pixels with >=2 identities in 16 sub-samples | 0.001198 | fraction | 1 |  | 0.001198 | 1.000 | 45.740032 ms for 133M rays | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [card foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034527 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=3 identities | 9.645e-07 | fraction | 1 |  | 9.645e-07 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=5 identities | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=9 identities | 0 | fraction | 1 |  | 0 |  |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0 | fraction | 1 |  | 0 |  |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0 | fraction | 1 |  | 0 |  |  | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.001198 | fraction | 1 |  | 0.001198 | 1.000 | 36.236288 ms for 133M rays | 20260925_034522 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: >=3 identities | 0.01 | fraction | 1 |  | 0.01013 | 0.987 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: >=5 identities | 0.0002755 | fraction | 1 |  | 0.0002713 | 1.016 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: >=9 identities | 1.206e-07 | fraction | 1 |  | 1.206e-07 | 1.000 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: missed >=4 of 16 (>=25% pixel area) | 0.002524 | fraction | 1 |  | 0.002363 | 1.068 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: missed >=8 of 16 (>=50% pixel area) | 0.0002844 | fraction | 1 |  | 0.0002599 | 1.094 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: pixels with >=2 identities in 16 sub-samples | 0.0773 | fraction | 1 |  | 0.07719 | 1.001 | 42.763264 ms for 133M rays | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [card foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.005847 | fraction | 1 |  | 0.005605 | 1.043 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=3 identities | 0.142 | fraction | 1 |  | 0.142 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=5 identities | 0.05651 | fraction | 1 |  | 0.05651 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=9 identities | 0.008013 | fraction | 1 |  | 0.008013 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.08362 | fraction | 1 |  | 0.08362 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.03196 | fraction | 1 |  | 0.03196 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.2266 | fraction | 1 |  | 0.2266 | 1.000 | 52.352000 ms for 133M rays | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.1329 | fraction | 1 |  | 0.1329 | 1.000 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: >=3 identities | 0.009742 | fraction | 1 |  | 0.01001 | 0.973 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: >=5 identities | 0.0002921 | fraction | 1 |  | 0.0002855 | 1.023 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: >=9 identities | 0 | fraction | 1 |  | 1.206e-07 | 0.000 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: missed >=4 of 16 (>=25% pixel area) | 0.002372 | fraction | 1 |  | 0.002392 | 0.991 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: missed >=8 of 16 (>=50% pixel area) | 0.0002655 | fraction | 1 |  | 0.0002599 | 1.021 |  | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: pixels with >=2 identities in 16 sub-samples | 0.0713 | fraction | 1 |  | 0.07187 | 0.992 | 38.670336 ms for 133M rays | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [card foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.005597 | fraction | 1 |  | 0.005642 | 0.992 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034527 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=3 identities | 0.1194 | fraction | 1 |  | 0.1194 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=5 identities | 0.05387 | fraction | 1 |  | 0.05387 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=9 identities | 0.007977 | fraction | 1 |  | 0.007977 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.06241 | fraction | 1 |  | 0.06241 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.02752 | fraction | 1 |  | 0.02752 | 1.000 |  | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.1829 | fraction | 1 |  | 0.1829 | 1.000 | 48.626688 ms for 133M rays | 20260925_034522 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.1014 | fraction | 1 |  | 0.1014 | 1.000 | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_034522 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 1 taps | 0.1864 | ms | 1 |  | 0.1864 | 1.000 | 0.008988 ns per light lookup, 0.008988 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 5 taps | 0.6093 | ms | 1 |  | 0.6072 | 1.003 | 0.029383 ns per light lookup, 0.005877 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 9 taps | 1.055 | ms | 1 |  | 1.046 | 1.009 | 0.050864 ns per light lookup, 0.005652 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 1 taps | 0.08499 | ms | 1 |  | 0.08602 | 0.988 | 0.010247 ns per light lookup, 0.010247 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 5 taps | 0.171 | ms | 1 |  | 0.17 | 1.006 | 0.020617 ns per light lookup, 0.004123 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 9 taps | 0.2826 | ms | 1 |  | 0.2816 | 1.004 | 0.034074 ns per light lookup, 0.003786 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, scattered (worst) depth, sun + 1.5 local lights, 5 taps | 2.072 | ms | 1 |  | 2.07 | 1.001 | 0.099901 ns per light lookup, 0.019980 ns per tap | 20260925_033921 |
| vsm | VSM lookup 4K, scattered (worst) depth, sun, 5 taps | 0.7762 | ms | 1 |  | 0.767 | 1.012 | 0.093580 ns per light lookup, 0.018716 ns per tap | 20260925_033921 |
| shade | shading 4K, G-buffer in / out + sun only (memory floor of the kernel) | 0.3471 | ms | 1 |  | 0.3594 | 0.966 | 0.041852 ns per pixel | 20260925_033921 |
| shade | shading 4K, fused: same, page-table entries prefetched before atlas taps | 2.28 | ms | 1 |  | 2.262 | 1.008 | 0.274938 ns per pixel | 20260925_033921 |
| shade | shading 4K, fused: sun + 4 locals GGX, 3 shadowed x 5 VSM taps (in-kernel), 4 probes, froxel | 2.485 | ms | 1 |  | 2.474 | 1.005 | 0.299630 ns per pixel | 20260925_033921 |
| shade | shading 4K, fused: sun + 8 locals (3 shadowed in-kernel) | 2.346 | ms | 1 |  | 2.35 | 0.998 | 0.282840 ns per pixel | 20260925_033921 |
| shade | shading 4K, split, no light loop (sun only) + probes + froxel | 0.3799 | ms | 1 |  | 0.3717 | 1.022 | 0.045802 ns per pixel | 20260925_033921 |
| shade | shading 4K, split, sun only + froxel (no probes) | 0.3574 | ms | 1 |  | 0.3615 | 0.989 | 0.043086 ns per pixel | 20260925_033921 |
| shade | shading 4K, split: sun + 16 locals | 0.7209 | ms | 1 |  | 0.7188 | 1.003 | 0.086914 ns per pixel | 20260925_033921 |
| shade | shading 4K, split: sun + 8 locals | 0.426 | ms | 1 |  | 0.4239 | 1.005 | 0.051358 ns per pixel | 20260925_033921 |
| shade | shading 4K, split: visibility from a 4 B/pixel buffer (separate VSM pass), sun + 4 locals, probes, froxel | 0.3727 | ms | 1 |  | 0.3697 | 1.008 | 0.044938 ns per pixel | 20260925_033921 |
| rtas | BLAS build city 36k tris | 0.3123 | ms | 10 | 0.3041..0.3297 | 0.3092 | 1.010 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | BLAS build terrain 2.1M tris (fast trace) | 3.434 | ms | 10 | 3.241..3.656 | 3.595 | 0.955 | 139 MB | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | BLAS build tree 3k tris (fast trace) | 0.2519 | ms | 4 | 0.2509..0.254 |  |  | 229 KB | 20260925_033921,20260925_034354,20260925_034405,20260925_034527 |
| rtas | BLAS build tree 80k tris (fast trace) | 0.4357 | ms | 6 | 0.4157..0.4536 | 0.4332 | 1.006 | 5484 KB | 20260925_034517,20260925_034522,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | BLAS build tree with OMM leaves (fast trace) | 0.4521 | ms | 10 | 0.2591..0.4977 | 0.4598 | 0.983 | 6265 KB | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | BLAS full build 60k-tri skinned mesh (allow update), per mesh, 64 in flight | 92.38 | us | 10 | 90.67..96.4 | 93.44 | 0.989 | 3710 KB each | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | BLAS refit 60k-tri skinned mesh, per mesh, 64 in flight | 15.33 | us | 10 | 15.26..18.46 | 15.31 | 1.001 | 256 characters = 3.919872 ms (expected, linear) | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS build 1.1M instances (fast trace + allow update) | 3.247 | ms | 10 | 2.948..3.36 | 2.974 | 1.092 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS build 1.1M instances (fast trace) | 3.273 | ms | 10 | 2.976..3.441 | 3.413 | 0.959 | 212 MB | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS build 100k instances (fast build) | 0.2939 | ms | 10 | 0.2918..0.2949 | 0.2949 | 0.997 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS build 100k instances (fast trace + allow update) | 0.2954 | ms | 10 | 0.2939..0.297 | 0.2959 | 0.998 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS build 100k instances (fast trace) | 0.2949 | ms | 10 | 0.2939..0.2959 | 0.2949 | 1.000 | 28 MB | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS refit 1.1M instances | 1.406 | ms | 10 | 1.401..1.582 | 1.406 | 1.000 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rtas | TLAS refit 100k instances | 0.05325 | ms | 10 | 0.05222..0.05325 | 0.05222 | 1.020 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.846 | Grays/s | 3 | 1.808..2.009 | 1.991 | 0.927 | 2.087936 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.018 | Grays/s | 5 | 0.964..1.025 | 1.003 | 1.016 | 4.239360 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| incoherent short (same, tMax 2 m) | 3.492 | Grays/s | 3 | 3.388..3.525 | 3.531 | 0.989 | 1.201152 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| incoherent short (same, tMax 2 m) [thin foliage] | 2.907 | Grays/s | 5 | 2.811..2.919 | 2.913 | 0.998 | 1.446912 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| primary 3840x2160 camera | 3.354 | Grays/s | 3 | 3.138..3.355 | 2.961 | 1.133 | 2.471936 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| primary 3840x2160 camera [thin foliage] | 3.581 | Grays/s | 5 | 3.347..3.879 | 3.877 | 0.924 | 2.316288 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| sun shadow from terrain (0.25 deg jitter) | 2.947 | Grays/s | 3 | 2.94..2.988 | 2.972 | 0.991 | 1.423360 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.062 | Grays/s | 5 | 1.025..1.081 | 1.153 | 0.922 | 4.091904 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| incoherent long (canopy origins, sphere dirs, 2 km) | 2.059 | Grays/s | 3 | 1.894..2.079 | 1.813 | 1.136 | 2.036736 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 0.9939 | Grays/s | 5 | 0.986..1.044 | 1.03 | 0.965 | 4.253696 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| incoherent short (same, tMax 2 m) | 3.596 | Grays/s | 3 | 3.577..3.628 | 3.631 | 0.990 | 1.172480 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| incoherent short (same, tMax 2 m) [thin foliage] | 2.943 | Grays/s | 5 | 2.913..2.96 | 2.957 | 0.995 | 1.427456 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| primary 3840x2160 camera | 3.131 | Grays/s | 3 | 3.124..3.35 | 2.953 | 1.060 | 2.476032 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| primary 3840x2160 camera [thin foliage] | 3.876 | Grays/s | 5 | 3.872..3.879 | 3.877 | 1.000 | 2.139136 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| sun shadow from terrain (0.25 deg jitter) | 2.938 | Grays/s | 3 | 2.909..2.955 | 2.674 | 1.099 | 1.441792 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays OMM forced 2-state (no any-hit) \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.078 | Grays/s | 5 | 1.066..1.097 | 1.028 | 1.048 | 3.923968 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays alpha any-hit shader \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.839 | Grays/s | 3 | 1.68..1.856 | 1.809 | 1.017 | 2.280448 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays alpha any-hit shader \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.186 | Grays/s | 5 | 1.158..1.289 | 1.284 | 0.923 | 3.622912 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays alpha any-hit shader \| incoherent short (same, tMax 2 m) | 2.951 | Grays/s | 3 | 2.583..2.966 | 2.917 | 1.012 | 1.421312 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays alpha any-hit shader \| incoherent short (same, tMax 2 m) [thin foliage] | 2.783 | Grays/s | 5 | 2.487..2.807 | 2.783 | 1.000 | 1.686528 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays alpha any-hit shader \| primary 3840x2160 camera | 3.165 | Grays/s | 3 | 3.031..3.388 | 2.998 | 1.056 | 2.736128 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays alpha any-hit shader \| primary 3840x2160 camera [thin foliage] | 3.587 | Grays/s | 5 | 3.576..3.879 | 3.883 | 0.924 | 2.312192 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays alpha any-hit shader \| sun shadow from terrain (0.25 deg jitter) | 3.139 | Grays/s | 3 | 2.798..3.248 | 3.148 | 0.997 | 1.291264 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays alpha any-hit shader \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.465 | Grays/s | 5 | 1.397..1.562 | 1.554 | 0.943 | 3.003392 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays alpha any-hit shader, OMM-enabled pipeline (flag cost) \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.809 | Grays/s | 3 | 1.72..1.825 | 1.61 | 1.124 | 2.298880 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays alpha any-hit shader, OMM-enabled pipeline (flag cost) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.217 | Grays/s | 5 | 1.215..1.309 | 1.122 | 1.085 | 3.435520 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.9 | Grays/s | 3 | 1.879..1.911 | 1.716 | 1.107 | 2.232320 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.206 | Grays/s | 5 | 1.147..1.289 | 1.273 | 0.948 | 3.476480 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| incoherent short (same, tMax 2 m) | 3.17 | Grays/s | 3 | 3.165..3.175 | 3.23 | 0.981 | 1.323008 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| incoherent short (same, tMax 2 m) [thin foliage] | 2.889 | Grays/s | 5 | 2.549..2.897 | 2.862 | 1.009 | 1.645568 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| primary 3840x2160 camera | 3.163 | Grays/s | 3 | 3.107..3.34 | 2.971 | 1.064 | 2.483200 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER \| primary 3840x2160 camera [thin foliage] | 3.581 | Grays/s | 5 | 3.551..3.883 | 3.877 | 0.924 | 2.335744 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.717 | Grays/s | 3 | 1.617..1.721 | 1.636 | 1.049 | 2.443264 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.123 | Grays/s | 5 | 1.07..1.129 | 1.046 | 1.073 | 3.726336 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| incoherent short (same, tMax 2 m) | 2.79 | Grays/s | 3 | 2.788..2.804 | 2.862 | 0.975 | 1.504256 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| incoherent short (same, tMax 2 m) [thin foliage] | 2.541 | Grays/s | 5 | 2.26..2.554 | 2.518 | 1.009 | 1.659904 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| primary 3840x2160 camera | 2.765 | Grays/s | 3 | 2.585..2.771 | 2.698 | 1.025 | 2.993152 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) \| primary 3840x2160 camera [thin foliage] | 3.127 | Grays/s | 5 | 3.12..3.136 | 3.343 | 0.936 | 2.658304 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays opaque \| incoherent long (canopy origins, sphere dirs, 2 km) | 2.035 | Grays/s | 3 | 2.02..2.051 | 2.015 | 1.010 | 2.076672 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays opaque \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.343 | Grays/s | 5 | 1.211..1.361 | 1.336 | 1.006 | 3.131392 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays opaque \| incoherent short (same, tMax 2 m) | 3.325 | Grays/s | 3 | 3.317..3.336 | 3.233 | 1.028 | 1.261568 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays opaque \| incoherent short (same, tMax 2 m) [thin foliage] | 2.972 | Grays/s | 5 | 2.949..2.983 | 2.943 | 1.010 | 1.420288 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays opaque \| primary 3840x2160 camera | 3.153 | Grays/s | 3 | 3.148..3.376 | 2.975 | 1.060 | 2.456576 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays opaque \| primary 3840x2160 camera [thin foliage] | 3.6 | Grays/s | 5 | 3.578..3.883 | 3.876 | 0.929 | 2.314240 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays opaque \| sun shadow from terrain (0.25 deg jitter) | 3.115 | Grays/s | 3 | 2.731..3.117 | 3.098 | 1.005 | 1.346560 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays opaque \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.45 | Grays/s | 5 | 1.441..1.454 | 1.4 | 1.036 | 2.891776 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | DispatchRays trivial hit, NVAPI SER (reorder overhead) \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.722 | Grays/s | 3 | 1.705..1.733 | 1.928 | 0.893 | 2.460672 ms | 20260925_033921,20260925_034354,20260925_034405 |
| rays | DispatchRays trivial hit, NVAPI SER (reorder overhead) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.186 | Grays/s | 5 | 1.157..1.198 | 1.153 | 1.029 | 3.535872 ms | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha any-hit loop, +1M grass \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.08 | Grays/s | 3 | 1.038..1.084 | 0.9858 | 1.095 | 3.884032 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha any-hit loop, +1M grass \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 0.6405 | Grays/s | 5 | 0.6321..0.6435 | 0.6185 | 1.035 | 6.548480 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha first-hit, +1M grass \| sun shadow from terrain (0.25 deg jitter) | 1.487 | Grays/s | 3 | 1.479..1.702 | 1.462 | 1.017 | 2.464768 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha first-hit, +1M grass \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.6459 | Grays/s | 5 | 0.6445..0.657 | 0.6503 | 0.993 | 6.504448 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha-test any-hit loop \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.164 | Grays/s | 3 | 1.161..1.167 | 1.046 | 1.113 | 3.612672 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha-test any-hit loop \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 0.7072 | Grays/s | 5 | 0.696..0.7241 | 0.6917 | 1.022 | 5.936128 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha-test any-hit loop \| incoherent short (same, tMax 2 m) | 2.263 | Grays/s | 3 | 2.066..2.293 | 1.919 | 1.179 | 1.828864 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha-test any-hit loop \| incoherent short (same, tMax 2 m) [thin foliage] | 2.49 | Grays/s | 5 | 2.201..2.496 | 2.442 | 1.019 | 1.684480 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha-test any-hit loop \| primary 3840x2160 camera | 3.1 | Grays/s | 3 | 3.088..3.318 | 3.245 | 0.955 | 2.499584 ms for 8 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha-test any-hit loop \| primary 3840x2160 camera [thin foliage] | 3.911 | Grays/s | 5 | 3.631..3.917 | 3.917 | 0.999 | 2.117632 ms for 8 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha-test any-hit loop \| sun shadow from terrain (0.25 deg jitter) | 0.8949 | Grays/s | 3 | 0.8605..0.8996 | 0.8084 | 1.107 | 4.662272 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha-test any-hit loop \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.5705 | Grays/s | 5 | 0.5612..0.5877 | 0.5597 | 1.019 | 7.373824 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery alpha-test first-hit \| sun shadow from terrain (0.25 deg jitter) | 1.659 | Grays/s | 3 | 1.642..1.881 | 1.527 | 1.087 | 2.230272 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery alpha-test first-hit \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.7339 | Grays/s | 5 | 0.7182..0.7585 | 0.7133 | 1.029 | 5.529600 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery no-flags, non-opaque leaves ignored (traversal only) \| incoherent long (canopy origins, sphere dirs, 2 km) | 2.99 | Grays/s | 3 | 2.988..3.034 | 2.905 | 1.029 | 1.402880 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery no-flags, non-opaque leaves ignored (traversal only) \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.542 | Grays/s | 5 | 1.508..1.55 | 1.462 | 1.054 | 2.723840 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest \| incoherent long (canopy origins, sphere dirs, 2 km) | 2.04 | Grays/s | 3 | 1.931..2.098 | 1.803 | 1.131 | 2.056192 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.244 | Grays/s | 5 | 1.195..1.309 | 1.175 | 1.058 | 3.215360 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest \| incoherent short (same, tMax 2 m) | 3.388 | Grays/s | 3 | 3.344..3.428 | 3.18 | 1.065 | 1.254400 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest \| incoherent short (same, tMax 2 m) [thin foliage] | 2.773 | Grays/s | 5 | 2.56..2.876 | 2.786 | 0.995 | 1.638400 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest \| primary 3840x2160 camera | 2.957 | Grays/s | 3 | 2.944..3.127 | 2.848 | 1.038 | 2.652160 ms for 8 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest \| primary 3840x2160 camera [thin foliage] | 3.733 | Grays/s | 5 | 3.453..3.741 | 3.733 | 1.000 | 2.219008 ms for 8 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest \| sun shadow from terrain (0.25 deg jitter) | 1.449 | Grays/s | 3 | 1.382..1.458 | 1.352 | 1.071 | 3.035136 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.9303 | Grays/s | 5 | 0.8916..0.9322 | 0.8782 | 1.059 | 4.548608 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest, +1M grass \| incoherent long (canopy origins, sphere dirs, 2 km) | 1.867 | Grays/s | 3 | 1.706..1.881 | 1.912 | 0.976 | 2.229248 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest, +1M grass \| incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.11 | Grays/s | 5 | 1.07..1.116 | 1.014 | 1.095 | 3.758080 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque closest, +1M grass \| primary 3840x2160 camera | 2.716 | Grays/s | 3 | 2.7..2.846 | 2.638 | 1.030 | 2.914304 ms for 8 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque closest, +1M grass \| primary 3840x2160 camera [thin foliage] | 3.067 | Grays/s | 5 | 3.059..3.257 | 3.066 | 1.000 | 2.704384 ms for 8 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque first-hit \| sun shadow from terrain (0.25 deg jitter) | 3.17 | Grays/s | 3 | 2.874..3.215 | 3.115 | 1.018 | 1.323008 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque first-hit \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.45 | Grays/s | 5 | 1.435..1.503 | 1.476 | 0.983 | 2.922496 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | RayQuery opaque first-hit, +1M grass \| sun shadow from terrain (0.25 deg jitter) | 2.711 | Grays/s | 3 | 2.627..2.723 | 2.811 | 0.964 | 1.540096 ms for 4 M rays | 20260925_033921,20260925_034354,20260925_034405 |
| rays | RayQuery opaque first-hit, +1M grass \| sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.17 | Grays/s | 5 | 1.149..1.178 | 1.177 | 0.994 | 3.586048 ms for 4 M rays | 20260925_034517,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| rays | sanity: primary alpha-tested hit fraction | 1 | fraction | 10 | 1..1 | 1 | 1.000 |  | 20260925_033921,20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| mpm | 1M particles: G2P (27-node gather, APIC, 64 B write) | 0.3604 | ms | 1 |  | 0.3604 | 1.000 | 0.343750 ns per particle | 20260925_033921 |
| mpm | 1M particles: P2G via global atomics (27 nodes x 4 channels), incl. clear | 2.056 | ms | 1 |  | 2.26 | 0.910 | 1.892578 ns per particle | 20260925_033921 |
| mpm | 1M particles: P2G via groupshared block accumulation + halo atomics, incl. clear | 0.2263 | ms | 1 |  | 0.2263 | 1.000 | 0.147461 ns per particle | 20260925_033921 |
| mpm | 1M particles: block sort (histogram + prefix + scatter, 64 B particles) | 0.9738 | ms | 1 |  | 0.9687 | 1.005 |  | 20260925_033921 |
| mpm | 1M particles: full step (clear + sort + block P2G + grid + G2P) | 1.627 | ms | 1 |  | 1.811 | 0.898 | 1.551758 ns per particle-step | 20260925_033921 |
| mpm | 1M particles: grid clear (128^3 x 16 B dense) | 0.07168 | ms | 1 |  | 0.07066 | 1.014 |  | 20260925_033921 |
| mpm | 1M particles: grid update (dense 128^3) | 0.03379 | ms | 1 |  | 0.03584 | 0.943 | sparse grids scale with active nodes (131072 here) | 20260925_033921 |
| mpm | 2M particles: G2P (27-node gather, APIC, 64 B write) | 0.7281 | ms | 1 |  | 0.726 | 1.003 | 0.347168 ns per particle | 20260925_033921 |
| mpm | 2M particles: P2G via global atomics (27 nodes x 4 channels), incl. clear | 4.424 | ms | 1 |  | 4.799 | 0.922 | 2.075195 ns per particle | 20260925_033921 |
| mpm | 2M particles: P2G via groupshared block accumulation + halo atomics, incl. clear | 0.3656 | ms | 1 |  | 0.3656 | 1.000 | 0.140137 ns per particle | 20260925_033921 |
| mpm | 2M particles: block sort (histogram + prefix + scatter, 64 B particles) | 2.199 | ms | 1 |  | 2.388 | 0.921 |  | 20260925_033921 |
| mpm | 2M particles: full step (clear + sort + block P2G + grid + G2P) | 3.622 | ms | 1 |  | 3.739 | 0.969 | 1.727051 ns per particle-step | 20260925_033921 |
| mpm | 2M particles: grid clear (128^3 x 16 B dense) | 0.07168 | ms | 1 |  | 0.07168 | 1.000 |  | 20260925_033921 |
| mpm | 2M particles: grid update (dense 128^3) | 0.02867 | ms | 1 |  | 0.03686 | 0.778 | sparse grids scale with active nodes (262144 here) | 20260925_033921 |
| pso | compute PSO, 128 stages, DXIL 53 KB: cold create | 126.3 | ms | 1 |  | 167.9 | 0.752 | dxc 93.714900 ms, warm recreate 3.998900 ms, 2.397539 ms/KB | 20260925_034232 |
| pso | compute PSO, 2048 stages, DXIL 871 KB: cold create | 3680 | ms | 1 |  | 4863 | 0.757 | dxc 4085.325600 ms, warm recreate 70.908100 ms, 4.226344 ms/KB | 20260925_034232 |
| pso | compute PSO, 32 stages, DXIL 14 KB: cold create | 27.72 | ms | 1 |  | 28.99 | 0.956 | dxc 24.003700 ms, warm recreate 1.435700 ms, 1.976441 ms/KB | 20260925_034232 |
| pso | compute PSO, 512 stages, DXIL 205 KB: cold create | 612.6 | ms | 1 |  | 775.3 | 0.790 | dxc 537.550100 ms, warm recreate 15.498800 ms, 2.991437 ms/KB | 20260925_034232 |
| pso | compute PSO, 8192 stages, DXIL 3501 KB: cold create | 17835 | ms | 1 |  | 1.792e+04 | 0.995 | dxc 37805.380300 ms, warm recreate 292.970200 ms, 5.094236 ms/KB | 20260925_034232 |
| pso | raytracing state object create, 5 shaders (cpu) | 27.47 | ms | 9 | 26.65..28.58 | 26.24 | 1.047 | dxc 13.654500 ms | 20260925_034354,20260925_034405,20260925_034517,20260925_034522,20260925_034527,20260925_034608,20260925_034614,20260925_034620,20260925_034626 |
| async | bandwidth read alone (wall) | 1.613 | ms | 1 |  | 1.833 | 0.880 |  | 20260925_033921 |
| async | fma compute alone (wall) | 5.835 | ms | 1 |  | 6.074 | 0.961 |  | 20260925_033921 |
| async | raster + bandwidth read concurrent (wall) | 3.052 | ms | 1 |  | 3.236 | 0.943 | serial sum 3.049100 ms, overlap gain -0.088550 % | 20260925_033921 |
| async | raster + fma concurrent (wall) | 6.87 | ms | 1 |  | 7.538 | 0.911 | serial sum 7.270400 ms, overlap gain 5.505887 % | 20260925_033921 |
| async | raster 16.7M tris alone (wall) | 1.436 | ms | 1 |  | 1.625 | 0.883 |  | 20260925_033921 |

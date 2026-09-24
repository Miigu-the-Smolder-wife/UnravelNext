# Measured hardware floors (RTX 4080)

Adapter: NVIDIA GeForce RTX 4080, driver 32.0.15.9186, Agility SDK 618, DXC C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64

Every row is a GPU-timestamp measurement (median of repeated runs after a 1.5 s warm-up) unless the note says cpu. The run column is the JSON/log stamp the number came from.

| section | measurement | value | unit | note | run |
|---|---|---:|---|---|---|
| env | highest shader model | 104 | hex |  | 20260925_031731 |
| env | mesh shader tier | 10 | tier |  | 20260925_031731 |
| env | raytracing tier | 11 | tier |  | 20260925_031731 |
| env | timestamp frequency | 1e+09 | Hz |  | 20260925_031731 |
| compute | fp32 fma throughput (2048 iters, 8 chains) | 13.42 | TFLOPS | median of 5, min 13.821917 | 20260925_015010 |
| compute | fp32 fma throughput (2048 iters, 8 float4 chains, unroll 8) | 44.11 | TFLOPS | median of 9, best 46.587202, peak 48.7 at 2.5 GHz | 20260925_020109 |
| compute | fp32 fma throughput (512 iters, 8 chains) | 13.83 | TFLOPS | median of 5, min 15.595832 | 20260925_015010 |
| compute | fp32 fma throughput (512 iters, 8 float4 chains, unroll 8) | 49.2 | TFLOPS | median of 9, best 49.218089, peak 48.7 at 2.5 GHz | 20260925_020109 |
| memory | cached loads, each group re-reads its own 4 KB (L1-resident) | 1.311e+04 | GB/s | SM load-path ceiling | 20260925_022638 |
| memory | copy 512 MiB read + 512 MiB write | 608.6 | GB/s | read+write bytes summed | 20260925_022638 |
| memory | dependent load latency, 262144 KiB working set | 243.1 | ns/load | single thread pointer chase | 20260925_022638 |
| memory | dependent load latency, 4096 KiB working set | 123.4 | ns/load | single thread pointer chase | 20260925_022638 |
| memory | dependent load latency, 64 KiB working set | 35.44 | ns/load | single thread pointer chase | 20260925_022638 |
| memory | random 4 B loads, 1024 MiB window | 4.974 | Gloads/s | 159.152463 GB/s at 32 B sectors | 20260925_022638 |
| memory | random 4 B loads, 2 MiB window | 66.33 | Gloads/s | 2122.623482 GB/s at 32 B sectors | 20260925_022638 |
| memory | random 4 B loads, 32 MiB window | 64.76 | Gloads/s | 2072.284585 GB/s at 32 B sectors | 20260925_022638 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 128 MiB window | 1385 | GB/s | beyond L2 | 20260925_022638 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 32 MiB window | 3987 | GB/s | L2 resident (64 MB L2) | 20260925_022638 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 512 MiB window | 789.6 | GB/s | beyond L2 | 20260925_022638 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 64 MiB window | 3972 | GB/s | beyond L2 | 20260925_022638 |
| memory | random 4 KB chunk reads, 1 GiB traffic, 8 MiB window | 4033 | GB/s | L2 resident (64 MB L2) | 20260925_022638 |
| memory | random 64 B loads, 1024 MiB window | 4.965 | Gloads/s | 317.774377 GB/s useful | 20260925_022638 |
| memory | random 64 B loads, 2 MiB window | 57.04 | Gloads/s | 3650.395126 GB/s useful | 20260925_022638 |
| memory | random 64 B loads, 32 MiB window | 55.35 | Gloads/s | 3542.486486 GB/s useful | 20260925_022638 |
| memory | read 1 GiB traffic inside 16 MiB window | 1.295e+04 | GB/s | L2 = 64 MB on AD103 | 20260925_020109 |
| memory | read 1 GiB traffic inside 32 MiB window | 1.295e+04 | GB/s | L2 = 64 MB on AD103 | 20260925_020109 |
| memory | read 1 GiB traffic inside 4 MiB window | 1.295e+04 | GB/s | L2 = 64 MB on AD103 | 20260925_020109 |
| memory | read 1 GiB traffic inside 48 MiB window | 1.295e+04 | GB/s | L2 = 64 MB on AD103 | 20260925_020109 |
| memory | read 1 GiB traffic inside 96 MiB window | 1.263e+04 | GB/s | L2 = 64 MB on AD103 | 20260925_020109 |
| memory | sequential read 1 GiB (uint4 loads) | 184.7 | GB/s |  | 20260925_015010 |
| memory | sequential read 1 GiB of hashed data | 688.5 | GB/s | true DRAM read floor | 20260925_022638 |
| memory | sequential read 1 GiB of structured data | 694.9 | GB/s | compression inflated | 20260925_022638 |
| memory | sequential write 1 GiB (uint4 stores) | 659.9 | GB/s |  | 20260925_015010 |
| memory | sequential write 1 GiB, hashed (incompressible) data | 639 | GB/s |  | 20260925_022638 |
| memory | sequential write 1 GiB, structured (compressible) data | 622.7 | GB/s |  | 20260925_022638 |
| texture | RGBA8 4096^2 coherent bilinear lod0 | 814.1 | Gsamples/s |  | 20260925_022638 |
| texture | RGBA8 4096^2 coherent point lod0 | 652.1 | Gsamples/s |  | 20260925_022638 |
| texture | RGBA8 4096^2 coherent trilinear SampleGrad | 361.6 | Gsamples/s |  | 20260925_022638 |
| texture | RGBA8 4096^2 incoherent bilinear lod0 | 24.79 | Gsamples/s |  | 20260925_022638 |
| atomics | InterlockedAdd, 1048576 addresses | 63.38 | Gatomics/s |  | 20260925_022638 |
| atomics | InterlockedAdd, 256 addresses | 3.036 | Gatomics/s |  | 20260925_022638 |
| atomics | InterlockedAdd, single address | 2.181 | Gatomics/s |  | 20260925_022638 |
| dispatch | 1000 x (PSO switch + constants + Dispatch(1) + UAV barrier) | 0.8858 | us/dispatch |  | 20260925_022638 |
| dispatch | 1000 x Dispatch(1) + UAV barrier | 0.8847 | us/dispatch | fixed cost floor of one dependent pass | 20260925_022638 |
| dispatch | 1000 x Dispatch(1) no barrier | 0.03891 | us/dispatch |  | 20260925_022638 |
| dispatch | 1000 x Dispatch(1024 groups of 64) + UAV barrier | 1.417 | us/dispatch | 65k-thread pass with real work | 20260925_022638 |
| raster | 16.7M tris, edge 0.5 px, depth only | 32.93 | Gtris/s | 0.518080 ms | 20260925_022638 |
| raster | 16.7M tris, edge 0.5 px, vis id + depth | 28.43 | Gtris/s | 0.600064 ms | 20260925_022638 |
| raster | 16.7M tris, edge 1.0 px, depth only | 23.07 | Gtris/s | 0.739328 ms | 20260925_022638 |
| raster | 16.7M tris, edge 1.0 px, vis id + depth | 22.51 | Gtris/s | 0.757760 ms | 20260925_022638 |
| raster | 16.7M tris, edge 16.0 px, depth only | 3.42 | Gtris/s | 4.987904 ms | 20260925_022638 |
| raster | 16.7M tris, edge 16.0 px, vis id + depth | 3.368 | Gtris/s | 5.065728 ms | 20260925_022638 |
| raster | 16.7M tris, edge 2 px, 50% culled by SV_CullPrimitive | 21.58 | Gtris/s (emitted) | 0.790528 ms | 20260925_022638 |
| raster | 16.7M tris, edge 2.0 px, depth only | 12.92 | Gtris/s | 1.319936 ms | 20260925_022638 |
| raster | 16.7M tris, edge 2.0 px, vis id + depth | 12.9 | Gtris/s | 1.321984 ms | 20260925_022638 |
| raster | 16.7M tris, edge 32.0 px, depth only | 2.402 | Gtris/s | 7.101440 ms | 20260925_022638 |
| raster | 16.7M tris, edge 32.0 px, vis id + depth | 2.361 | Gtris/s | 7.226368 ms | 20260925_022638 |
| raster | 16.7M tris, edge 4.0 px, depth only | 9.339 | Gtris/s | 1.826816 ms | 20260925_022638 |
| raster | 16.7M tris, edge 4.0 px, vis id + depth | 9.119 | Gtris/s | 1.870848 ms | 20260925_022638 |
| raster | 16.7M tris, edge 8.0 px, depth only | 6.071 | Gtris/s | 2.809856 ms | 20260925_022638 |
| raster | 16.7M tris, edge 8.0 px, vis id + depth | 5.969 | Gtris/s | 2.857984 ms | 20260925_022638 |
| raster | 174k meshlets x 1 triangle (edge 2 px) | 1149 | Mtris/s | per-meshlet overhead bound | 20260925_022638 |
| raster | 32 full-screen layers back-to-front (all pass, vis id + depth write) | 279.9 | Gpix/s | 0.948224 ms | 20260925_022638 |
| raster | 32 full-screen layers back-to-front, depth only | 1112 | Gpix/s | 0.238592 ms | 20260925_022638 |
| raster | 32 full-screen layers front-to-back (early-z reject) | 3016 | Gpix/s | 0.088000 ms | 20260925_022638 |
| raster | 32 full-screen layers front-to-back, depth only (early-z reject) | 3869 | Gpix/s | 0.068608 ms | 20260925_022638 |
| raster | 4K depth clear alone | 0.004096 | ms |  | 20260925_022638 |
| raster | meshlet launch only (174k meshlets, zero output) | 1149 | Mmeshlets/s | 0.151552 ms incl. depth clear | 20260925_022638 |
| coverage | 0.5M tris 0.25 px: conservative raster, trivial PS | 0.6062 | ms |  | 20260925_030411 |
| coverage | 0.5M tris 0.25 px: standard raster + area + append (centre-sample pixels only) | 0.3348 | ms | 1.304510 M pixels touched by centre sampling vs 15.230712 M true fragments | 20260925_030411 |
| coverage | 0.5M tris 0.50 px: conservative raster, trivial PS | 0.6328 | ms |  | 20260925_030411 |
| coverage | 0.5M tris 0.50 px: standard raster + area + append (centre-sample pixels only) | 0.3983 | ms | 2.609021 M pixels touched by centre sampling vs 16.620044 M true fragments | 20260925_030411 |
| coverage | 0.5M tris 1.00 px: conservative raster, trivial PS | 0.6786 | ms |  | 20260925_030411 |
| coverage | 0.5M tris 1.00 px: standard raster + area + append (centre-sample pixels only) | 0.51 | ms | 5.217903 M pixels touched by centre sampling vs 19.400160 M true fragments | 20260925_030411 |
| coverage | 1049k slivers 0.25 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.053 | ms | 60.912056 M fragments (29.045132 per tri), 19.954522 G frags/s, area check 0.195374 | 20260925_030411 |
| coverage | 1049k slivers 0.50 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.202 | ms | 66.470106 M fragments (31.695416 per tri), 20.758623 G frags/s, area check 0.095375 | 20260925_030411 |
| coverage | 1049k slivers 1.00 px x 20 px (2.1M tris): conservative raster + exact area + append | 3.457 | ms | 77.593619 M fragments (36.999521 per tri), 22.445207 G frags/s, area check 0.045374 | 20260925_030411 |
| coverage | 2.1M tris 0.25 px: conservative raster, trivial PS | 2.425 | ms |  | 20260925_030411 |
| coverage | 2.1M tris 0.25 px: standard raster + area + append (centre-sample pixels only) | 1.326 | ms | 5.220250 M pixels touched by centre sampling vs 60.912056 M true fragments | 20260925_030411 |
| coverage | 2.1M tris 0.50 px: conservative raster, trivial PS | 2.585 | ms |  | 20260925_030411 |
| coverage | 2.1M tris 0.50 px: standard raster + area + append (centre-sample pixels only) | 1.613 | ms | 10.439807 M pixels touched by centre sampling vs 66.470106 M true fragments | 20260925_030411 |
| coverage | 2.1M tris 1.00 px: conservative raster, trivial PS | 2.694 | ms |  | 20260925_030411 |
| coverage | 2.1M tris 1.00 px: standard raster + area + append (centre-sample pixels only) | 2.011 | ms | 20.873505 M pixels touched by centre sampling vs 77.593619 M true fragments | 20260925_030411 |
| coverage | 262k slivers 0.25 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.7731 | ms | 15.230712 M fragments (29.050278 per tri), 19.701952 G frags/s, area check 0.195306 | 20260925_030411 |
| coverage | 262k slivers 0.50 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.809 | ms | 16.620044 M fragments (31.700218 per tri), 20.544952 G frags/s, area check 0.195307 | 20260925_030411 |
| coverage | 262k slivers 1.00 px x 20 px (0.5M tris): conservative raster + exact area + append | 0.8745 | ms | 19.400160 M fragments (37.002869 per tri), 22.184390 G frags/s, area check 0.195305 | 20260925_030411 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=3 identities | 0.002063 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=5 identities | 1.085e-05 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: >=9 identities | 0 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.001163 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.0001365 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.009397 | fraction | 43.800576 ms for 133M rays | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.002237 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees + 1M grass: >=3 identities | 7.113e-06 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: >=5 identities | 1.206e-07 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: >=9 identities | 0 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: missed >=4 of 16 (>=25% pixel area) | 4.581e-06 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: missed >=8 of 16 (>=50% pixel area) | 4.823e-07 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: pixels with >=2 identities in 16 sub-samples | 0.001595 | fraction | 50.741248 ms for 133M rays | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees + 1M grass: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 7.475e-06 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=3 identities | 9.645e-07 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=5 identities | 1.206e-07 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: >=9 identities | 0 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0 | fraction |  | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.001198 | fraction | 37.839872 ms for 133M rays | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 1.206e-07 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_031725 |
| edges | city view (camera 0,0), 100k trees: >=3 identities | 9.645e-07 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: >=5 identities | 1.206e-07 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: >=9 identities | 0 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: missed >=4 of 16 (>=25% pixel area) | 0 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: missed >=8 of 16 (>=50% pixel area) | 0 | fraction |  | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: pixels with >=2 identities in 16 sub-samples | 0.001198 | fraction | 46.853120 ms for 133M rays | 20260925_030659 |
| edges | city view (camera 0,0), 100k trees: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 1.206e-07 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=3 identities | 0.142 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=5 identities | 0.05651 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: >=9 identities | 0.008013 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.08362 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.03196 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.2266 | fraction | 56.796160 ms for 133M rays | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.1329 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: >=3 identities | 0.01013 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: >=5 identities | 0.0002713 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: >=9 identities | 1.206e-07 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: missed >=4 of 16 (>=25% pixel area) | 0.002363 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: missed >=8 of 16 (>=50% pixel area) | 0.0002599 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: pixels with >=2 identities in 16 sub-samples | 0.07719 | fraction | 40.594432 ms for 133M rays | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees + 1M grass: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.005605 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=3 identities | 0.1194 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=5 identities | 0.05387 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: >=9 identities | 0.007977 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: missed >=4 of 16 (>=25% pixel area) | 0.06241 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: missed >=8 of 16 (>=50% pixel area) | 0.02752 | fraction |  | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: pixels with >=2 identities in 16 sub-samples | 0.1829 | fraction | 49.253376 ms for 133M rays | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees [thin foliage]: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.1014 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_031725 |
| edges | forest view (camera 600,600 looking +x), 100k trees: >=3 identities | 0.01001 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: >=5 identities | 0.0002855 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: >=9 identities | 1.206e-07 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: missed >=4 of 16 (>=25% pixel area) | 0.002392 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: missed >=8 of 16 (>=50% pixel area) | 0.0002599 | fraction |  | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: pixels with >=2 identities in 16 sub-samples | 0.07187 | fraction | 36.802560 ms for 133M rays | 20260925_030659 |
| edges | forest view (camera 600,600 looking +x), 100k trees: pixels with sub-samples missed by the 3x3 centre set (>=1 of 16) | 0.005642 | fraction | invisible to a 1-spp visibility buffer + 3x3 identity test | 20260925_030659 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 1 taps | 0.1864 | ms | 0.008988 ns per light lookup, 0.008988 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 5 taps | 0.6072 | ms | 0.029284 ns per light lookup, 0.005857 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, coherent scene depth, sun + 1.5 local lights, 9 taps | 1.046 | ms | 0.050420 ns per light lookup, 0.005602 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 1 taps | 0.08602 | ms | 0.010370 ns per light lookup, 0.010370 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 5 taps | 0.17 | ms | 0.020494 ns per light lookup, 0.004099 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, coherent scene depth, sun, 9 taps | 0.2816 | ms | 0.033951 ns per light lookup, 0.003772 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, scattered (worst) depth, sun + 1.5 local lights, 5 taps | 2.07 | ms | 0.099802 ns per light lookup, 0.019960 ns per tap | 20260925_030411 |
| vsm | VSM lookup 4K, scattered (worst) depth, sun, 5 taps | 0.767 | ms | 0.092469 ns per light lookup, 0.018494 ns per tap | 20260925_030411 |
| shade | fused shading 4K: sun + 16 local lights (GGX), 3 shadowed x 5 VSM taps, 4 probes, froxel, 16 B in / 8 B out | 2.347 | ms | 0.282963 ns per pixel | 20260925_025946 |
| shade | fused shading 4K: sun + 4 local lights (GGX), 3 shadowed x 5 VSM taps, 4 probes, froxel, 16 B in / 8 B out | 2.476 | ms | 0.298519 ns per pixel | 20260925_025946 |
| shade | fused shading 4K: sun + 8 local lights (GGX), 3 shadowed x 5 VSM taps, 4 probes, froxel, 16 B in / 8 B out | 2.565 | ms | 0.309259 ns per pixel | 20260925_025946 |
| shade | shading 4K, G-buffer in / out + sun only (memory floor of the kernel) | 0.3594 | ms | 0.043333 ns per pixel | 20260925_030411 |
| shade | shading 4K, fused: same, page-table entries prefetched before atlas taps | 2.262 | ms | 0.272716 ns per pixel | 20260925_030411 |
| shade | shading 4K, fused: sun + 4 locals GGX, 3 shadowed x 5 VSM taps (in-kernel), 4 probes, froxel | 2.474 | ms | 0.298272 ns per pixel | 20260925_030411 |
| shade | shading 4K, fused: sun + 8 locals (3 shadowed in-kernel) | 2.35 | ms | 0.283333 ns per pixel | 20260925_030411 |
| shade | shading 4K, split, no light loop (sun only) + probes + froxel | 0.3717 | ms | 0.044815 ns per pixel | 20260925_030411 |
| shade | shading 4K, split, sun only + froxel (no probes) | 0.3615 | ms | 0.043580 ns per pixel | 20260925_030411 |
| shade | shading 4K, split: sun + 16 locals | 0.7188 | ms | 0.086667 ns per pixel | 20260925_030411 |
| shade | shading 4K, split: sun + 8 locals | 0.4239 | ms | 0.051111 ns per pixel | 20260925_030411 |
| shade | shading 4K, split: visibility from a 4 B/pixel buffer (separate VSM pass), sun + 4 locals, probes, froxel | 0.3697 | ms | 0.044568 ns per pixel | 20260925_030411 |
| rtas | BLAS build city 36k tris | 0.3092 | ms |  | 20260925_031731 |
| rtas | BLAS build terrain 2.1M tris (fast trace) | 3.595 | ms | 139 MB | 20260925_031731 |
| rtas | BLAS build tree 3.3k tris (fast trace) | 0.2499 | ms | 229 KB | 20260925_030659 |
| rtas | BLAS build tree 80k tris (fast trace) | 0.4332 | ms | 5484 KB | 20260925_031731 |
| rtas | BLAS build tree with OMM leaves (fast trace) | 0.4598 | ms | 6265 KB | 20260925_031731 |
| rtas | BLAS full build 60k-tri skinned mesh (allow update), per mesh, 64 in flight | 93.44 | us | 3710 KB each | 20260925_031731 |
| rtas | BLAS refit 60k-tri skinned mesh, per mesh, 64 in flight | 15.31 | us | 256 characters = 3.919872 ms (expected, linear) | 20260925_031731 |
| rtas | TLAS build 1.1M instances (fast trace + allow update) | 2.974 | ms |  | 20260925_031731 |
| rtas | TLAS build 1.1M instances (fast trace) | 3.413 | ms | 212 MB | 20260925_031731 |
| rtas | TLAS build 100k instances (fast build) | 0.2949 | ms |  | 20260925_031731 |
| rtas | TLAS build 100k instances (fast trace + allow update) | 0.2959 | ms |  | 20260925_031731 |
| rtas | TLAS build 100k instances (fast trace) | 0.2949 | ms | 28 MB | 20260925_031731 |
| rtas | TLAS refit 1.1M instances | 1.406 | ms |  | 20260925_031731 |
| rtas | TLAS refit 100k instances | 0.05222 | ms |  | 20260925_031731 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.991 | Grays/s | 2.106368 ms | 20260925_022638 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.003 | Grays/s | 4.183040 ms | 20260925_031731 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | incoherent short (same, tMax 2 m) | 3.531 | Grays/s | 1.187840 ms | 20260925_022638 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | incoherent short (same, tMax 2 m) [thin foliage] | 2.913 | Grays/s | 1.439744 ms | 20260925_031731 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | primary 3840x2160 camera | 2.961 | Grays/s | 2.801664 ms | 20260925_022638 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | primary 3840x2160 camera [thin foliage] | 3.877 | Grays/s | 2.139136 ms | 20260925_031731 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | sun shadow from terrain (0.25 deg jitter) | 2.972 | Grays/s | 1.411072 ms | 20260925_022638 |
| rays | DispatchRays OMM 4-state (unknown -> any-hit) | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.153 | Grays/s | 3.639296 ms | 20260925_031731 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.813 | Grays/s | 2.313216 ms | 20260925_022638 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.03 | Grays/s | 4.071424 ms | 20260925_031731 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | incoherent short (same, tMax 2 m) | 3.631 | Grays/s | 1.155072 ms | 20260925_022638 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | incoherent short (same, tMax 2 m) [thin foliage] | 2.957 | Grays/s | 1.418240 ms | 20260925_031731 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | primary 3840x2160 camera | 2.953 | Grays/s | 2.808832 ms | 20260925_022638 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | primary 3840x2160 camera [thin foliage] | 3.877 | Grays/s | 2.139136 ms | 20260925_031731 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | sun shadow from terrain (0.25 deg jitter) | 2.674 | Grays/s | 1.568768 ms | 20260925_022638 |
| rays | DispatchRays OMM forced 2-state (no any-hit) | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.028 | Grays/s | 4.078592 ms | 20260925_031731 |
| rays | DispatchRays alpha any-hit shader | incoherent long (canopy origins, sphere dirs, 2 km) | 1.809 | Grays/s | 2.318336 ms | 20260925_022638 |
| rays | DispatchRays alpha any-hit shader | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.284 | Grays/s | 3.265536 ms | 20260925_031731 |
| rays | DispatchRays alpha any-hit shader | incoherent short (same, tMax 2 m) | 2.917 | Grays/s | 1.437696 ms | 20260925_022638 |
| rays | DispatchRays alpha any-hit shader | incoherent short (same, tMax 2 m) [thin foliage] | 2.783 | Grays/s | 1.507328 ms | 20260925_031731 |
| rays | DispatchRays alpha any-hit shader | primary 3840x2160 camera | 2.998 | Grays/s | 2.766848 ms | 20260925_022638 |
| rays | DispatchRays alpha any-hit shader | primary 3840x2160 camera [thin foliage] | 3.883 | Grays/s | 2.136064 ms | 20260925_031731 |
| rays | DispatchRays alpha any-hit shader | sun shadow from terrain (0.25 deg jitter) | 3.148 | Grays/s | 1.332224 ms | 20260925_022638 |
| rays | DispatchRays alpha any-hit shader | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.554 | Grays/s | 2.699264 ms | 20260925_031731 |
| rays | DispatchRays alpha any-hit shader, OMM-enabled pipeline (flag cost) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.61 | Grays/s | 2.605056 ms | 20260925_022638 |
| rays | DispatchRays alpha any-hit shader, OMM-enabled pipeline (flag cost) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.122 | Grays/s | 3.738624 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | incoherent long (canopy origins, sphere dirs, 2 km) | 1.716 | Grays/s | 2.444288 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.273 | Grays/s | 3.294208 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | incoherent short (same, tMax 2 m) | 3.23 | Grays/s | 1.298432 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | incoherent short (same, tMax 2 m) [thin foliage] | 2.862 | Grays/s | 1.465344 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | primary 3840x2160 camera | 2.971 | Grays/s | 2.791424 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading (16 paths, 256 MB textures), no SER | primary 3840x2160 camera [thin foliage] | 3.877 | Grays/s | 2.139136 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.636 | Grays/s | 2.563072 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.046 | Grays/s | 4.009984 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | incoherent short (same, tMax 2 m) | 2.862 | Grays/s | 1.465344 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | incoherent short (same, tMax 2 m) [thin foliage] | 2.518 | Grays/s | 1.666048 ms | 20260925_031731 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | primary 3840x2160 camera | 2.698 | Grays/s | 3.074048 ms | 20260925_022914 |
| rays | DispatchRays divergent hit shading, NVAPI SER (reorder by material) | primary 3840x2160 camera [thin foliage] | 3.343 | Grays/s | 2.481152 ms | 20260925_031731 |
| rays | DispatchRays heavy divergent hit shading (8 textures), no SER | incoherent long (canopy origins, sphere dirs, 2 km) | 1.913 | Grays/s | 2.192384 ms | 20260925_022638 |
| rays | DispatchRays heavy divergent hit shading (8 textures), no SER | primary 3840x2160 camera | 2.97 | Grays/s | 2.792448 ms | 20260925_022638 |
| rays | DispatchRays heavy divergent hit shading, NVAPI SER (reorder by instance) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.563 | Grays/s | 2.682880 ms | 20260925_022638 |
| rays | DispatchRays heavy divergent hit shading, NVAPI SER (reorder by instance) | primary 3840x2160 camera | 2.864 | Grays/s | 2.895872 ms | 20260925_022638 |
| rays | DispatchRays opaque trivial hit (reference) | incoherent long (canopy origins, sphere dirs, 2 km) | 2.092 | Grays/s | 2.004992 ms | 20260925_022914 |
| rays | DispatchRays opaque trivial hit (reference) | incoherent short (same, tMax 2 m) | 3.391 | Grays/s | 1.236992 ms | 20260925_022914 |
| rays | DispatchRays opaque trivial hit (reference) | primary 3840x2160 camera | 3.306 | Grays/s | 2.508800 ms | 20260925_022914 |
| rays | DispatchRays opaque | incoherent long (canopy origins, sphere dirs, 2 km) | 2.015 | Grays/s | 2.081792 ms | 20260925_022638 |
| rays | DispatchRays opaque | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.336 | Grays/s | 3.139584 ms | 20260925_031731 |
| rays | DispatchRays opaque | incoherent short (same, tMax 2 m) | 3.233 | Grays/s | 1.297408 ms | 20260925_022638 |
| rays | DispatchRays opaque | incoherent short (same, tMax 2 m) [thin foliage] | 2.943 | Grays/s | 1.425408 ms | 20260925_031731 |
| rays | DispatchRays opaque | primary 3840x2160 camera | 2.975 | Grays/s | 2.788352 ms | 20260925_022638 |
| rays | DispatchRays opaque | primary 3840x2160 camera [thin foliage] | 3.876 | Grays/s | 2.140160 ms | 20260925_031731 |
| rays | DispatchRays opaque | sun shadow from terrain (0.25 deg jitter) | 3.098 | Grays/s | 1.353728 ms | 20260925_022638 |
| rays | DispatchRays opaque | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.4 | Grays/s | 2.996224 ms | 20260925_031731 |
| rays | DispatchRays trivial hit, NVAPI SER (reorder overhead) | incoherent long (canopy origins, sphere dirs, 2 km) | 1.928 | Grays/s | 2.174976 ms | 20260925_022914 |
| rays | DispatchRays trivial hit, NVAPI SER (reorder overhead) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.153 | Grays/s | 3.637248 ms | 20260925_031731 |
| rays | RayQuery alpha any-hit loop, +1M grass | incoherent long (canopy origins, sphere dirs, 2 km) | 0.9858 | Grays/s | 4.254720 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha any-hit loop, +1M grass | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 0.6185 | Grays/s | 6.780928 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery alpha first-hit, +1M grass | sun shadow from terrain (0.25 deg jitter) | 1.462 | Grays/s | 2.869248 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha first-hit, +1M grass | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.6503 | Grays/s | 6.450176 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery alpha-test any-hit loop | incoherent long (canopy origins, sphere dirs, 2 km) | 1.046 | Grays/s | 4.008960 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha-test any-hit loop | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 0.6917 | Grays/s | 6.064128 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery alpha-test any-hit loop | incoherent short (same, tMax 2 m) | 1.919 | Grays/s | 2.185216 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha-test any-hit loop | incoherent short (same, tMax 2 m) [thin foliage] | 2.442 | Grays/s | 1.717248 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery alpha-test any-hit loop | primary 3840x2160 camera | 3.245 | Grays/s | 2.555904 ms for 8 M rays | 20260925_022638 |
| rays | RayQuery alpha-test any-hit loop | primary 3840x2160 camera [thin foliage] | 3.917 | Grays/s | 2.117632 ms for 8 M rays | 20260925_031731 |
| rays | RayQuery alpha-test any-hit loop | sun shadow from terrain (0.25 deg jitter) | 0.8084 | Grays/s | 5.188608 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha-test any-hit loop | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.5597 | Grays/s | 7.493632 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery alpha-test first-hit | sun shadow from terrain (0.25 deg jitter) | 1.527 | Grays/s | 2.747392 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery alpha-test first-hit | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.7133 | Grays/s | 5.879808 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery no-flags, non-opaque leaves ignored (traversal only) | incoherent long (canopy origins, sphere dirs, 2 km) | 2.905 | Grays/s | 1.443840 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery no-flags, non-opaque leaves ignored (traversal only) | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.462 | Grays/s | 2.868224 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque closest | incoherent long (canopy origins, sphere dirs, 2 km) | 1.803 | Grays/s | 2.326528 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque closest | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.175 | Grays/s | 3.568640 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque closest | incoherent short (same, tMax 2 m) | 3.18 | Grays/s | 1.318912 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque closest | incoherent short (same, tMax 2 m) [thin foliage] | 2.786 | Grays/s | 1.505280 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque closest | primary 3840x2160 camera | 2.848 | Grays/s | 2.912256 ms for 8 M rays | 20260925_022638 |
| rays | RayQuery opaque closest | primary 3840x2160 camera [thin foliage] | 3.733 | Grays/s | 2.222080 ms for 8 M rays | 20260925_031731 |
| rays | RayQuery opaque closest | sun shadow from terrain (0.25 deg jitter) | 1.352 | Grays/s | 3.101696 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque closest | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 0.8782 | Grays/s | 4.775936 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque closest, +1M grass | incoherent long (canopy origins, sphere dirs, 2 km) | 1.912 | Grays/s | 2.193408 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque closest, +1M grass | incoherent long (canopy origins, sphere dirs, 2 km) [thin foliage] | 1.014 | Grays/s | 4.136960 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque closest, +1M grass | primary 3840x2160 camera | 2.638 | Grays/s | 3.144704 ms for 8 M rays | 20260925_022638 |
| rays | RayQuery opaque closest, +1M grass | primary 3840x2160 camera [thin foliage] | 3.066 | Grays/s | 2.705408 ms for 8 M rays | 20260925_031731 |
| rays | RayQuery opaque first-hit | sun shadow from terrain (0.25 deg jitter) | 3.115 | Grays/s | 1.346560 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque first-hit | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.476 | Grays/s | 2.842624 ms for 4 M rays | 20260925_031731 |
| rays | RayQuery opaque first-hit, +1M grass | sun shadow from terrain (0.25 deg jitter) | 2.811 | Grays/s | 1.491968 ms for 4 M rays | 20260925_022638 |
| rays | RayQuery opaque first-hit, +1M grass | sun shadow from terrain (0.25 deg jitter) [thin foliage] | 1.177 | Grays/s | 3.563520 ms for 4 M rays | 20260925_031731 |
| rays | sanity: primary alpha-tested hit fraction | 1 | fraction |  | 20260925_031731 |
| mpm | 1M particles: G2P (27-node gather, APIC, 64 B write) | 0.3604 | ms | 0.343750 ns per particle | 20260925_025946 |
| mpm | 1M particles: P2G via global atomics (27 nodes x 4 channels), incl. clear | 2.26 | ms | 2.087891 ns per particle | 20260925_025946 |
| mpm | 1M particles: P2G via groupshared block accumulation + halo atomics, incl. clear | 0.2263 | ms | 0.148438 ns per particle | 20260925_025946 |
| mpm | 1M particles: block sort (histogram + prefix + scatter, 64 B particles) | 0.9687 | ms |  | 20260925_025946 |
| mpm | 1M particles: full step (clear + sort + block P2G + grid + G2P) | 1.811 | ms | 1.727539 ns per particle-step | 20260925_025946 |
| mpm | 1M particles: grid clear (128^3 x 16 B dense) | 0.07066 | ms |  | 20260925_025946 |
| mpm | 1M particles: grid update (dense 128^3) | 0.03584 | ms | sparse grids scale with active nodes (131072 here) | 20260925_025946 |
| mpm | 2M particles: G2P (27-node gather, APIC, 64 B write) | 0.726 | ms | 0.346191 ns per particle | 20260925_025946 |
| mpm | 2M particles: P2G via global atomics (27 nodes x 4 channels), incl. clear | 4.799 | ms | 2.254395 ns per particle | 20260925_025946 |
| mpm | 2M particles: P2G via groupshared block accumulation + halo atomics, incl. clear | 0.3656 | ms | 0.140137 ns per particle | 20260925_025946 |
| mpm | 2M particles: block sort (histogram + prefix + scatter, 64 B particles) | 2.388 | ms |  | 20260925_025946 |
| mpm | 2M particles: full step (clear + sort + block P2G + grid + G2P) | 3.739 | ms | 1.782715 ns per particle-step | 20260925_025946 |
| mpm | 2M particles: grid clear (128^3 x 16 B dense) | 0.07168 | ms |  | 20260925_025946 |
| mpm | 2M particles: grid update (dense 128^3) | 0.03686 | ms | sparse grids scale with active nodes (262144 here) | 20260925_025946 |
| pso | compute PSO, 128 stages, DXIL 53 KB: cold create | 167.9 | ms | dxc 128.784700 ms, warm recreate 3.899400 ms, 3.190339 ms/KB | 20260925_022638 |
| pso | compute PSO, 2048 stages, DXIL 871 KB: cold create | 4863 | ms | dxc 3485.903300 ms, warm recreate 102.026900 ms, 5.585626 ms/KB | 20260925_022638 |
| pso | compute PSO, 32 stages, DXIL 14 KB: cold create | 28.99 | ms | dxc 28.302800 ms, warm recreate 1.586100 ms, 2.065889 ms/KB | 20260925_022638 |
| pso | compute PSO, 32768 stages, DXIL 14098 KB: cold create | 136490 | ms | dxc 382049.666800 ms, warm recreate 1729.920900 ms, 9.681428 ms/KB | 20260925_022638 |
| pso | compute PSO, 512 stages, DXIL 205 KB: cold create | 775.3 | ms | dxc 633.993000 ms, warm recreate 15.453000 ms, 3.785247 ms/KB | 20260925_022638 |
| pso | compute PSO, 8192 stages, DXIL 3501 KB: cold create | 1.792e+04 | ms | dxc 27027.723300 ms, warm recreate 296.495500 ms, 5.117318 ms/KB | 20260925_022638 |
| pso | mesh depth-only graphics PSO create (cpu) | 0.0093 | ms |  | 20260925_022638 |
| pso | mesh+pixel graphics PSO create (cpu) | 1.425 | ms |  | 20260925_022638 |
| pso | raytracing state object create, 5 shaders (cpu) | 26.24 | ms | dxc 13.304600 ms | 20260925_031731 |
| async | bandwidth read alone (wall) | 1.833 | ms |  | 20260925_022638 |
| async | fma compute alone (wall) | 6.074 | ms |  | 20260925_022638 |
| async | raster + bandwidth read concurrent (wall) | 3.236 | ms | serial sum 3.458100 ms, overlap gain 6.413926 % | 20260925_022638 |
| async | raster + fma concurrent (wall) | 7.538 | ms | serial sum 7.698900 ms, overlap gain 2.088610 % | 20260925_022638 |
| async | raster 16.7M tris alone (wall) | 1.625 | ms |  | 20260925_022638 |
| experiment | fma variant 0 | 26 | TFLOPS | 10.570752 ms, DXIL 3304 B | 20260925_015612 |
| experiment | fma variant 1 | 42.89 | TFLOPS | 6.408192 ms, DXIL 6600 B | 20260925_015612 |
| experiment | fma variant 2 | 27.62 | TFLOPS | 9.951232 ms, DXIL 6932 B | 20260925_015612 |
| experiment | fma variant 3 | 40.44 | TFLOPS | 6.797312 ms, DXIL 4012 B | 20260925_015612 |
| experiment | fma variant 4 | 43.91 | TFLOPS | 6.260736 ms, DXIL 6572 B | 20260925_015612 |
| experiment | fma variant 5 | 14.51 | TFLOPS | 18.947072 ms, DXIL 7464 B | 20260925_015612 |
| experiment | read variant 0, per-thread 16 x 16 B, 1 GiB | 693.5 | GB/s | 1.548288 ms | 20260925_015612 |
| experiment | read variant 0, per-thread 64 x 16 B, 1 GiB | 606.1 | GB/s | 1.771520 ms | 20260925_015612 |
| experiment | read variant 1, per-thread 16 x 16 B, 1 GiB | 556 | GB/s | 1.931264 ms | 20260925_015612 |
| experiment | read variant 1, per-thread 64 x 16 B, 1 GiB | 693 | GB/s | 1.549312 ms | 20260925_015612 |
| experiment | read variant 2, per-thread 16 x 16 B, 1 GiB | 693 | GB/s | 1.549312 ms | 20260925_015612 |
| experiment | read variant 2, per-thread 64 x 16 B, 1 GiB | 692.6 | GB/s | 1.550336 ms | 20260925_015612 |
| experiment | read variant 3, per-thread 16 x 16 B, 1 GiB | 556.3 | GB/s | 1.930240 ms | 20260925_015612 |
| experiment | read variant 3, per-thread 64 x 16 B, 1 GiB | 667.5 | GB/s | 1.608704 ms | 20260925_015612 |
| experiment | read variant 4, per-thread 16 x 16 B, 1 GiB | 680.5 | GB/s | 1.577984 ms | 20260925_015612 |
| experiment | read variant 4, per-thread 64 x 16 B, 1 GiB | 687.6 | GB/s | 1.561600 ms | 20260925_015612 |
| experiment | read variant 5, per-thread 16 x 16 B, 1 GiB | 557.2 | GB/s | 1.927168 ms | 20260925_015612 |
| experiment | read variant 5, per-thread 64 x 16 B, 1 GiB | 692.6 | GB/s | 1.550336 ms | 20260925_015612 |

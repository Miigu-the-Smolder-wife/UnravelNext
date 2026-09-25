# DesignBench 20260926_053200 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| upload | copy queue alone: 2 x 16 MB in one submission | 13.1456 | ms | 2.552522 GB/s |
| upload | copy queue alone: 2 submissions of 16 MB in 4 MB pieces, fence each | 11.9622 | ms | 2.805039 GB/s (includes per-submission sync) |
| upload | kernel alone: DRAM streaming read 512 MB | 4.6887 | ms | 7.156447 GB/s |
| upload | overlapped: DRAM streaming read 512 MB while the copy queue uploads 0.0 GB | 3.0251 | ms | kernel x0.645189 (3 dispatches); copy 1.200864 GB/s during overlap |
| upload | kernel alone: L2-resident ALU (WarmCS 2048 iterations) | 1068.78 | ms |  |
| upload | overlapped: L2-resident ALU (WarmCS 2048 iterations) while the copy queue uploads 0.0 GB | 1082.15 | ms | kernel x1.012514 (1 dispatches); copy 0.030977 GB/s during overlap |

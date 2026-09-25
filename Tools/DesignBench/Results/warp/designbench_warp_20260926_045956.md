# DesignBench 20260926_045956 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks (0 MB), 1 B density: camera march (1 px per voxel), 32 B record | 4.4476 | ms | 120.648872 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks (0 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 1.6022 | ms | 43.462457 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks (1 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 2.3768 | ms | 64.474826 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks (1 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 3.9324 | ms | 106.673177 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks (1 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 3.7683 | ms | 102.221680 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks (0 MB), 1 B density: camera march (1 px per voxel), 32 B record | 4.14 | ms | 112.304687 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks (0 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 2.24 | ms | 60.763889 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks (4 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 3.6478 | ms | 98.952908 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks (4 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 3.4513 | ms | 93.622504 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks (4 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 1.6467 | ms | 44.669596 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks (1 MB), 1 B density: camera march (1 px per voxel), 32 B record | 2.6201 | ms | 71.074761 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks (1 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 1.9816 | ms | 53.754340 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks (5 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 3.6467 | ms | 98.923069 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks (6 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 5.3226 | ms | 144.384766 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks (6 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 2.2841 | ms | 61.960178 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world (2 MB), 1 B density: camera march (1 px per voxel), 32 B record | 4.6125 | ms | 125.122070 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world (2 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 4.9772 | ms | 135.015191 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world (16 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 9.5703 | ms | 259.611003 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world (18 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 24.996 | ms | 678.059896 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world (18 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 19.2498 | ms | 522.184245 ns/pixel |

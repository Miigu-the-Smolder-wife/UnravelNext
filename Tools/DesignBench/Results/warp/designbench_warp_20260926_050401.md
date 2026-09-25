# DesignBench 20260926_050401 (WARP dry run)

Adapter: WARP, driver , Agility SDK 618, 256x144, reps 1

| section | measurement | value | unit | note |
|---|---|---:|---|---|
| env | timestamp frequency | 1e+07 | Hz |  |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks, random (0 MB), 1 B density: camera march (1 px per voxel), 32 B record | 2.8761 | ms | 78.019206 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks, random (0 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 1.8325 | ms | 49.709744 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks, random (1 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 3.564 | ms | 96.679688 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks, random (1 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 7.2883 | ms | 197.707791 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 4,608 bricks, random (1 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 3.4223 | ms | 92.835829 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks, random (0 MB), 1 B density: camera march (1 px per voxel), 32 B record | 5.254 | ms | 142.523872 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks, random (0 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 6.5192 | ms | 176.844618 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks, random (4 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 6.2043 | ms | 168.302409 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks, random (4 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 6.0256 | ms | 163.454861 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 16,384 bricks, random (4 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 3.5516 | ms | 96.343316 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, random (1 MB), 1 B density: camera march (1 px per voxel), 32 B record | 10.7026 | ms | 290.326606 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, random (1 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 3.8703 | ms | 104.988607 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, random (5 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 5.34 | ms | 144.856771 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, random (6 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 6.8081 | ms | 184.681532 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, random (6 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 11.9995 | ms | 325.507270 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, tree blocks (1 MB), 1 B density: camera march (1 px per voxel), 32 B record | 17.4531 | ms | 473.445638 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, tree blocks (1 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 21.5175 | ms | 583.699544 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, contiguous (1 MB), 1 B density: camera march (1 px per voxel), 32 B record | 30.5839 | ms | 829.641385 ns/pixel |
| bricks | vista world 20x13x2 bricks, pool 24,576 bricks, contiguous (1 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 23.6975 | ms | 642.835829 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, shuffled (2 MB), 1 B density: camera march (1 px per voxel), 32 B record | 30.6577 | ms | 831.643338 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, shuffled (2 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 27.7703 | ms | 753.317600 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, shuffled (16 MB), 8 B AoS: camera march (1 px per voxel), 32 B record | 33.1385 | ms | 898.939345 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, shuffled (18 MB), SoA 1 B density + 8 B attributes where density > 0: camera march (1 px per voxel), 32 B record | 102.928 | ms | 2792.108832 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, shuffled (18 MB), SoA 1 B density + 8 B attributes where density > 0: receiver sun march (1 px per voxel), 4 B T only | 27.1099 | ms | 735.403103 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, tree blocks (2 MB), 1 B density: camera march (1 px per voxel), 32 B record | 26.5934 | ms | 721.392144 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, tree blocks (2 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 24.5961 | ms | 667.211914 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, contiguous (2 MB), 1 B density: camera march (1 px per voxel), 32 B record | 29.2633 | ms | 793.817817 ns/pixel |
| bricks | vista world 20x13x2 bricks, unique world, contiguous (2 MB), 1 B density: receiver sun march (1 px per voxel), 4 B T only | 20.959 | ms | 568.549262 ns/pixel |

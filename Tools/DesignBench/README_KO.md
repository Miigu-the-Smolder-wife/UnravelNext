# DesignBench — 설계 개정 1의 마이크로벤치 (2026-09-25)

`Docs/Design/COVERAGE_REDESIGN_KO.md` 7.3과 `Docs/Design/Requests/20260925_D_coverage_redesign.md` 9절이 요구한 네 벤치다. [예상]으로만 잡혀 있던 단가(fragment 파이프라인, 브릭 DDA, 밴드 스케줄링, 점유율 조건의 셰이딩 커널)를 이 기계의 [실측]으로 바꾼다.

- **독립 프로젝트**다(`Bench/CMakeLists.txt`). `Tools/DesignBench/CMakeLists.txt`에 두면 `cmake/Modules.cmake`가 주 빌드에 넣고 `cmake/Tracks.cmake`(코어)에 폴더 대응이 없어 모든 세션의 구성이 실패하므로, Microbench처럼 따로 빌드한다.
- 빌드: `powershell -File Tools/DesignBench/Build.ps1` → `Tools/DesignBench/build/DesignBench.exe`. 커널은 실행 때 DXC로 컴파일한다(`Bench/shaders/*.hlsl`).
- 하드웨어 실행(측정)은 GPU 잠금 안에서 하나씩: `powershell -File Tools/CI/GpuLock.ps1 -Track Design -- powershell -File Tools/DesignBench/Run.ps1 -Tag <이름> --only-<coverage|bricks|bands|shade>`. 실행 파일은 `UNX_GPU_LOCK`이 없으면 측정을 거부한다. `Run.ps1`이 nvidia-smi 클럭 표본(250 ms)을 `Results/clocks_*.csv`에 남긴다.
- WARP 사전 실행(종료·정합성 확인, 잠금 불필요): `DesignBench.exe --warp [--debug] --only-<...>` (256×144, 1회). 모든 커널 루프는 상수 상한을 가진다.
- 결과: `Results/designbench_<시각>.{json,md,log}`(하드웨어), `Results/warp/`(WARP). 값은 GPU 타임스탬프, 워밍업 3회 뒤 9회의 중앙값, 각 절 앞에 1.5 s FMA 워밍업.

## 벤치와 읽는 법

| 절 | 커널 | 잰 것 | 설계 항 |
|---|---|---|---|
| coverage | `coverage2.hlsl`: `SliverMS`(조각·카드 생성), `FragPS`(보존 래스터 + 정확 면적 + 32 부표본 마스크 + 24 B 레코드; APPEND_MODE 0 전역 목록·웨이브 원자 1회 / 1 타일 구간·fragment마다 원자 / 2 타일 구간·(웨이브, 타일)마다 원자; COUNT=1 세기 패스), `PrefixCS`(타일 오프셋·용량 = 직전 수 × 1.25 + 8), `CompositeCS`(타일 groupshared 정렬 (픽셀, 깊이) → 앞→뒤 합성, fragment당 텍스처 탭 2) | fragment당 ns: 세기, append(모드별), 합성; F ≈ 5/10/20 M, 0.25·0.5 px 조각과 4 px 카드 | 개정 1 4.1·4.5, 5.2 "대역 B 래스터 F_B × 0.050", "coverage 합성 F_B × 0.050 + P_cov × 0.030" |
| bricks | `bricks.hlsl`: `MarchCS`(브릭 표 간접 + 복셀 raw 로드, 카메라/직교 태양 광선, 16·32·48 스텝, 32 B 레코드; VOXEL_BYTES 1·8), `EntryMapCS`(브릭당 16² 셀 × 16 스텝 투과 맵) | 픽셀당·스텝당 ns, L2 상주(19 MB) 대 초과(151 MB) | 개정 1 6.3·6.4, 5.2 "대역 C march P_C × 0.06" |
| bands | `bands.hlsl`: `ResolveCS`(8 B 읽기 → 12 B 쓰기), `ShadeCS`(32 또는 48 B 읽기 → 4 B 쓰기); 전체 화면 대 4·8·16 밴드(밴드마다 해석 → 셰이딩), 매 반복 전 128 MB 쓰기로 L2 비움 | 픽셀당 ns, 4K·1440p | 개정 1 4.8 밴드 스케줄링 이득 |
| shade | `shade2.hlsl`: 8×8 타일 그룹, TILE_SH(3×3 프로브 레코드 80 B를 groupshared에 1회 → 픽셀마다 4프로브 가중 SH 평가) 대 픽셀마다 4 프로브 로드, 공기 볼륨 3D trilinear 3회(RGBA16F 160×90×195), K 아틀라스 탭 1회(RGBA8 (probesX·14)×(probesY·8)), 국소광 8개, EXTRA 32·64·128 추가 생존 값(레지스터 압박) | 변형별 ms·픽셀당 ns | 개정 1 4.4 "셰이딩 커널 0.70 ms(도시), 점유율 조건" |

## WARP 사전 실행 결과 [실측, 2026-09-25]

- bricks·bands·shade: 256×144에서 종료하고 값이 유한하다(`Results/warp/designbench_warp_20260925_1445*.json`). bricks의 첫 레코드 T = 0.092, 대표 깊이 13.3, 브릭 진입 3(직교 4×4×… 격자에서 기대 범위).
- coverage: **WARP는 보존 래스터 + 메시 셰이더에서 접근 위반(0xC0000005)으로 죽는다**(`--no-conservative`면 정상 종료). 커널 논리는 보존 래스터를 끈 실행으로 확인했다: 세기 패스의 fragment 수 = 모드 1·2의 append 수(9,704 / 15,996), 오버플로 0, 합성 종료. 하드웨어 실행은 보존 래스터를 켠다(`Results/warp/designbench_warp_20260925_144649.json`, `..._144659.json`).
- 셰이딩·합성 커널의 실행 중 종료 조건: 모든 `[loop]`에 상수 상한(64·128·256·1024·STEPS).

## 하드웨어 결과 [실측, RTX 4080, 드라이버 32.0.15.9186, 4K, GpuLock, 워밍업 3 + 9회 중앙값]

### coverage — `Results/designbench_20260925_144949.json` (첫 실행: SM 2805 MHz 중앙값)

| 경우 | 세기 패스 (면적 + 타일 원자) | append: 전역 목록 (웨이브 원자) | append: 타일 구간 (fragment 원자) | append: 타일 구간 ((웨이브, 타일) 원자) | 타일 합성 |
|---|---:|---:|---:|---:|---:|
| 조각 0.5 px × 20 px, 5.18 M fragment | 0.828 ms (0.160 ns/f) | 1.321 (0.255) | 1.277 (0.246) | 1.349 (0.260) | 0.554 (0.107 ns/f, 0.067 ns/px) |
| 조각 0.5 px, 10.37 M | 1.654 (0.160) | 2.639 (0.255) | 2.551 (0.246) | 2.695 (0.260) | 0.850 (0.082, 0.102) |
| 조각 0.5 px, 19.70 M | 3.132 (0.159) | 5.000 (0.254) | 4.834 (0.245) | 5.104 (0.259) | 1.439 (0.073, 0.173) |
| 조각 0.25 px, 9.50 M | 1.537 (0.162) | 2.475 (0.261) | 2.392 (0.252) | 2.529 (0.266) | 0.799 (0.084, 0.096) |
| 카드 4 px, 13.35 M | 1.917 (0.144) | 3.074 (0.230) | 2.964 (0.222) | 3.113 (0.233) | 0.957 (0.072, 0.115) |

- fragment 수에 선형이고(5 → 20 M에서 단가 불변) 오버플로 0, 타일 최대 fragment 518(4K 8×8 타일), 1,024 초과 타일 0.
- **append 방식은 비용에 나타나지 않는다**(세 모드 ±5 %): 타일 구간 append(설계 4.1)는 전역 목록과 같은 값이다. 타일당 원자는 병목이 아니다.
- **append 0.25 ns/fragment는 설계 [예상] 0.050(P0a 마이크로벤치 0.047 @16 B)의 5배**이고, 면적만 계산하는 세기 패스도 0.16이다. P0a 벤치의 빈 PS 보존 래스터 바닥(0.037 ns/f)과의 차이는 PS 안의 면적·마스크 계산이다: 이 PS는 V의 `Coverage.hlsli`와 같은 Sutherland-Hodgman 클립(7정점 배열, `p[(i+1)%n]` 동적 색인 → 로컬 메모리 spill 의심)이고 P0a 벤치는 배열 없는 Green 정리 스트리밍 클립이다. AREA·MASK 변형(두 번째 실행)이 이것을 가른다.
- 합성 0.07~0.11 ns/fragment(10 M에서 0.85 ms, 픽셀당 0.10 ns)는 설계 [예상] "F_B × 0.050 + P_cov × 0.030"(10 M·8.3 M px에서 0.75 ms)과 같은 크기다.


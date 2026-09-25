# 요청: 공기 원근·그림자진 공기·국소광 산란을 한 볼륨으로 (S 트랙, 2026-09-25)

M의 요청 `20260925_M_shading_lookup_cost.md`(셰이딩 커널 안 `atmosphereAerial` ≈ 1.15 ms)에 대한 S의 답이다. INTERFACES 5.6의 설명과
7.x의 자원 설명이 바뀌므로 코어의 반영을 요청한다.

## 실측 (M 게이트, city_block 4K, GPU 잠금, 300프레임 중앙값, GPU 프레임)

| 변형 (`atmosphereAerial` 본문만 교체) | 프레임 ms | 조회 몫 |
|---|---:|---:|
| 조회 없음 (`shading.experiment_disable=8`) | 2.631 | — |
| 기존 (3D RGBA32F 8회 trilinear + 파라미터 9 Load + 위상) | 3.748 | 1.12 |
| 파라미터를 상수로 (Load 0) | 3.741 | 1.11 |
| fetch 3회 (깊이 보간 없음) | 3.204 | 0.57 |
| fetch 0회, 위상·exp만 | 2.886 | 0.26 |

비용은 파라미터가 아니라 **의존 fetch 수와 커널 꼬리의 살아 있는 상태**다(점유율이 낮은 커널에서 지연이 드러난다). 그래서 조회 자체를
최소로 만든다.

## 변경

1. **공기 볼륨 한 장**: S의 `froxels()`가 프록셀 격자(`tile_px` × `depth_slices`, 깊이 노드 0..S, 노드 0 = 카메라, 노드 S = `far_m`)에
   Texture3D RGBA16F `gridX × gridY × 3(S+1)`을 만든다. 부분 0 = 카메라→노드 in-scattering(노출 곱한 값; 대기 단일·다중 산란, 캐스터
   그림자로 가려진 공기 제외, 국소광 산란 포함), 부분 1 = 광학 깊이, 부분 2 = 노드의 태양 투과율. `FrameResources::aerialPerspective`와
   `::froxels`가 이 볼륨이다(생산자 S, 단계 = `froxels()`; 기존 `atmosphere()`의 64×64×32 볼륨과 그 커널은 없어진다).
2. **`atmosphereAerial(AtmosphereSrvs, uv, linearDepth, out inscatter, out transmittance)`**: 시그니처는 같다. 의미가 넓어진다. 대기뿐
   아니라 캐스터가 그림자를 드리운 공기(god ray)와 국소광의 공기 산란까지 포함한 **완성된 공기 합성**을 돌려준다. 픽셀당 3D fetch 2회이고,
   파라미터 풀기와 위상 계산이 없다.
3. **새 함수 `atmosphereAirView(AtmosphereSrvs, uv, linearDepth, out inscatter, out transmittance, out sunIlluminance)`**: 위 결과에 더해
   표면점의 비차폐 태양 조도(lux)를 fetch 1회로 준다. 메인 뷰 픽셀에서 `atmosphereAerial` + `atmosphereSunIlluminance`를 대신한다.
   `atmosphereSunIlluminance`는 임의 위치용(R)으로 남는다.
4. **`froxelScattering`**: 2의 결과에 포함되므로 중립값 `(0, 0, 0, 1)`을 돌려준다. **5.6에서 삭제를 요청한다.** `froxelLightRange` /
   `froxelLight`(국소광 목록)는 그대로다.

## 정확도 조건 (각 "정확" 주장의 조건)

- 방향: 타일 중심 광선의 위상을 타일 사이에서 쌍선형 보간한다. Mie(g = 0.8) 위상의 보간 오차 ≤ h²/8·|P''/P| = 4K 0.67°/타일에서
  0.1 % [예상, 해석식]. Rayleigh는 1e-5.
- 깊이: 노드 사이는 깊이에 대해 선형이고, 가중치는 하드웨어 8 bit(노드 간격의 1/512)다. 노드는 지수 간격(`far_m` 65536 m, 64 슬라이스,
  슬라이스당 ×1.20)이다.
- 대기 적분: 슬라이스당 고도 변화 `air_step_altitude_m` 이하의 중점 부분 단계를 쓴다(단계 안은 정확 지수). 오차 (Δh/H_mie)²/24
  ≤ 3e-4 [예상].
- fp16: 노출을 곱해 저장하므로 표시되는 값에 대해 상대 1e-3이다. 투과율은 광학 깊이로 저장한다(700 m까지 T 오차 1e-5).
- 검증: `Passes/Shadow/Tests/FroxelTests.cpp` 4번(기존 AtmosphereTests 5번의 기준과 허용치를 옮김). 현재 렌더 그래프의 쓰기 소실
  결함(코어에 신고함)으로 이 프레임의 확인이 막혀 있다. 확인되면 이 절에 실측을 채운다.

## M에게

셰이딩 커널에서는 깊이를 읽은 직후 `atmosphereAirView`를 부르고, 그 결과를 커널 끝의 합성에 쓰기를 권한다(fetch 지연이 나머지 작업과 겹친다).
`atmosphereSunIlluminance`(픽셀마다 파라미터 9 Load + LUT 4 Load)는 메인 뷰에서 필요 없어진다.

## 결과 (코어, v1.15)

- 기록: INTERFACES 5.6 표(`atmosphereAerial`의 넓어진 의미, `atmosphereAirView`, `froxelScattering` 삭제), `Frame.h`의 `FrameResources::aerialPerspective`/`froxels` 주석(공기 볼륨의 배치와 부분), 12절 v1.15.
- 4번 프레임 검증: 렌더 그래프 쪽 수정(e4f1ce8 깊이·렌더 타깃 풀 분리, f19bbb7 버퍼 첫 사용 활성화 배리어) 뒤 tau가 NO_ALIAS와 같다. 남은 in-scattering 차이는 S 확인을 기다린다(`probe queries`와 `S froxel light lists`의 앨리어싱에서만 나타남, 조율 세션 경유로 전달).

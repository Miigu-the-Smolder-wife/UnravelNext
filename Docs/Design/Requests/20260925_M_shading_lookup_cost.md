# 요청: 셰이딩 커널이 픽셀마다 부르는 S·R 공개 조회의 비용 (M 트랙, 2026-09-25)

설계서 2.11의 셰이딩 커널 예산은 4K 0.37~0.42 ms다(실측 근거: "태양 + 4 국소 GGX, 프로브 4, 프록셀", 16 B 읽기 + 8 B 쓰기의 합성
커널). M의 셰이딩 커널을 통합 프레임(V·M·S·R·C, `build/Mall`, city_block, 4K, 큐 우선순위 HIGH, GPU 잠금)에서 재고 항을 하나씩 뺐다
(`shading.experiment_disable`, 품질 해시에 기록되는 비용 분해 전용 키) [실측, 각 300프레임 중앙값]:

| 항 | 셰이딩 ms | 기여 |
|---|---:|---:|
| 전체 | 2.94 | — |
| S `atmosphereAerial` 제외 | 1.80 | **≈ 1.15** |
| R `screenProbeIrradiance` 제외 | 2.37 | **≈ 0.57** |
| R `reflectionRadiance` + K 경로 `screenProbeRadiance` 제외 | 1.79 | **≈ 1.15** |
| 모두 제외(G-buffer·depth·재질 워드 읽기, 태양 원반, 톤맵, 4 B 쓰기) | 0.37 | 설계 메모리 바닥 0.36과 같음 |

S·R가 없는 빌드(V·M·C)에서 M의 셰이딩은 0.385 ms(태양 원반 정확 적분 포함)로 예산 안이다. 초과분 ≈ 2.4 ms는 전부 공개 조회의
픽셀당 작업이다. 설계 비용식이 가정한 "프로브 4·프록셀 = 조회 몇 번"과 현재 API의 작업량이 다르다(설계 가정과 구현의 불일치 — 어느
쪽을 고칠지는 소유 트랙과 코어가 정한다). M은 품질을 낮추지 않으므로 이 항들을 빼거나 저해상도로 대신하지 않는다.

## S (`Passes/Atmosphere/Atmosphere.hlsli`, `Froxel.hlsli`)

- `atmosphereAerial`: 픽셀마다 3D 텍스처 8회(2 노드 × 4 성분) + `airParamsFromTexels`(텍스처 9회 Load) + 위상 함수 2개.
  `atmosphereSunIlluminance`도 `airParamsFromTexels`를 다시 부른다. 이제 `froxelScattering`(3D 2회 + 격자 헤더)이 합성 식
  `(L T_air + L_air) a + rgb`로 그 위에 더해진다.
- 원하는 것: 픽셀당 **텍스처 2~3회**로 끝나는 합성 조회 하나 — 예: 프록셀 적분이 공기 원근(색별 투과 T_air RGB 포함)과 그림자·국소광
  산란을 함께 담은 볼륨(두 장: in-scatter RGB + a, T_air RGB)을 내고, 셰이딩은 `L_out = L · T + S` 하나로 끝낸다. Mie 전방 피크처럼
  타일 안에서 방향에 민감한 위상은 S가 정확도를 판정해 볼륨/픽셀 분담을 정한다.
- 파라미터는 픽셀마다 텍스처에서 풀지 말고(9 Load) 상수로(프레임 상수의 예비 칸이나 S 상수 버퍼 1회 Load).

## R (`Passes/GI/ScreenProbes.hlsli`, `Passes/Reflection/Reflection.hlsli`)

- `screenProbeIrradiance`와 `screenProbeRadiance`가 같은 4 프로브 발자국(`giProbeFootprint`: 레코드 4×4 텍셀 Load, 프로브마다
  `worldFromDepth` 행렬곱, 평면·법선 가중)을 **각자 다시** 계산한다. Foliage는 반대 반구 조도로 한 번 더 부른다.
- 원하는 것: 한 번에 발자국을 계산하고 `{ 조도(n), 조도(-n) 선택, 근거리 가림, K 경로 복사휘도(dir, cone) }`를 함께 돌려주는 조회
  (예: `screenProbeGather(ProbeSrvs, pixel, n, depth, dir, cone, wantBack)`), 그리고 프로브 위치를 레코드에 월드/뷰 좌표로 담아
  프로브마다의 역투영을 없애는 것.
- `reflectionRadiance`(G/M 결과)는 타일 유효 텍셀 + 픽셀 텍셀 2회 Load로 가볍다. 비용은 K 경로의 `screenProbeRadiance`(4 프로브 ×
  2 밉 × 쌍선형) 쪽이 크다.

## 영향

- 인터페이스: 5.6 표의 S·R 함수 시그니처(추가 또는 교체). M은 새 조회로 바꾸고 같은 분해 측정을 다시 낸다.
- 측정 재현: `powershell -File Tools/CI/GpuLock.ps1 -Track M -- build/Mall/bin/unx_gate_shading_mgate.exe --scene city_block
  --resolution 4K --frames 300 --set shading.experiment_disable=<0|2|4|8|31>`(빌드: `Build.ps1 -Track Mall -Tracks "V;M;S;R;C"`).

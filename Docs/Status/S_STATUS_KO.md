# S 트랙 상태 (그림자·하늘·대기·프록셀) — 2026-09-25

소유: `Native/Render/Passes/Shadow`, `Native/Render/Passes/Atmosphere`, `Config/quality/shadow.toml`, `atmosphere.toml`, `Results/S`.
표기: **[실측]** = GPU 잠금 아래 게이트 중앙값(`Tools/CI/GpuLock.ps1`), 또는 테스트 실행 결과. **[예상]** = 측정 전 계산값.

## 1. 구현된 것

| 항목 | 설계 | 위치 |
|---|---|---|
| 투과 LUT(광학 깊이), 다중산란 LUT(Hillaire, 파라미터 행 포함), sky-view(지평선에서 나눔) | 2.3 | `Atmosphere/*.hlsl`, `AtmosphereSystem` |
| 공기 볼륨(프록셀 격자): 대기 단일·다중 산란, 캐스터 그림자로 가려진 공기, 국소광 공기 산란, 노드 태양 투과 | 2.3, 2.4 | `Atmosphere/FroxelIntegrate.hlsl`, `Shadow/FroxelSystem` |
| 공개 조회: `atmosphereSkyRadiance/SunRadiance/SunIlluminance`, `atmosphereAerial`(fetch 2회), `atmosphereAirView`(fetch 3회) | 5.6 | `Atmosphere/Atmosphere.hlsli` |
| 프록셀 광원 리스트: 타일 절두체 → 슬라이스별 중요도순 ≤ 32, 고정 run(오버플로 없음), 절단 통계 | 2.4, 7.4 | `FroxelLists.hlsl`, `Froxel.hlsli` |
| 태양 VSM: 클립맵, 페이지 캐시, dirty 규칙(변환·스킨 / 바람 > 텍셀 / 새 페이지 / 카메라 이동), 버퍼 풀 | 2.3 | `Shadow/Vsm*.hlsl`, `VsmSystem` |
| 블록 계층(8~128 텍셀 최소제곱 평면 + 잔차 경계), 탐색 경계 격자 | 2.3 | `VsmPageMax`, `VsmSearchGrid` |
| 가시성 패스(2패스: 분류·압축 → 반영 penumbra 간접 디스패치), 4 B/px | 2.11, 7.3 | `ShadowVisibility/Penumbra.hlsl` |
| 공기 그림자: 선분이 지나는 페이지 → 32·8 텍셀 블록 → 텍셀(정확) 걷기, 공기 페이지 요청 | 2.3 | `VsmAir.hlsli`, `VsmMarkAir.hlsl` |
| 페이지 래스터: V 서비스의 타일 국소 모드(`tileLocal`) | 2.3 | `VsmSystem.cpp` |

## 2. 정확성 [실측, 디버그 레이어 오류 0]

- `unx_test_atmosphere_atmospheretests`: 배정밀 기준 대비 투과 3.3e-6, 하늘 0.17 %, 다중산란 수렴 0.48 %, 태양 3.5e-4. PASS.
- `unx_test_shadow_vsmtests`: 정확한 원반 기준 대비 평균 오차 4e-5. dirty 규칙: 정지 0 / 0.32 m 이동 27 / 움직인 캐스터 191 / 태양
  변경 전부. 바람은 흔들림보다 텍셀이 작은 단만 다시 그린다. PASS.
- `unx_test_shadow_froxeltests`:
  - 리스트: 보수성(점 36.9만 개에서 누락 0), 중요도 순서, 두 프레임 간 결정성, 절단 통계.
  - 국소광 공기 산란: 기준 대비 평균 0.04 %, 최대 0.08 %.
  - 그림자진 공기: 균일 슬라이스는 저장 정밀도 안에서 정확하고, 경계 슬라이스는 텍셀 경계 안이다.
  - 공기 원근 vs 기준(4번): 렌더 그래프 앨리어싱 결함(코어가 조사 중)으로 기본 설정에서는 막혀 있다. `UNX_GRAPH_NO_ALIAS=1`로 확인해 이 절에 채운다.

## 3. 성능 [실측, RendererGate city_block 정지 카메라, V·M·S·C 빌드]

| 패스 | 4K ms | 1440p ms | 설계 항 [예상] |
|---|---:|---:|---|
| `s.froxel.integrate` (공기 전부) | 0.59 | 0.25 | 2.3 프록셀 0.03 |
| ├ 기본 (리스트 읽기·scan·노드 쓰기) | 0.10 | | |
| ├ 공기 적분 (LUT 부분 단계) | 0.27 | | |
| └ 공기 그림자 (선분 걷기) | 0.22 | | |
| `s.froxel.lists` | 0.045 | 0.021 | 2.4 리스트 0.01 |
| `s.vsm.markair` | 0.089 | 0.041 | — |
| `s.vsm.mark` | 0.054 | 0.050 | 2.3 페이지 관리 0.04 |
| `s.vsm.raster.*` (dirty 5.5 페이지) | 0.062 | 0.051 | 2.3 T_sun/30G 0.20 |
| `s.shadow.visibility` + `penumbra` | 0.52 + 0.17 | 0.23 + 0.09 | 2.11 |
| **S 합** | **1.57** | **0.77** | 2.13: 0.30 + 0.14 |

변천: 페이지 래스터 26.8 → 0.06 ms(타일 국소). 공기 그림자 2.45 → 0.22 ms(정사각형 분할 → 선분 걷기). M 셰이딩 안 `atmosphereAerial`은
1.12 ms였고, 새 구조에서 fetch 2~3회로 바뀐다(M 게이트 재측정 필요).

## 4. 설계 가정 오류 (실측이 설계와 다른 곳)

1. **가시성 패스**: 설계의 0.17 ms 마이크로벤치는 깊이와 법선을 읽지 않았다. 깊이 + 수신 평면만으로 0.114 ms가 바닥이다(4K).
2. **요청 페이지 수**: texel ≤ footprint 규칙이면 4K에서 3,000~4,000 페이지다(설계는 800~1,500).
3. **프록셀 적분**: 설계는 "F × 300 FLOP + F × 64 B"였다. 실제 프록셀 일은 의존 메모리 조회다(LUT 2개 + VSM 테이블 → 블록 → 텍셀). 게다가
   이 항이 이제 옛 공기 원근 볼륨까지 대신한다.
4. **셰이딩 조회**: 설계는 "프록셀 조회 1회"였다. 옛 API는 픽셀마다 3D 8회 + 파라미터 9 Load였고, 낮은 점유율의 셰이딩 커널에서
   1.12 ms였다. 새 공기 볼륨으로 고쳤다.

## 5. 진행 중·다음 (모두 이번 범위에서 구현 대상)

- **먼 공기의 캐스터 그림자**: 12단 클립맵(최대 텍셀 2 m)은 4K 약 1 km 너머 공기에 맞는 단이 없다. 20단으로 늘린다(텍셀 512 m까지).
  비용식 [예상]: 공기 페이지 ≤ 약 320개(단마다 폭 5 × 슬라이스당 0.5), 거친 페이지는 거의 dirty가 되지 않는다. 래스터는 거친 LOD다.
  먼 픽셀이 2 m 단에 묶이지 않아 페이지 수는 오히려 준다. 이후 GPU 잠금으로 실측하고, C 기준 영상(먼 거리 god ray·산 그림자)과 비교한다.
- 공기 적분 0.27 ms, 공기 그림자 0.22 ms, markair 0.09 ms 줄이기(attribution 키 `atmosphere.froxels.experiment_disable`로 분해 중).
- 국소광 VSM 128(큐브 면, 가시성 슬롯 1~3, `shadowSlotOfLight`, `shadowVisibilityDirect`).
- `lights_max` 초과 광원을 프록셀 조도로 합치는 경로(현재는 절단하고 수와 에너지를 통계로 낸다).
- 국소광 그림자가 공기 산란에 들어가지 않음(국소 VSM 뒤).
- 태양 방향이 바뀌면 전부 다시 그림(회전 불변 캐시 설계 필요). 반사 뷰의 페이지 요청. 스킨 경계가 bind pose.
- `FrameResources::vsmPool`이 TextureRef인데 풀이 버퍼다(S 공개 HLSL API만 쓰게 되어 있음, 코어 요청 필요 시 제출).

## 6. 요청

- `Docs/Design/Requests/20260925_S_depth_raster_page_mask.md` (v1.1 반영), `..._S_raster_instance_and_wind_change.md` (v1.4 반영),
  `..._S_page_local_raster.md` (v1.7 반영), `..._S_air_volume.md` (공기 볼륨, 5.6 변경 요청).

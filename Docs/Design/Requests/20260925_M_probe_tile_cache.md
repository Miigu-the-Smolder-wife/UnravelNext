# 요청: 화면 프로브 조회의 타일 공유 API (M 트랙 → R, 2026-09-25, 설계 개정 1 4.4)

## 왜

설계 개정 1(4.4, 5.3 표)은 셰이딩 커널의 프로브 조회를 "타일 groupshared, 픽셀당 ALU + K 하드웨어 탭"으로 바꾸고, 셰이딩 커널을 도시 4K 0.70 ms(밴드 전)로 잡는다. M 분해 [실측, 도시 4K, 600프레임]에서 프로브 관련 항은 K gather 0.37 ms와 조도 0.08 ms다.

지금 `screenProbeGather`는 픽셀마다 다음을 읽고, L2 왕복 3~4단이 종속으로 이어진다.
1. 헤더 1회
2. 프로브 4개의 plane 0(위치·법선) → 가중치
3. SH 4 × 4 texel과 plane 5(맵 블록) 4회
4. 아틀라스 탭 ≤ 8회(블록 ≤ 4 × 밉 ≤ 2)

점유율이 낮은 셰이딩 커널에서는 이 종속 지연이 그대로 드러난다(개정 1 2절 (a)).

`gi.screen_probe_spacing_px = 8`로 고정이다(GiSystem이 강제). 따라서 M의 8×8 타일 하나가 읽는 프로브는 정확히 타일 주변 3×3개다. 픽셀 f = (p + 0.5)/8 − 0.5의 floor가 t − 1 또는 t이므로 프로브 t − 1 .. t + 1이다.

## 원하는 변경 (R, `ScreenProbes.hlsli`)

같은 수학을 레코드 출처만 groupshared로 바꾼 변형이다. 결과는 `screenProbeGather`와 비트 단위로 같아야 한다.

```hlsl
#ifdef GI_PROBE_TILE_CACHE      // 셰이딩 커널(8×8 타일 그룹)이 include 전에 정의
groupshared uint4 gs_giProbe[9][6];   // 타일 주변 3×3 프로브의 plane 0..5 texel (864 B)
groupshared uint4 gs_giHeader;        // { spacing, probesX, probesY, 0 }

// 그룹의 레인 0..53이 (프로브 k = lane / 6, plane = lane % 6)을 한 번씩 읽는다(좌표는 count − 1로 clamp,
// 텍스처판 giLoadProbeSurface와 같은 규칙). 레인 54는 헤더를 읽는다. 호출자는 그 뒤
// GroupMemoryBarrierWithGroupSync()를 부른다.
void giProbeTileLoad(ProbeSrvs s, uint2 tile, uint lane);

// screenProbeGather와 같은 인자와 결과. 레코드·헤더·plane 5는 groupshared에서 읽고, 아틀라스 탭만 텍스처에서 한다.
ScreenProbeLighting screenProbeGatherTile(ProbeSrvs s, uint2 tile, uint2 pixel, float3 worldPos, float3 normal,
                                          float linearDepth, bool back, bool wantRadiance, float3 dir, float coneHalfAngle);
#endif
```

- 가중치(`giProbeFootprintAt`), SH 평가(`giFootprintIrradiance`), 블록 병합과 밉 선택(`giProbeFootprintRadiance`)은 레코드를 인자로 받는 내부 함수로 나눠 두 경로가 공유하면 좋겠다. 그러면 수학이 한 곳에 있다.
- 검증(R 단위 테스트): 한 프레임의 모든 픽셀에서 `screenProbeGatherTile`과 `screenProbeGather`의 결과가 비트 단위로 같다. 앞면·뒷면·K 모두.

## 비용 [예상, 도시 4K]

- 타일당 읽기 55회(54 + 헤더)가 L2 왕복 1회에 끝난다. 픽셀당 종속 로드는 아틀라스 탭 1단만 남는다.
- 픽셀당 로드 약 25회(헤더 1 + plane 0 4 + SH 16 + plane 5 4)가 타일당 55회로 줄어든다. 64픽셀 기준 1,600회 → 55회다.
- 아틀라스 탭은 결맞음 bilinear 812 G/s [실측 FLOORS texture] 기준으로 8.3 M px × 평균 2~4탭 = 0.02~0.04 ms다.
- 기대: K 0.37 + 조도 0.08 = 0.45 ms가 약 0.05~0.08 ms로 준다. 확정은 M이 적용 뒤 `shading.experiment_disable` 비트 2·4 분해로 잰다.
- 그룹 배리어 1회는 커널 시작에 있다. 두 웨이브가 셰이딩 전에 만나므로, 셰이딩 뒤 교환(가장자리 검출 groupshared판, 0.554 ms로 실패)과 달리 조회 지연을 서로 기다리지 않는다. 다만 M이 A/B로 확인한다.

## M 쪽 변경 (R 반영 뒤)

- `ShadeOpaque`(주·fallback)가 `#define GI_PROBE_TILE_CACHE` 뒤 include하고, 커널 시작에서 `giProbeTileLoad` + 배리어를 부른다. `screenProbeGather`는 `screenProbeGatherTile`로 바꾼다.
- coverage 합성 커널(4.5)도 같은 타일 캐시를 쓴다.
- 평면 뷰는 프로브가 없다(`screenProbes` 무효). 그 뷰에서는 타일 로드를 부르지 않는다.

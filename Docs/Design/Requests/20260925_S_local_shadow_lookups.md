# 요청: 국소광 그림자 조회용 FrameResources 필드 (S 트랙, 2026-09-25)

S가 국소광 VSM(광원당 큐브 6면 × mip 7, 그림자 슬롯 128)을 구현했다. 가시성 슬롯 1~3은 채워진다
(`unx_test_shadow_localshadowtests` [실측]: 구 광원 r 0.1 m 평균 오차 0.0011, 반영 0.054, 점광원 3e-4, 캐스터 이동 뒤에도 같다).
INTERFACES 5.6의 두 함수를 `Passes/Shadow/ShadowVisibility.hlsli`에 구현했다:

```hlsl
uint shadowSlotOfLight(FroxelSrvs f, uint2 pixel, float linearDepth, uint lightIndex);        // 1~3 or 0xFFFFFFFF
float shadowVisibilityDirect(ShadowSrvs s, uint lightIndex, float3 worldPos, float3 normal);  // slots past the third
```

- `shadowSlotOfLight`: 픽셀 프록셀 리스트에서 그림자를 던지는 광원(`lightCastsShadow`)의 순서다(7.3). FroxelSrvs만 쓴다.
- `shadowVisibilityDirect`: 가시성 슬롯과 같은 추정량(`VsmLocalSample.hlsli`)을 가장 촘촘한 상주 mip으로 계산한다.
  - 국소광 레코드와 "장면 광원 → 그림자 슬롯" 표가 필요하다. 둘 다 S의 업로드 링에 있다(그래프 자원이 아니고 디스크립터 인덱스뿐).
  - 이 둘은 `ShadowSrvs.lights`와 `ShadowSrvs.pad0`에 싣는다.

## 코어에 요청하는 변경

1. `FrameResources`에 다음을 넣는다(생산자 S, 단계 `shadowPages`, 그래프 자원이 아니라 이번 프레임의 SRV 디스크립터 인덱스).
   - `uint32_t vsmLocalLights = UINT32_MAX;` StructuredBuffer<VsmLocalLight>(48 B × 128)
   - `uint32_t vsmSlotOfLight = UINT32_MAX;` StructuredBuffer<uint>(장면 광원마다 그림자 슬롯 또는 0xFFFF)
2. INTERFACES 5.6의 `ShadowSrvs` 설명에 `lights = vsmLocalLights`, `pad0 = vsmSlotOfLight`를 적는다.
3. `shadowVisibilityDirect` 시그니처는 그대로 둔다. 선택 사항으로 footprint 인자를 추가하면(`..., float footprint`) 픽셀 발자국에 맞는 mip을 쓴다(지금은 가장 촘촘한 상주 mip).

## 알림

- 프록셀 리스트 항목의 bit 15는 "S의 그림자 슬롯이 있음"이다. 광원 수 한도가 32767로 바뀌었다(`froxelLight`는 마스크된 인덱스, `froxelLightShadowed` 추가).
- 반사 뷰(평면 반사 카메라)의 슬롯 1~3은 아직 255다. 리스트가 메인 뷰 것이기 때문이며, S의 다음 항목이다.

## 결과 (코어, 2026-09-25, INTERFACES v1.19)

- `FrameResources::vsmLocalLights`, `vsmSlotOfLight`(uint32_t, 기본 `UINT32_MAX`)를 추가했다. 이번 프레임의 SRV 디스크립터 인덱스이고(S 업로드 링), 생산은 `shadowPages`다.
- INTERFACES 5.6: `shadowVisibilityDirect`는 `ShadowSrvs.lights` = `vsmLocalLights`, `.pad0` = `vsmSlotOfLight`로 읽는다. 시그니처는 그대로다. footprint 인자는 소비자(M·R)가 필요하다고 할 때 추가한다.
- INTERFACES 5.6 `Froxel.hlsli` 행과 7.4: 항목 bit 15 = 그림자 슬롯 유무(`froxelLightShadowed`), `froxelLight`는 마스크된 인덱스, 광원 한도 32767.
- 검증: 헤더 추가뿐이다. core;S 빌드가 통과했다(코어 워크트리, 실행 없음).

# 요청: 픽셀 기준 국소광 가시성 공개 함수 (S 트랙, 2026-09-25, M 요청에 따른 추가)

## 왜
- v1.20 오버플로 목록에서 넘친 타일은 M fallback 커널이 슬롯 밖 광원을 직접 평가한다. 7.3의 "결과는 정확하다"가 성립하려면 fallback의 값이 S 가시성 슬롯·오버플로 값과 같은 계산이어야 한다.
- 지금 공개된 `shadowVisibilityDirect(s, li, worldPos, normal)`는 footprint 0(가장 촘촘한 상주 mip)을 쓴다. 가시성 패스는 깊이에서 얻은 receiver(위치·기하 법선)와 픽셀 footprint를 쓴다. 입력이 달라 값이 다를 수 있다.

## 변경 (S 소유 파일 `Passes/Shadow/ShadowVisibility.hlsli`, 구현 9e73555)
INTERFACES 5.6 표의 ShadowVisibility.hlsli 칸에 아래를 추가해 주기 바란다.
- `struct ShadowPixelReceiver { float3 world; float footprint; float3 normal; uint valid; };`
- `ShadowPixelReceiver shadowPixelReceiver(uint2 pixel, uint depthSrv, uint gbufferSrv)`
  - 바인딩된 프레임 상수의 뷰 기준이다.
  - 깊이에서 얻은 위치·기하 법선(ShadowReceiver.hlsli)과 픽셀 footprint(m)를 준다.
  - 하늘이거나 뷰 밖이면 valid = 0이다.
- `float shadowLocalVisibilityAtReceiver(ShadowSrvs, uint lightIndex, ShadowPixelReceiver)`
  - 가시성 슬롯 1~3, 오버플로 목록과 **같은 계산**이다(S의 ShadowVisibility.hlsl과 ShadowOverflow.hlsl이 이 함수를 부른다).
  - 그림자 슬롯이 없는 광원은 1이다.
  - 저장된 값은 `round(saturate(v) * 255) / 255`다. fallback이 슬롯 값과 비트 단위로 같으려면 같은 양자화를 거친다.
- `float shadowLocalVisibilityAtPixel(ShadowSrvs, uint lightIndex, uint2 pixel, uint depthSrv, uint gbufferSrv)`
  - 위 두 함수를 합친 것이다.
  - 광원을 여럿 도는 fallback은 receiver를 한 번 만들고 `AtReceiver`를 부른다.
- ShadowSrvs에 쓰는 값은 v1.19와 같다(`lights` = vsmLocalLights, `pad0` = vsmSlotOfLight). 이 함수들은 `searchBound`를 읽지 않는다.
- `shadowVisibilityDirect`는 그대로 둔다(뷰 픽셀이 없는 호출자용).

## 비용
- 새 자원이나 필드는 없다. 헤더에 Frame.hlsli와 GBuffer.hlsli가 포함된다. 기존 소비자(ShadeOpaque, GiTrace, ReflectionTrace)는 이미 그 둘을 포함하며, 오프라인 컴파일로 통과를 확인했다(V;M;S;C 빌드).

## 결과 (코어, 2026-09-25, INTERFACES v1.21)

- 5.6 `ShadowVisibility.hlsli` 칸에 네 선언(`ShadowPixelReceiver`, `shadowPixelReceiver`, `shadowLocalVisibilityAtReceiver`, `shadowLocalVisibilityAtPixel`)과 조건(같은 계산·같은 양자화, 슬롯 없는 광원 1, `searchBound` 미사용)을 적었다. `shadowVisibilityDirect`는 "뷰 픽셀이 없는 호출자용"으로 표기했다.
- 문서 기록뿐이다(S 파일, 새 자원 없음).

# 화면 공간 추적: 다른 트랙이 쓰는 법 (S2 → R)

2026-10-01, S2. 코드: `Native/Render/Passes/Reflection/ScreenTrace.hlsli`(공용 include), `ReflectionHzb.hlsl`(깊이 피라미드), 사용 예 `ReflectionScreenTrace.hlsl`. 반사 쪽 스위치 `reflection.lumen_screen_traces`. 구조는 ue6-main `HZBTracing.ush` / `LumenScreenTracing.ush`.

## C++ (패스를 기록하는 쪽)

```cpp
#include "unx/refl/ReflectionSystem.h"
const refl::ScreenTraceInputs screen = refl::ReflectionSystem::get(fc).screenTraceInputs(fc, main);  // 프레임당 1회 기록, 누가 먼저 불러도 됨
if (screen.hzb.valid() && screen.prevColor.valid()) { /* 화면 추적 사용 */ }   // prevColor 무효 = 색 이력 없음 → 월드 광선만
// 패스 선언: b.use(main.depth, Use::SrvCompute); b.use(screen.hzb, Use::SrvCompute); b.use(screen.prevColor, Use::SrvCompute);
// 상수: c.srv(screen.hzb), c.srv(screen.prevColor), fc.frame.upscale.outputWidth / outputHeight(이전 색의 크기),
//       fc.frame.upscale.exposureRatio, fc.frame.upscale.prevViewProj(행 4개 = float4 4개)
```
- `screen.hzb`: R32F 아틀라스. 뷰 깊이의 단계 1~6(단계 L의 텍셀 = 2^L px 안의 가장 가까운 깊이 = 가장 큰 device depth). 단계 0은 `main.depth` 자체다. 위치는 `sctLevelOrigin`.
- `screen.prevColor` = `ViewResources::prevSceneColor`: 업스케일러의 이전 프레임 출력(출력 해상도 RGBA16F, 선형 radiance × 이전 프레임 노출, 지터 없음). 첫 프레임·리셋·업스케일 꺼짐이면 무효.

## HLSL

```hlsl
#include "Passes/Reflection/ScreenTrace.hlsli"
Texture2D<float> depth = ResourceDescriptorHeap[...];   // main.depth
Texture2D<float> hzb = ResourceDescriptorHeap[...];     // screen.hzb
const SctResult r = sctTrace(depth, hzb, size /*뷰 크기*/, origin, direction, maxDistance, 50 /*반복*/, 0.005 /*상대 두께*/, thicknessSteps);
if (r.hit && !r.uncertain)
{
    const float3 hit = sctWorld(r.at);                    // 월드 적중점. r.at = (픽셀 x, y, device depth)
    float3 radiance;                                      // nits
    if (sctPreviousColour(prevColour, prevSize, prevViewProj, hit, exposureRatio, noise01, radiance)) { /* 화면 적중: 월드 광선 생략 */ }
}
// 적중이 아니면 r.at은 광선이 마지막으로 장면 앞에 있던 점이다: sctWorld(r.at)에서 월드 광선을 이어 쏠 수 있다.
```
- `origin`은 표면에서 약간 띄운다(반사는 법선 방향으로 화소 기울기 × 2 + 1 mm). 프레임 상수(b1)는 주 시야여야 한다.
- `thicknessSteps`: 적중 뒤 광선을 몇 걸음 더 가 보아 얇은 물체 뒤인지 확인(언리얼의 NumThicknessStepsToDetermineCertainty; 화면 프로브 수집은 4, 반사는 0).
- 언리얼 기본값: 반사 50회·두께 0.005. 화면 프로브 수집 쪽 값은 R의 소스 확인대로.

## 언리얼과 다른 점

1. 적중점에서 이전 프레임 깊이 검사를 하지 않는다(깊이 이력이 없다): 이번 프레임에 새로 드러난 표면은 지난 프레임에 그 자리를 가리던 것의 색을 읽는다.
2. 적중점을 정지한 것으로 본다(움직이는 물체의 속도 재투영 없음).
3. 반사: 화면 추적이 놓친 광선의 월드 광선은 화면 추적이 끝난 곳이 아니라 표면에서 다시 시작한다.
4. 업스케일이 꺼진 출력(1080p 미만)에서는 색 이력이 없어 화면 추적을 건너뛴다.
5. 먼 거리 화면 추적(DistantScreenTraces), 머리카락 화면 추적, 월드 hit에서 화면 색 읽기(SampleSceneColorAtHit)는 없다.

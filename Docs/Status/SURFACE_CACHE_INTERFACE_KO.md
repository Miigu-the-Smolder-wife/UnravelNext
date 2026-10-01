# 표면 캐시: 다른 트랙이 쓰는 법 (S2 → R, A)

2026-10-01, S2. 코드는 `Native/Render/Passes/SurfaceCache/`(커널)와 `ReflectionSystem`(버퍼·패스 기록). 스위치 `surface_cache.enabled`(기본 false), 매개변수 `Config/quality/surface_cache.toml`.

## 무엇인가

카메라 주변 표면의 조명을 월드 공간에 상주시켜 둔 저장소다. Lumen의 표면 캐시가 하는 일을 한다: 광선 hit이 조명을 그 자리에서 확률 표본으로 추정하지 않고, 이미 갱신돼 있는 값을 읽는다. 규칙과 수치는 ue6-main의 카드 표면 캐시(LumenSceneRendering / LumenSceneLighting / LumenSceneDirectLighting / LumenRadiosity)를 따랐고, 표현만 카드 아틀라스 대신 월드 셀 해시다(이 엔진에는 카드 캡처가 없다).

| | 규칙 (괄호 = 언리얼의 대응 값) |
|---|---|
| 텍셀 = 셀 | 크기 = 카메라 거리 / 100, 최소 5 cm, 2의 거듭제곱 단계(CardTexelDensityScale 100, CardMaxTexelDensity 0.2/cm). 법선의 주축 6면으로 나눔 |
| 캡처 | 매 프레임 카메라 위치에서 구면 균등 방향으로 경로를 쏘고 3번 더 튕기며 맞힌 셀을 표시(시야 방향 무관). 프레임당 표시 수 = 칸 수 / 64(CardCaptureFactor 64) |
| 상주 | 셀은 최근 255프레임 안에 표시됐으면 남는다(NumFramesToKeepUnusedPages 256). 카메라가 움직여 점의 단계가 바뀌면 표시는 새 단계의 셀로 가고, 그 셀이 조명을 받기 전에는 읽기가 한 단계 곱거나 거친 옛 셀을 대신 읽는다(ResampleLighting) |
| 직접광 | 프레임당 칸 수 / 32셀(UpdateFactor 32), 새 셀 먼저. 셀에서 비가림 조도가 큰 광원 8개(MaxLightsPerTile 8)를 각각 그림자 광선 1개로 계산 + 태양 그림자 광선 1개. 값은 교체(시간 누적 없음). `surface_cache.direct_stochastic`(기본 false, 언리얼의 Stochastic도 기본 0): 8개 대신 A의 월드 점 광원 표본기(`MegaLightsWorld.hlsli`)로 셀당 표본 1개 + 최대 12프레임 누적 |
| radiosity | 셀 4×4마다 프로브 1개(ProbeSpacing 4), 프레임당 칸 수 / 64 / 16개(UpdateFactor 64), 광선 4×4(HemisphereProbeResolution 4), 광선 세기 상한 40(MaxRayIntensity), 최대 4회 누적(Temporal.MaxFramesAccumulated 4). 셀은 갱신 때 주변 3×3 프로브를 거리·평면 가중으로 읽는다(SpatialFilterProbes, ProbePlaneWeighting) |
| 읽기 | 조명을 한 번이라도 받은 셀만 유효. 그 전에는 유효하지 않다고 답한다(부르는 쪽이 자기 추정으로 처리) |

## 부르는 법

C++ (패스를 기록하는 쪽):
```cpp
#include "unx/refl/ReflectionSystem.h"
const BufferRef surfaceCache = refl::ReflectionSystem::get(fc).surfaceCacheBuffer(fc);  // 꺼져 있거나 첫 프레임 전이면 invalid
// 패스 선언: b.use(surfaceCache, Use::UavCompute);  (DispatchRays 패스는 Use::UavGraphics)
// 루트 상수 어디든: c.uav(surfaceCache)  (없으면 0xFFFFFFFF)
```
GI는 반사보다 먼저 기록되므로, GI hit의 표시는 그 프레임 것이고 읽기는 한 프레임 전 조명을 본다(표면 캐시의 갱신 패스는 반사 기록 안에서 돈다).

HLSL (hit 셰이딩):
```hlsl
#include "Passes/SurfaceCache/SurfaceCache.hlsli"
RWByteAddressBuffer sc = ResourceDescriptorHeap[uav];
const ScLayout layout = scLayout(sc);
// face = 광선이 온 쪽을 향한 기하 법선
scMark(sc, layout, position, face, albedo, emission);        // 셀을 살려 둔다(없으면 생성). 프레임 첫 표시가 점·재질을 적는다
const ScSample cell = scRead(sc, layout, position, face);
if (cell.valid)
{
    // 조도(E): cell.direct(국소광), cell.sun(태양 직접), cell.indirect(다중 반사). 셀의 cell.albedo, cell.emission.
    // 확산 표면이 내보내는 빛: scFinalLighting(cell) = (direct + sun + indirect) x albedo / pi + emission
}
```
- 반사 hit(`ReflectionShade.hlsli`)은 `cell.direct + cell.indirect`를 hit의 조도로 쓰고, hit 자신의 재질·방출·태양 항은 그대로 둔다. GI hit도 같은 방식이면 된다: 국소광 확률 표본과 캐시 조회 대신 셀의 값.
- `albedo`는 확산 반사율(반사 hit은 `baseColor × (1 − metallic) + 0.45 × 스페큘러 색`, 언리얼의 fully-rough 근사), `emission`은 재질 방출(nits).
- 커널 크기: 표시 + 읽기는 DXIL 약 3~5 KB.

## 언리얼과 다른 점 (목록)

1. 표현: 카드 아틀라스가 아니라 월드 셀 해시. 그래서 캡처가 래스터가 아니라 카메라 위치에서 쏘는 구면 광선 + 튕김이다. 카메라에서 어떤 경로로도 닿지 않는 표면에는 셀이 없다.
2. 셀 키는 32비트 지문이다(좌표·단계·면의 해시). 서로 다른 셀이 한 값으로 겹칠 확률은 조회당 약 4 / 2^32.
3. 카메라가 움직여 셀의 단계(크기)가 바뀌면 새 단계의 셀이 처음부터 조명을 받는다. 그동안 읽기는 인접 단계의 옛 셀로 떨어진다(언리얼은 카드 재할당 때 조명을 새 해상도로 다시 표본한다: 같은 목적, 다른 방법). 확률판 직접광의 카드 공간 이웃 범위 제한(NeighborhoodClampScale 2)은 없다.
4. 조명 갱신 순서: 새 셀 먼저, 남은 예산의 절반은 소비자(반사·GI hit)가 지난 프레임에 읽은 셀(`surface_cache.lighting_feedback`, 기본 true = 언리얼의 Lighting.Feedback), 나머지는 목록을 도는 창. 언리얼은 마지막 갱신 뒤 지난 프레임 수와 피드백을 한 우선순위 히스토그램으로 합친다. `scMark`는 소비자의 표시(우선순위를 요구), 캐시 자신의 캡처·radiosity 광선은 `scMarkQuiet`(셀만 살려 둠).
5. 직접광의 그림자: 셀당 광원당 광선 1개. 언리얼의 "균일하게 가려진 타일은 광선 생략"(AdaptiveShadowTracing)은 없다.
6. radiosity: 프로브가 SH가 아니라 프로브 법선의 조도 하나를 갖는다(셀은 면이 같은 프로브만 읽는다).
7. 반사 hit은 태양 항을 캐시에서 읽지 않고 직접 셰이딩한다(우리 VSM·그림자 광선). 캐시가 유효하지 않은 hit은 검게 두지 않고 예전 hit 셰이딩으로 처리한다.
8. FX 입자 광원은 셀의 직접광에 들어가지 않는다.

품질을 내주는 값(사용자 결정, toml에 QUALITY TRADE 표시): `radiosity_max_ray_intensity = 40`, `remainder_light = false`(셀당 가장 센 8개 밖의 광원은 버림. true면 나머지에서 1개를 더 뽑아 에너지를 지킨다).

# MegaLights 판정 메모: 로비 천장 가장자리 빛 띠 (2026-10-02, 작성 A)

조정 06:30 항목 3. `shading.mega_lights`를 켜면 로비 천장 코브의 넓은 빛 띠가 사라진다(광선을 끄면 돌아옴: 10-01 `ml_ns`). 이것이 맞는 결과인지 판정할 자료.

## 1. 띠를 만드는 광원 [장면에서 읽음: `Results/Local/Fix-11/postgame/lobby_lights.txt`]
- 로비 광원 827개: 점 75, 스폿 345, 사각 407. 전부 그림자 켬(게임의 LampShadowRule: 범위가 방 밖에 닿는 광원은 그림자).
- 천장 코브: **사각 띠 광원 387개**, y = 7.705 m, 길이 0.9~4.9 m(4.9 m 67개, 3.9 m 229개, 3.4 m 77개 등) × 폭 0.04 m, 범위 5 m.
- 띠의 "발광체 반경"(S의 `emitterRadius`, 반대각선): 4.9 m 띠는 2.45 m.

## 2. 옛 경로와 표본 경로가 다른 점 [코드에서 읽음]
| | 그림자가 시작되는 곳 | 근거 |
|---|---|---|
| 옛 경로(S의 국소 그림자 맵) | 광원 **중심**에서 `max(5 cm, 발광체 반경)` 밖. 4.9 m 띠는 중심에서 2.45 m 안의 형상이 그림자를 드리우지 않음 | `VsmSystem.cpp` 641행 `d.nearM = std::max(0.05f, radius)` |
| 표본 경로(`m.ml.trace`) | 광원 **표면의 뽑힌 점** 5 cm 앞까지 광선. 그 앞의 형상은 전부 가림 | `MegaLightsWorld.hlsli mlSampleVisible`, `shading.mega_lights_ray_end_bias_m` 0.05 |

즉 옛 경로의 빛 띠는 "띠 광원 둘레 2.45 m 안의 형상(코브 홈통·천장 턱)을 그림자에서 뺀" 결과였고, 표본 경로는 그 형상을 그림자 캐스터로 본다. 어느 쪽이 맞는지는 코브 형상이 실제로 빛을 막도록 지어졌는지에 달려 있다(3절 실험).

## 3. 실험: 광선 끝 바이어스를 키우며 띠가 돌아오는 거리 [잠금 안, 로비 1080p 60프레임씩]
(결과는 실행 뒤 채움)

## 4. 언리얼 MegaLights는 광원 자신의 하우징을 어떻게 빼는가 [소스: ue6-main]
- 그림자 광선의 끝: `MaxTraceDistance = 표본까지 거리 − UnpackLightRayEndBias(광원)` (`MegaLightsHardwareRayTracing.usf` 98행).
- 끝 바이어스는 **광원별 값**이다: `LightProxy->GetRayEndBias()`, 음수면 전역 `r.MegaLights.HardwareRayTracing.EndBias`(기본 1 cm)를 씀 (`LightGridInjection.cpp` 1685–1688행). cvar 설명: "Bias for the end of a shadow ray. Can be used to fix self-occlusion artifacts between a light and nearby geometry like an enclosure mesh."
- 면 추리기는 하지 않는다: `r.MegaLights.HardwareRayTracing.ForceTwoSided` 기본 true → 모든 메시가 양면으로 가림 (`MegaLightsRayTracing.cpp` 223–226행). 하우징 바깥면만 추려 빼는 식이 아니다.
- 인스턴스 마스크는 불투명 그림자 캐스터(`RAY_TRACING_MASK_OPAQUE_SHADOW`). 하우징 메시가 그림자를 드리우지 않게 하려면 그 메시의 Cast Shadow를 끄는 것이 저작 쪽 수단이다.
- 정리: 언리얼의 기본은 "광원 근처 형상도 가린다(끝 1 cm)"이고, 광원이 하우징·홈통 안에 있을 때의 수단은 **(a) 그 광원의 Ray End Bias를 키움, (b) 하우징 메시의 그림자 끔** — 둘 다 저작 값이다. 엔진이 자동으로 "발광체 반경 안은 뺀다"는 규칙은 없다.

## 5. 판정에 쓸 선택지 (결정은 사용자·조정)
1. 언리얼대로: 전역 끝 바이어스는 작게 두고, **광원별 끝 바이어스**를 장면 광원 속성으로 추가(게임이 코브 띠에 값을 줌) 또는 코브 형상의 그림자 캐스팅을 끔. 형상이 실제로 막는 곳은 막힌 채로 남는다.
2. S의 옛 규칙 유지: 끝 바이어스 = max(전역, 그 광원의 발광체 반경). 띠가 옛 경로처럼 나오지만, 긴 띠 광원 둘레 2.45 m 안의 어떤 형상도 그 광원의 그림자를 드리우지 않는다(선반·난간 등도 포함).
3. 지금대로(끝 5 cm): 코브 형상이 띠 광원을 가린다. 띠가 사라지는 것이 형상대로의 결과라면 이것이 맞고, 레벨 쪽에서 광원 위치나 형상을 고쳐야 한다.

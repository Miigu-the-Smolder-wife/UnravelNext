# 요청: GGX D의 float 소거 (거칠기 0 = 거울에서 1/0) (C 트랙, 2026-09-25)

## 관찰 [실측]
`Native/Scene/src/MaterialModel.cpp`(코어 소유)의

```cpp
const float t = NoH * NoH * (a2 - 1) + 1;   // a2 = alpha^2
return a2 / (kPi * t * t);
```

은 alpha = max(r², 1e-4) = 1e-4(거칠기 0, 설계서의 거울·평면 반사 재질)에서 `a2 - 1`이 float로 정확히 `-1`이 된다
(a2 = 1e-8 < float 엡실론). 그래서 반사 방향(NoH = 1) 근처에서 `t = 0`, D = ∞, f = ∞이다. C의 테스트 장면(Interior의
거울, metallic 1 / roughness 0)을 기준 경로추적기로 그리자 f = ∞, pdf = ∞ → NaN 표본이 픽셀 표본의 7~10%였다
(`unx_reference render --scene interior --camera overview`). `MaterialModel.hlsli`의 미러도 같은 식이면 실시간 거울
(M 경로, 평면 반사면의 셰이딩)에서 같은 inf/NaN이 난다.

## 원하는 변경 (수학적으로 같은 함수, 소거 없는 형태)
1 − NoH² = |n × h|² 를 직접 쓴다(Filament의 형태):

```cpp
// sin2 = |cross(n, h)|^2
const float t = sin2 + a2 * NoH * NoH;       // = NoH^2 (a2 - 1) + 1 without cancellation
return a2 / (kPi * t * t);
```

`distributionGgx(NoH, alpha)`의 시그니처는 n·h만 받으므로 `evaluate` 안에서 `cross(n, h)`를 계산해 넘기는 형태로 바꿔야 한다
(C++·HLSL 둘 다). 최대값은 1/(π α²) = 3.2e7로 float 범위 안이다.

## C 쪽 처리
기준 경로추적기는 같은 모델을 double + 위 형태로 평가한다(`Reference/PathTracer/src/Bsdf.h evaluateModel`). 테스트
`unx_test_reference model`이 거칠기 ≥ 0.05에서 `scene::model::evaluate`와의 상대 차이 ≤ 2e-4, 거칠기 0 반사 방향에서
유한값임을 확인한다.

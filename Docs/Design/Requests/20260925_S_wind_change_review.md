# S 검토: 커밋 뒤 장면 바람 변경 (I 요청 `20260925_I_wind_change.md`에 대한 답, 2026-09-25)

## 결론
구조(바람 revision, 두 바람 상태를 받는 변화 상한, 6.4 문구 변경)에 동의한다. S의 페이지 dirty 규칙 (b)에는 아래 형태가 맞다.

## 상한은 끝점만의 함수여야 한다
- v1 `windOffset(t; wind)`는 상태 함수다. 그 시각의 바람과 시각만으로 정해지고, 이력(위상 적분 등)이 없다.
- 그래서 페이지를 그린 순간의 바람(t0, s0, d0)과 지금의 바람(t1, s1, d1)만으로 |Δ|의 상한이 정해진다. 그 사이에 바람이 몇 번 바뀌었어도 마찬가지다.
- 서서히 바뀌는 날씨 바람도 매 프레임 끝점 비교로 정확하다.
- **계약에 넣어 달라**: P3 바람 모델도 이 성질(무기억)을 지키거나, 지키지 않으면 상한이 이력을 받는 형태로 바뀐다고 적는다.

## 더 좁은(정확한) 인자
v1: windOffset = d · K · s² · m(t), m(t) = 0.6 + 0.4 sin(1.7t + φ) ∈ [0.2, 1]. K는 높이·강성의 속도 무관 부분이다.

|Δ| = |d₁K s₁² m₁ − d₀K s₀² m₀|
    ≤ K [ s₁² |m₁ − m₀| + |s₁² − s₀²| · m₀ + |d₁ − d₀| · s₀² m₀ ]
    ≤ K [ s₁² · 0.4 min(2, 1.7|t₁ − t₀|) + |s₁² − s₀²| + 2 sin(Δθ/2) · s₀² ]

I 초안(시간 항 + |s₁² − s₀²|/max² + 2 sin(Δθ/2), 모두 max(s₀, s₁)²로 정규화)도 상한이다. 위 식은 시간 항에 s₁², 방향 항에 s₀²가 따로 붙어서 더 좁다. 약해지는 바람에서 불필요한 다시 그리기가 줄어든다.

## 원하는 함수 형태 (Deformation.hlsli)
속도를 분리해야 페이지가 그린 시점의 속도와 무관하게 상한을 계산할 수 있다.
- `float windOffsetScale(GpuInstance inst, float3 centre, float radius)`: K × h²에 해당하는 속도 무관 부분. 지금의 `windOffsetBound` = scale × s²다.
- `float windChangeBound(float scale, float t0, float s0, float3 d0, float t1, float s1, float3 d1)`: 위 식이다.
- `windOffsetBound`와 지금의 `windChangeFactor`는 컬링 등 기존 호출자를 위해 그대로 둔다.

## S 쪽 변경 (코어 확정 뒤 S가 구현)
- 페이지 메타(32 B)의 `windAmplitude`를 scale의 최대값으로 바꾼다. 그린 순간의 바람을 pad 두 워드에 저장한다: s₀는 float, d₀는 팔면체 16 bit × 2다.
  - 방향 양자화 오차는 상한 쪽으로 넣는다(Δθ에 1e-4 rad를 더함).
- VsmRelease: `scale × windChangeBound(...) > windTexels × texel`이면 stale로 표시한다. 지금의 `windChanged` 전부 무효화는 없앤다.
- 프레임 상수에는 이미 g_windSpeed와 g_windDirection이 있다. revision은 S에게 필요 없다(끝점 비교). R의 BLAS 판단에는 필요할 수 있다.

## 비용 [예상]
- 페이지당 VsmRelease 산술이 몇 개 늘고, 메타에 두 워드를 더 쓴다. 풀 순회 비용은 그대로다.
- 바람이 바뀌는 동안 다시 그리는 페이지는 실제 변위가 텍셀을 넘는 페이지뿐이다. 지금 방식(바람이 바뀌면 바람 캐스터 페이지 전부)에서 줄어든다.

## 결과 (코어, 2026-09-25, INTERFACES v1.23)

- `windOffsetScale`과 `windChangeBound`를 `Deformation.hlsli`에 넣었다(요청한 식 그대로). `windOffsetBound`는 scale × s²로 다시 썼다(값 동일).
- 무기억 계약을 6.4와 함수 주석에 적었다.

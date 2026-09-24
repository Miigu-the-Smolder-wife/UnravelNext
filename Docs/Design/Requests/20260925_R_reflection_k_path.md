# 요청: 반사 K 경로를 M의 셰이딩 커널에서 평가 + 타일 최소 거칠기 (R 트랙, 2026-09-25)

## 왜 필요한가

설계서 2.6은 반사를 K(캐시 조회) / G(표본 격자 + 4광선) / M(1광선) 세 경로로 나눈다. K는 로브 반각 θ_r,narrow ≥ 22°이고 시차 조건
d_r ≥ 2ρ/θ_r(ρ = **화면 프로브 간격**의 월드 크기)을 만족하는 픽셀이다. 대부분의 픽셀이 K다(도시·숲 시점에서 G/M은 젖은 도로·웅덩이·광택면).

현재 인터페이스(5.6)의 반사 API는 `reflectionRadiance(reflectionSrv, pixel)` 하나다. 이것만으로 K를 풀려면 R이 **모든 K 픽셀**에 대해 픽셀 패스를
돌려 결과를 `view.reflection`(RGBA16F)에 써야 한다.
- 비용 [예상, 600 GB/s]: depth 4 B + G-buffer 8 B 읽기 + 8 B 쓰기 = 20 B/px × 8.3 M = 166 MB → **0.28 ms**(4K), 여기에 캐시 조회.
  설계서 2.13의 반사 항(도시 0.41, 숲 0.07 ms)에는 K 조회 비용이 없다. 이 방식은 숲 구간 반사 예산의 4배다.
- M의 셰이딩 커널은 이미 그 픽셀의 법선·거칠기·시선·깊이를 레지스터에 갖고 있다. K를 거기서 평가하면 추가 비용은 프로브 조회뿐이다.

또 G/M 픽셀을 고르려면 R이 화면 전체의 거칠기를 읽어야 한다(G-buffer RG32 읽기 66 MB → 0.11 ms [예상]). M의 재질 해석은 거칠기를 이미
계산하므로, 8×8 타일당 최소 거칠기 1바이트를 부산물로 쓰면(웨이브 min + 타일당 저장 1회) R은 130 k 바이트만 읽고 광선이 필요한 타일만 픽셀 단위로 본다.

## 원하는 변경

1. **R의 공개 HLSL에 함수 추가** (R이 구현, 이름·시그니처 고정 요청):
   - `Passes/GI/ScreenProbes.hlsli`:
     `float3 screenProbeRadiance(ProbeSrvs s, uint2 pixel, float3 normal, float linearDepth, float3 dir, float coneHalfAngle)`
     — 화면 프로브의 방향 복사(프로브마다 8×8 반구 팔면체, 로브 폭별 밉)를 4프로브 보간해 반각 coneHalfAngle 원뿔로 prefilter한 입사 복사휘도(nit).
   - `Passes/Reflection/Reflection.hlsli`:
     `float reflectionLobeHalfAngle(float perceptualRoughness, float NoV)` — 설계서 2.6의 θ_r,narrow(75 % 에너지 반각, 스침각 cos θ_o 압축).
     M과 R이 같은 식으로 K/G/M을 나누기 위해 공유한다.
2. **M의 셰이딩 규칙** (INTERFACES 5.6 표의 `reflectionRadiance` 설명에 추가):
   `reflectionRadiance(...).a == 1`이면 R이 계산한 G/M 결과를 쓰고, `a == 0`이면 K: `screenProbeRadiance(probes, pixel, n, depth, reflect(-v, n), reflectionLobeHalfAngle(r, NoV))`
   에 M의 재질 모델 스페큘러 항(split-sum 방향 알베도 등)을 곱한다. 보조 뷰(평면 반사)는 지금처럼 `giCacheRadiance`.
3. **M의 재질 해석 부산물** (새 `ViewResources` 필드, 코어·M): `TextureRef roughnessTiles` — R8_UNORM, ⌈W/8⌉×⌈H/8⌉,
   타일 안 표면 픽셀의 최소 지각 거칠기(하늘만인 타일 = 1). 생산 M, 소비 R.

## 비용 [예상]

- 화면 프로브 방향 복사: 프로브당 8×8 + 4×4 텍셀 × 4 B(RGB9E5) = 320 B, 4K 130 k 프로브 → 42 MB 쓰기 ≈ 0.07 ms(R의 `r.gi.gather`에 포함).
- M의 K 조회: 픽셀당 4프로브 × 밉 1단 bilinear(텍셀 4개), 프로브는 64픽셀이 공유해 L1/L2 적중 → ≈ 0.03~0.05 ms.
- 타일 최소 거칠기: M 쪽 추가 쓰기 130 KB, R의 분류 읽기 130 KB → ≈ 0.
- 합 ≈ 0.1 ms. 대안(R의 전화면 K 패스)은 ≥ 0.28 ms.

## 영향

- M: 셰이딩 커널에 K 분기 하나, 재질 해석에 타일 min 출력 하나. 코어: `ViewResources` 필드 하나, INTERFACES 5.1·5.6 표.
- R: 위 두 함수 구현, G/M 광선은 `roughnessTiles`로 고른 타일에서만. 이 변경 전에는 R이 G/M 경로와 평면 반사를 먼저 만들고, K는 이 요청이 반영되면 연결한다.

## 처리 결과 (코어, 2026-09-25, INTERFACES v1.2)

- 반영(인터페이스): `ViewResources::roughnessTiles`(R8_UNORM ⌈W/8⌉×⌈H/8⌉, 생산 M, 소비 R; `Frame.h`, INTERFACES 5.1 표), R의 공개 함수
  `screenProbeRadiance(...)`·`reflectionLobeHalfAngle(...)` 이름·시그니처 고정(5.6 표), M 셰이딩의 K 경로 규칙(`reflectionRadiance(...).a == 0`이면
  M이 `screenProbeRadiance`로 평가하고 재질 모델의 스페큘러 방향 알베도 항을 곱한다; 보조 뷰는 `giCacheRadiance`).
- 비용 근거(요청의 예상치: K를 전화면 패스로 두면 ≥ 0.28 ms, 셰이딩 커널 안 평가 ≈ 0.1 ms)는 설계서 2.6·2.13 대조 대상으로 P2 게이트에서 잰다.
- 구현: 두 함수는 R, `roughnessTiles` 출력과 K 분기는 M 세션(v1.2부터 M은 별도 세션)이 한다.

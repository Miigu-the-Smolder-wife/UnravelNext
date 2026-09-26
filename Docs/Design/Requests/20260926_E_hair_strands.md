# 요청: 가닥 머리카락의 화면·그림자·광선 합류 (E → V, M, S, R) — 2026-09-26

B10(머리카락)의 E 몫은 들어갔다. 시뮬레이션·따라 가닥·세그먼트 생성은 `Native/Render/Passes/Hair`(UnravelNext 62bc275), 섬유 BSDF는 `Passes/Hair/HairBsdf.hlsli`(46ad09b)다. ARCHITECTURE 2.8에 따르면 머리카락은 전용 층이 없고 coverage 층(대역 B)으로 가므로, 아래는 각 트랙이 이미 가진 기계에 붙이는 합류점이다.

## E가 매 프레임 내는 것 (`tracks::hair`, V 앞)

- `FrameResources::hairSegments`: 세그먼트마다 `float4` 두 개 — (카메라 상대 p0, 반지름 r0), (p1, r1). r = 0이면 LOD가 뺀 가닥이니 건너뛴다. 가닥 f의 세그먼트 i는 레코드 `first + f × segmentsPerStrand + i`에 있고, 한 가닥의 세그먼트는 끝점을 공유한다(비트 동일).
- `FrameResources::hairBodies`(raw): 몸 수, 몸마다 8 워드 `{ 첫 세그먼트, 세그먼트 수, 가닥당 세그먼트, 재질, 인스턴스, LOD 유지 비율(float), 폭 배율(float), 0 }`.
- LOD: 한 몸의 따라 가닥이 픽셀 단면당 2개를 넘으면(ARCHITECTURE 2.8), 결정적 부분집합만 남기고 폭에 1/비율을 곱해 투영 coverage를 지킨다. 그보다 촘촘한 곳의 대역 C 가닥 브릭은 V 몫이다.
- 시뮬레이션은 tick마다(2 substep) 돈다. Verlet, 전역 형상, TressFX 4 국소 형상 제약, 캡슐 충돌, DFTL(길이 정확)을 거치고, 프레임 시각의 보간 결과를 낸다. 비용: 가이드 2.5k/체 × 12 노드 기준으로 ARCHITECTURE 2.14의 0.08 ms/tick 행에 해당한다. 측정은 사용자 게임이 끝난 뒤 GpuLock으로 한다.

## V (coverage 층 대역 B)

- 세그먼트를 캡슐로 래스터한다. 세그먼트마다 보존 사각형을 쓰고, 픽셀과 세그먼트 단면(폭 2r의 투영 띠)이 겹치는 해석적 면적을 coverage 레코드의 면적으로 쓴다. 폭은 대개 픽셀보다 좁다.
- 레코드 식별: 머리카락 id 공간(몸 + 세그먼트 색인)을 쓰고, attr32에 가닥 방향 u(뿌리 0 → 끝 1)를 넣는다. M이 가닥 접선을 `p1 − p0`에서 다시 얻는다.
- 깊이: 세그먼트 위 최근접점의 깊이.

## M (재질 클래스 hair, coverage fragment 셰이딩)

- `HairBsdf.hlsli`의 `hairKernel(wo, wi, h, eta, sigma_a, betaM, betaN, tilt)`을 쓴다. 섬유 좌표계의 x는 가닥 접선 `normalize(p1 − p0)`이다.
- 픽셀보다 가는 가닥은 h를 [−1, 1]에서 적분한다(원거리 BCSDF). Gauss–Legendre 몇 점을 쓸지는 비용식으로 M이 정한다.
- 흡수는 멜라닌 농도에서 PBRT 4e 9.9의 식으로 σ_a를 만든다. 재질 매개변수(멜라닌, betaM, betaN, tilt, eta)는 재질 표에 둔다.
- 시험: `unx_test_hair_hairtests`가 백색로(albedo 1)와 pdf 정규화를 확인한다.

## S (그림자)

- 세그먼트를 VSM 캐스터로 쓴다. 머리카락은 반투명이므로 v1.26의 VSM 투과율 층에 coverage × (1 − 섬유 투과)를 누적한다(deep shadow).

## R (광선)

- 표준 DXR만 쓴다(휴대성 규칙: 벤더 전용 곡선·LSS 금지). 세그먼트마다 절차 AABB를 두고, 캡슐·원기둥 교차 셰이더를 쓰거나 삼각형 관 BLAS를 쓴다. 둘 중 무엇을 쓸지는 비용식으로 R이 정한다. hit 셰이딩은 M과 같은 `hairKernel`을 쓴다.
- 반사·GI에 머리카락을 넣을지와 그 정확 조건은 R이 비용식으로 정한다(ARCHITECTURE 2.6 "광선이 실제로 닿는 대상").

## 호스트 (I)

- `hair::hairSystem(renderer.trackState())`에서 `addBody(BodyDesc)`를 호출한다. BodyDesc는 가이드 휴지 위치(관절 공간), 가이드별 관절, 따라 가닥 오프셋, 반지름, 재질, 인스턴스를 담는다.
- World tick마다 `tick(body, joints, capsules, wind, dt)`를 호출한다. 관절은 월드 변환, 캡슐은 몸의 충돌 캡슐, 바람은 렌더 B의 `windExact` 값이다.
- 프레임마다 `setFrameFraction(w)`를 호출한다.

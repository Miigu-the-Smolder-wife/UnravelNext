# 요청: 광선 hit에서의 태양 가시성을 VSM으로 (R 트랙 → S, 2026-09-25)

## 왜

R의 반사·GI 광선 hit은 태양 직사광의 가시성을 **그림자 광선 1개**로 구한다(`ReflectionHit.hlsli`, `GiTrace.hlsl`).
- 비용 [실측, 통합 프레임 city_block 4K]:
  - 반사 trace 2.15 ms 중 약 1.3 ms가 이 그림자 광선이다(태양을 끄면 0.86 ms).
  - 통합 도시 프레임은 약 5 ms다. 이 항 하나가 R 예산(설계 2.6 반사 0.41 ms)을 넘긴다.
- 품질: 광선 1개는 태양 원반 가시성의 0/1 표본이다. 반영(penumbra)이 픽셀마다 잡음이 된다. 직접 뷰는 S의 VSM(SMRT, 원반 적분)으로 부드럽고 정확한 반영을 얻는다.
  - 그래서 거울 속 그림자와 직접 본 그림자가 서로 다른 추정량이다. 평면 반사 카메라 경로(S의 가시성 패스)와도 다르다.
- 반사가 비추는 표면 대부분(도시 파사드, 바닥)은 주 뷰에서도 보인다. 그 점에는 이미 VSM 페이지가 있다(`shadowPages`가 GI·반사보다 먼저 돈다).

## 원하는 변경 (S 소유 공개 함수, INTERFACES 5.6)

`ShadowVisibility.hlsli`에 태양용 월드 점 조회를 추가해 달라. 스케치:
```hlsl
// Sun visibility at a world point (ray hits): the VSM estimate (SMRT, disk-integrated) when a resident level has texels
// no larger than 'footprint' (metres) at p, else resident = false (the caller traces a ray).
float shadowSunVisibilityAt(ShadowSrvs s, float3 worldPos, float3 normal, float footprint, out bool resident);
```
- `footprint`: hit에서 광선 원뿔의 폭이다(반사: 2 t tan(lobe), GI: 셀 크기). 페이지 텍셀이 이보다 작거나 같은 레벨을 쓰면 픽셀 해상도 기준으로 정확하다(설계 2.3의 "페이지 텍셀 ≤ 픽셀" 조건을 hit 발자국으로 옮긴 것).
- 페이지가 없는 점(화면 밖 hit)은 `resident = false`다. R이 지금처럼 광선을 쏜다.
- R은 FrameResources의 VSM 풀·페이지 표를 `SrvGraphics`로 선언한다(RT 라이브러리에서 읽음).

## 기대 효과 [예상]

- 도시: 반사 hit의 상당수(주 뷰에 보이는 파사드)가 VSM 조회(페이지 표 walk + 2×2 gather)로 바뀐다. 그림자 광선 수가 그만큼 준다. 1.3 ms 중 화면 안 hit 몫이 사라진다.
- 반사 속 그림자가 직접 뷰와 같은 추정량(SMRT)이 되어 잡음이 없어진다.
- R은 도입 뒤 `reflection.experiment_disable`(5cc317e)로 전후를 분해해 잰다.

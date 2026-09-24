# 요청: `roughnessTiles`의 값을 "타일 최소 반사 로브 반각"으로 (R 트랙, 2026-09-25, v1.2 보완)

## 왜

v1.2의 `ViewResources::roughnessTiles`는 8×8 타일의 **최소 지각 거칠기**다. R은 이 값으로 "타일 전체가 K 경로라서 광선이 필요 없는가"를
판단하려 했는데, 거칠기만으로는 판단할 수 없다.

- 설계서 2.6의 K 조건은 로브의 **좁은 축** θ_r,narrow = θ_r · cos θ_o ≥ 22°다(스침각에서 로브가 입사면에 수직한 방향으로 cos θ_o만큼 압축).
- θ_r ≤ 120°(α = 1)이므로 cos θ_o < 0.18(θ_o > 79°)인 픽셀은 **거칠기와 무관하게** K가 아니다. 먼 도로·바닥·벽면처럼 스침각 픽셀은 흔하다.
- 그래서 최소 거칠기가 높은 타일도 스침각 픽셀이 있으면 G/M 광선이 필요하다. R이 이를 알려면 모든 타일의 법선을 읽어야 한다
  (G-buffer RG32 66 MB + depth 33 MB ≈ 0.17 ms [예상], 4K).

M의 재질 해석은 픽셀의 법선·거칠기·시선을 이미 레지스터에 갖고 있으므로, 같은 비용(타일당 웨이브 min + 저장 1회)으로 정확한 판단값을 낼 수 있다.

## 원하는 변경

- `roughnessTiles` (R8_UNORM ⌈W/8⌉×⌈H/8⌉, 생산 M, 소비 R)의 값을
  **min over 타일의 표면 픽셀 of `reflectionLobeHalfAngle(perceptualRoughness, NoV)` / π** (라디안을 π로 나눈 unorm8; 하늘만인 타일 = 1)로 바꾼다.
  `reflectionLobeHalfAngle`은 R의 `Passes/Reflection/Reflection.hlsli`(v1.2 고정 함수)다. M과 R이 같은 식을 쓴다.
- 이름은 그대로 두거나(`roughnessTiles`), 코어가 원하면 `reflectionLobeTiles`로 바꾼다(R은 어느 쪽이든 따른다).
- unorm8의 해상도(π/255 = 0.7°)는 22° 경계 판정에 충분하다. R은 값 < 22°(+ 반올림 여유 1단계)인 타일만 픽셀 단위로 본다.

## 영향

- M: `materialResolve`의 타일 출력 식 하나(`reflectionLobeHalfAngle` include). 코어: `Frame.h` 주석과 INTERFACES 5.1 표의 설명.
- R: 이 변경이 들어오기 전에는 모든 타일을 픽셀 단위로 분류한다(비용은 R 상태 문서에 실측으로 기록).

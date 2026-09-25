# S 요청: fragment 가시성 출력 (INTERFACES 7.3 확장, COVERAGE_REDESIGN 4.3)

작성: S, 2026-09-26. 대상: core(Frame.h 필드), M(합성 커널 입력), V(깊이 구간 입력).

## 1. 입력 (V 생산)

- `ViewResources::coverageDepthRange` (TextureRef, R32G32_UINT, 픽셀당): 그 픽셀 fragment들의 device depth(reversed-Z) float 비트, x = 가장 가까운 것(max), y = 가장 먼 것(min). fragment가 없는 픽셀은 (0, 0xFFFFFFFF)다. 투명(see-through) 레코드도 포함한다. 생산 위치는 V가 정한다(조율 의견: 래스터 뒤 타일 패스 MODE 3).
- 이것이 없는 빌드에서는 S가 시험 경로에서만 레코드 스캔으로 대신한다. 제품 경로에서는 쓰지 않는다.

## 2. 출력 (S 생산, M 소비)

- `ViewResources::shadowFragmentVisibility` (BufferRef, StructuredBuffer<uint3>, 픽셀당 12 B, 색인 y × width + x). coverageDepthRange가 있는 픽셀만 유효하다.
  - `.x` sunT4: 선분 z_near → z_far(view 광선 위, 픽셀 중심) 위의 점 k = 0..3, z_k = z_near + (z_far − z_near) × k / 3에서 태양 가시성(불투명 분류 × 투과율 층 T). unorm8이고 바이트 k에 둔다. M은 fragment 깊이로 선형 보간한다(선형 view depth 기준).
  - `.y` localNear: 바이트 1~3 = 국소광 슬롯 1~3의 가시성(7.3 부호화, 255 = 슬롯 없음)을 z_near에서 잰 값이다. 바이트 0 bit 0 = **pair 플래그**(아래)다.
  - `.z` localFar: 같은 값을 z_far에서 잰 것이다. 바이트 0은 예비다.
- `ViewResources::shadowFragmentSun` (BufferRef, raw, coverage 레코드 풀 원소당 1 B): pair 플래그가 켜진 픽셀의 fragment만 유효하다. 바이트 f = unorm8 태양 가시성이고, f = (청크 − 1) × 64 + i % 64(풀 원소 번호, M과 합의)다. 플래그 없는 픽셀의 바이트는 이번 프레임에 쓰이지 않는다. 옛 값이 남을 수 있으니 플래그 없이 읽지 않는다.

## 3. S 계산 (정확 조건)

1. 구간 분류: 두 끝점의 광공간 높이 [h_lo, h_hi]와 두 끝점 reach 정사각형을 감싸는 정사각형(중간 점의 reach 정사각형은 모두 이 안에 든다)으로 블록 계층을 검사한다. 그 안의 모든 텍셀이 h_lo 이하면 전부 lit이고, 모든 텍셀이 h_hi 위면 전부 umbra다. 둘 다 아니면 mixed다. lit/umbra 판정은 정확하다(보수적 포함).
2. lit/umbra 픽셀: sunT4 = 255/0 × 투과율 층 T(z_k). 층이 비어 있는 동안 T = 1이다.
3. mixed 픽셀: pair 플래그를 세우고, pass 2가 그 픽셀의 fragment마다 픽셀 receiver와 같은 SMRT(vsmSunVisibility, fragment 법선의 receiver 평면)를 돌려 shadowFragmentSun에 쓴다.
4. 국소 슬롯: 픽셀 receiver와 같은 함수(shadowLocalVisibilityAtReceiver)를 z_near·z_far 두 점에서 계산한다(프록셀 목록은 각 점의 깊이 것).
5. 루프 상한(INTERFACES 3.6): pass 2의 타일 레코드 루프는 타일 fragment 수, 상한은 풀 용량이다. 닿으면 S 오류 비트(VSM stats 새 단어)를 세운다.

## 4. 게이트

- 시험: fragment_exact 모드(`shadow.fragment_exact = 1`)에서 모든 fragment를 직접 SMRT로 계산한 값과 비교한다. M의 보간값(sunT4, pair)과의 차이 ≤ 1/255다(lit/umbra 확정 픽셀은 정확히 같아야 한다).
- 통계: 탭 0회 조건(14.7 (ii)) 성립 비율, mixed 픽셀 비율, pair 수. forest_combat eye/up/edge.

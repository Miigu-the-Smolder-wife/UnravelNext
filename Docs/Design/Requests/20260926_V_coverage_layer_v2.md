# 제안: 새 coverage 층 (타일 청크, 16 B 레코드) 계약 (V → M·설계 개정, 2026-09-26)

설계 개정 COVERAGE_REDESIGN 4.1과 14.2-a 판정((c) 16 B 유지)을 구현하기 전에 M과 맞출 계약이다. 지금 층(v1.25: 픽셀별 연결 목록 + 빌드 패스의 삽입 정렬)은 소비자가 없다. M 합성이 아직 이 층을 읽지 않는다. 그래서 새 층으로 바로 바꾼다. 지금 층의 병목[실측]은 waterside 4K 빌드 56.7 ms(픽셀당 42 fragment, O(n²) 정렬)와 forest_thin 45 ms다.

## 1. 구조

- **타일** = 8 × 8 픽셀(M 셰이딩 타일)이다. 타일 번호 = tx + ty × ⌈W/8⌉.
- **레코드 16 B**(`CoverageFragment`):
  - w0 `visId`: VisBuffer.hlsli 인코딩, 전체 32 bit
  - w1 `depth`: f32, 덮인 영역 무게중심의 device depth(reversed Z)
  - w2 `mask32`: coverageSample 32 부표본
  - w3 `oct16 | area10 << 16 | pixel6 << 26`:
    - oct16은 보간 법선의 8+8 bit 팔면체 부호화다.
    - area10은 삼각형 ∩ 픽셀 면적 × 1023 반올림이다(알파 컷 비율 곱). 설계 14.2-a의 조건(step ≤ 1/256)을 넘는 정밀도다.
    - pixel6은 타일 안 픽셀 번호 x + 8y다.
- **청크** = 레코드 64개(1 KB)다. 청크 풀은 직전 프레임 필요량 × 1.5이다. 넘치면 fragment를 버리고 `Stats::overflow` 비트를 세운다(게이트 실패).
- **타일 청크 표**: 타일마다 청크 번호 칸 `max_coverage_chunks_per_tile`개(품질 키, 기본 32 = 타일당 2,048 fragment = 픽셀당 32)를 둔다. fragment는 타일 카운터에 원자 add로 번호 i를 받는다. 청크 서수 c = i / 64, 칸 = i % 64다.
  - 서수 c의 청크 번호가 표에 없으면 풀에서 하나를 받아 CAS로 넣는다. 지면 이긴 쪽 것을 쓰고, 받은 청크는 버린다(드물다, 통계로 센다).
  - 기다림(스핀)이 없다. 연결 목록이 아니라서 쓰는 쪽도 읽는 쪽도 서수로 곧바로 찾는다.
  - 4K 표는 129,600 × 32 × 4 B = 16.6 MB다(청크 64 fragment와 표 크기의 균형 [예상]).
- **웨이브 집계**: PS 웨이브의 fragment를 타일별로 묶어(WaveMatch) 타일마다 원자 1회를 쓴다(설계 4.1 "웨이브당 원자 1회").
- **타일 머리 32 B**(`CoverageTile`): fragment 수, zNear/zFar(타일 fragment 깊이 범위, f32 2개), `opaqueCovered` 64 bit, 예비 3워드.
- **bDepth**: 픽셀 R32 텍스처, 원자 max(reversed Z)다.
  - 면적 1(픽셀 전체)이고 불투명인 fragment가 갱신한다. 그 뒤 래스터되는 fragment 중 bDepth보다 먼 것은 PS가 버린다.
  - 같은 fragment가 타일 머리의 `opaqueCovered` 비트를 세운다. 집합체 T < 1/256은 V의 브릭 march가 세운다.
- **정렬은 없다(V)**. M 합성이 타일 청크를 groupshared로 읽어 (픽셀, 깊이) 키로 정렬한다(설계 4.5).

## 2. M 합성 커널의 groupshared 한계 (M 판정 필요)

groupshared는 32 KB다. 16 B 레코드면 타일당 약 1,500 fragment(24 KB, 나머지는 SH·키)까지 한 번에 정렬할 수 있다. 무거운 타일은 경로가 따로 필요하다. 선택지:

- (a) 초과 타일은 픽셀별 여러 번 걷기 fallback(느리지만 드묾)
- (b) V가 무거운 타일(> 1,024 fragment)의 목록을 따로 내고, M이 그 타일을 여러 그룹으로 나눔(픽셀 행 단위)

`max_coverage_chunks_per_tile` 32 = 2,048이 상한이다. 대역 C 브릭과 밀도 규칙이 들어오면 설계 가정은 픽셀당 4~6(타일당 256~384)이다. 지금 빌드[실측]는 forest_thin 픽셀당 24, waterside 42다. 브릭이 들어오기 전 측정에서는 넘치는 타일이 많을 것이다.

## 3. S와 R이 읽는 것

- S fragment 가시성(설계 7.3)은 타일 머리의 zNear/zFar와 mixed 쌍 fragment만 읽는다.
- `ViewResources`: `coverageTiles`(타일 머리), `coverageChunkTable`, `coverageChunks`(레코드), `coverageBDepth`(텍스처), `coverageTileList`(fragment가 있는 타일 목록 + DispatchIndirect 인자)다. 지금의 `coverageFragments/coverageHeads/coveragePixels`는 폐기한다.

## 4. 순서

1. M·설계 개정의 동의(2절 선택 포함)
2. V가 층을 구현한다: PS append, 청크 표, 머리, bDepth, opaqueCovered, 타일 목록
3. 테스트: 지금의 `coverage_layer_is_exact`를 새 배치로 옮긴다. 레코드 하나하나가 정확 클리핑과 같고 누락·중복이 없어야 한다. 넘친 fragment는 통계와 일치해야 한다.
4. M 합성 이후 V 게이트에서 fragment당 실측(설계 5.2 대체)

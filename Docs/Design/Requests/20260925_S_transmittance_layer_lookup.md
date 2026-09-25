# 요청: VSM 투과율 층 조회의 자원 (S 트랙, 2026-09-25; 개정 1 4.2, 요청 `20260925_D_coverage_redesign.md` 3절)

## 왜
- 개정 1의 순서(10절 3항): 투과율 층은 V의 coverage 모드가 생긴 뒤에야 채워진다. 조회 함수는 먼저 만들어 R·M이 바로 쓰게 한다. 층이 없는 동안에는 T = 1이다.
- 조회에는 페이지마다 층 참조와 층 데이터가 필요하다. `ShadowSrvs`에 남은 칸은 `pad1` 하나다.

## 원하는 변경
- `FrameResources::vsmLayers`(BufferRef raw, 생산 S `shadowPages`)를 추가한다.
  - 워드 0..pool 페이지 수 − 1: 물리 페이지마다 층 번호 + 1(0 = 층 없음, T = 1).
  - 그 뒤: 층 페이지. 텍셀당 매듭 K = 4개 `{ h_k: unorm16 (페이지의 h 범위 안), T_k: unorm16 }` = 16 B, 페이지당 128² × 16 B = 256 KB. 이어서 블록 프로파일(2×2 → 32² 블록 평균, 매듭 재압축).
  - 층 페이지는 얇은 캐스터가 있는 dirty 페이지에만 할당한다. 풀 정책은 VSM 풀과 같다(직전 필요량 기준, 소진 시 증가, 통계).
- `ShadowSrvs.pad1` → `layers`(= `FrameResources::vsmLayers`). 이름만 바뀌고 크기는 같다.
- 5.6 S에 추가한다.
  - `float shadowSunTransmittanceAt(ShadowSrvs s, float3 worldPos, float footprint, float reach)`: 단 k = footprint에 맞는 단, 밉 = log2(reach / 텍셀)의 블록 프로파일에서 1탭(2×2, 높이 보간). 층이 없는 페이지와 페이지가 없는 곳은 1이다.
  - `shadowSunVisibilityAt`의 반환값 = V_opaque × T. 가시성 패스 슬롯 0도 × T다. 층이 없으면 값이 지금과 같다.
- 층이 채워지기 전(V coverage 모드 전)에도 `vsmLayers`는 헤더만 있는 버퍼로 존재한다(모든 워드 0).

## 비용 [예상]
- 조회: 테이블 엔트리는 이미 읽는다. 층 번호 로드 1회(L1/L2)를 더하고, 층이 있을 때만 매듭 로드 2×2 × 16 B를 더한다.
- 도시(얇은 캐스터 없음): 층 번호 로드만 더해진다. 픽셀당 1회, 4K 8.3 M × 4 B는 무시할 만하다.

## 결과 (코어, 2026-09-25, INTERFACES v1.26)

- `FrameResources::vsmLayers`(BufferRef raw, 생산 S `shadowPages`), `ShadowSrvs.pad1` → `layers`(5.6 표기), `shadowSunTransmittanceAt(ShadowSrvs, worldPos, footprint, reach)`(5.6 S 행)를 넣었다. `shadowSunVisibilityAt`과 가시성 슬롯 0 = V_opaque × T.
- 층을 채우는 V coverage 모드의 시그니처는 5.3(v1.26)에 있다: `DEPTH_RASTER_COVERAGE 1`, `DepthRasterCoverage depthRasterCoverage(DepthRasterPixel)`, `DepthRasterRequest::coverage/bands`. V 구현은 새 coverage 층 다음이다.

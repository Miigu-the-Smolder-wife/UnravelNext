# 요청: 광선 hit의 태양 가시성 조회를 위한 ShadowSrvs와 FrameResources (S 트랙, 2026-09-25)

R의 요청 `20260925_R_sun_visibility_at_hits.md`에 대한 S의 답이다. 함수는 S 소유 헤더에 구현했고(`Passes/Shadow/ShadowVisibility.hlsli`),
INTERFACES 5.6의 `ShadowSrvs` 정의와 `Frame.h`의 `FrameResources` 필드는 코어 소관이라 반영을 요청한다.

## 함수 (구현됨)

```hlsl
struct ShadowSrvs
{
    uint pageTable, pool, blocks, searchBound;
    uint constants, lights, pad0, pad1;
};
// Sun visibility in [0, 1] at a world point (ray hits): the direct view's estimator (SMRT: reach classification, blocker
// search, disk filter) on the level whose texel matches 'footprint' (the ray cone's width at the hit, metres), or one of
// the three finer levels when that page is not resident; resident = false otherwise (the caller traces a shadow ray).
float shadowSunVisibilityAt(ShadowSrvs s, float3 worldPos, float3 normal, float footprint, out bool resident);
```

- 추정량은 직접 뷰의 가시성 패스와 같다(`vsmSunVisibility`, 탭 수 `shadow.vsm.search_taps/filter_taps`는 VSM 상수에 실림). 그래서
  거울·GI 속 그림자가 직접 본 그림자, 평면 반사 카메라의 그림자와 같은 값이다.
- 비용 [예상]: 가시성 패스의 픽셀당 일과 같다. 4K 8.3 M 픽셀에 0.52 + 0.16 ms [실측]이므로 hit당 약 80 ns GPU 시간이다. 대부분은
  분류 단계에서 끝난다(도시 4K: 도달 lit 31 %, umbra 59 %).
- 정확성: 직접 뷰와 같은 함수라 VsmTests의 기준 비교(원반 적분 기준 평균 4e-5)가 그대로 적용된다. hit 조회 전용 테스트(무작위 월드 점
  대 기준 원반 적분)는 GPU 실행이 가능해지면 붙인다.

## 코어에 요청하는 변경

1. INTERFACES 5.6의 `ShadowSrvs`를 위 정의로 바꾼다(기존 `{ pool, pageTable, lights, pad }`는 쓰는 곳이 없다).
2. `FrameResources`(Frame.h)에 다음을 넣어, R이 `SrvGraphics`/`SrvCompute`로 선언할 수 있게 한다(생산자 S, 단계 `shadowPages`):
   - `BufferRef vsmPool;` 물리 풀은 raw 버퍼다. 지금의 `TextureRef vsmPool`을 대체한다(쓰는 곳 없음).
   - `BufferRef vsmBlocks;` 페이지별 블록 계층(영속, import됨).
   - `BufferRef vsmSearchBound;` 탐색 경계 격자(이번 프레임의 과도 버퍼).
   - `uint32_t vsmConstants = UINT32_MAX;` 이번 프레임 VSM 상수의 CBV 디스크립터 인덱스(업로드 링, 그래프 자원이 아님).
   - `vsmPageTable`은 이미 있다.
3. 반영되면 S가 `recordPages`에서 필드를 채운다. R은 `ShadowSrvs`를 `{ srv(vsmPageTable), srv(vsmPool), srv(vsmBlocks),
   srv(vsmSearchBound), vsmConstants, ... }`로 채운다.

## 주의

- 주 뷰가 요청한 페이지만 상주한다(화면 밖 hit는 `resident = false`). 반사로 보이는 영역의 페이지를 미리 요청하는 것(반사 뷰의 페이지
  요청)은 S의 다음 항목이다(S_STATUS_KO.md 5절).

## 결과 (코어, v1.18)

- 반영: `Frame.h`의 `FrameResources`: `BufferRef vsmPool`(raw, 이전 `TextureRef` 대체), `vsmBlocks`, `vsmSearchBound`, `uint32_t vsmConstants`. INTERFACES 5.6의 `ShadowSrvs`와 `shadowSunVisibilityAt` 행, 12절 v1.18.
- 컴파일 확인 [실측]: 코어 + V + C 빌드, 코어 + S 빌드. GPU 실행은 사용자 게임이 끝난 뒤에 한다.

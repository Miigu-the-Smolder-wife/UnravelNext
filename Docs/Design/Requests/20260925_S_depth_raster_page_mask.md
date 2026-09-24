# 요청: 깊이 래스터 서비스에 GPU 페이지 마스크 컬링 추가 (S 트랙, 2026-09-25)

## 왜 필요한가

설계서 2.3의 VSM 비용식은 **dirty 페이지에 들어가는 삼각형만** 래스터한다는 가정이다: T_sun ≈ 6 M(정지·보행), 60 m/s 이동 12 M
→ 0.20~0.40 ms. dirty 페이지 집합은 GPU가 매 프레임 정한다(깊이 버퍼로 요청 표시 → 할당 → 캐스터 변경·바람·새 페이지 판정).
CPU는 그 집합을 모른다(readback은 1프레임 지연 + 동기 비용).

현재 `DepthRasterRequest`(INTERFACES 5.3)는 CPU의 `RasterView` 목록(뷰포트·viewProj)만 받는다. S가 할 수 있는 것은 클립맵
단마다 뷰 하나(16384² 가상 뷰포트)를 요청하고 픽셀 커널에서 dirty가 아닌 페이지를 버리는 것뿐이다. 그러면 V는 각 단의 절두체
안 캐스터를 **전부** 래스터한다. 12단 클립맵은 카메라 근처 기하를 12번 그린다 → T_sun ≈ 12 × (절두체 안 캐스터 전체), 비용식의
수십 배다. 픽셀 커널에서 버리는 것은 래스터·메시 셰이더 비용을 줄이지 못한다.

## 원하는 변경 (`Native/Render/include/unx/render/Frame.h`, INTERFACES 5.3)

```cpp
struct RasterView
{
    ...
    uint32_t cullMaskOffset = UINT32_MAX;  // 이 뷰의 페이지 마스크가 cullMask에서 시작하는 uint32 단어 위치; UINT32_MAX = 마스크 없음
};

struct DepthRasterRequest
{
    ...
    BufferRef cullMask;          // raw 버퍼. 뷰마다 ceil(viewportWidth / cullTilePx) × ceil(viewportHeight / cullTilePx) 비트,
                                 // 행 우선, 비트 i = 단어 (i >> 5)의 (i & 31)번째 비트. 1 = 이 타일에 래스터 필요.
    uint32_t cullTilePx = 0;     // 타일 한 변(픽셀). S는 128 (shadow.vsm.page_texels)
};
```

- V는 클러스터(와 가능하면 메시 셰이더의 삼각형)의 뷰포트 투영 사각형이 **1 비트도 켜지지 않은 타일만** 덮으면 버린다. 마스크는 S가
  같은 프레임의 앞선 패스에서 GPU로 쓴다. V는 `cullMask`를 `SrvGraphics`/`SrvCompute`로 선언하고 필요하면 내부에서 계층(예: 8×8 타일
  OR) 마스크를 만든다.
- 이 필터는 **성능용**이다. 정확성은 S의 픽셀 커널이 보장한다(dirty가 아닌 페이지의 픽셀은 커널이 쓰지 않는다). 따라서 V가 보수적으로
  더 그려도 결과는 같다.

## 함께 확인할 것 (문서 명확화, 코드 변경 없을 수 있음)

1. **직교 뷰의 LOD 척도.** 태양 클립맵 뷰는 직교 투영이다. `RasterView::lodPixelsPerMetre`는 "거리 1에서 미터당 텍셀"로 정의돼 있다.
   직교 뷰에서는 거리와 무관하게 미터당 텍셀이 일정(= 1 / 텍셀 크기)하다. V가 viewProj의 마지막 행 (0,0,0,1)로 직교를 판별하고
   `lodPixelsPerMetre`를 거리 무관 척도로 쓰는지 명시해 달라. (S는 직교 뷰에 1/τ_k를 넣는다.)
2. **렌더 타깃 없는 16384² 뷰포트.** S는 `depthTarget` 없이 픽셀 커널만 쓴다(원자적 깊이 쓰기, 페이지 테이블 간접). 뷰포트는 클립맵
   단의 가상 해상도 16384² 전체다. 렌더 타깃·깊이 없이 UAV만 쓰는 래스터(ForcedSampleCount 0)로 처리되는지 확인해 달라.
3. **컬 모드.** 그림자 깊이는 양면(`D3D12_CULL_MODE_NONE`)이어야 한다(닫히지 않은 메시·잎 카드·two-sided 재질). V가 `instanceMask`
   요청에서 재질의 two-sided 여부와 무관하게 양면으로 그리는지, 아니면 요청 필드(`cull`)가 필요한지 정해 달라. S는 양면을 원한다.
4. **alpha test.** 잎 카드 그림자는 alpha test가 필요하다(8.1). V의 깊이 래스터가 alpha-tested 재질에서 픽셀 커널 호출 전에 버리는지
   (또는 S의 픽셀 커널이 재질을 알 수 있게 `userData` 외에 vis id류 입력을 받는지) 정해 달라.

## 영향

- 코어/V: `Frame.h` 필드 3개 추가(기본값이면 지금과 같은 동작), V의 클러스터 컬링에 마스크 검사.
- S: 이 변경 전에는 dirty 페이지 래스터가 기능적으로는 맞지만 비용식을 넘는다. S의 테스트는 S 소유의 테스트용 래스터
  서비스(`Passes/Shadow/Tests/`)로 같은 계약을 흉내 내 정확성을 검증한다. 성능 게이트의 페이지 래스터 항은 V 연결 뒤에 잰다.

## 처리 결과 (코어/V, 2026-09-25, INTERFACES v1.1)

- 인터페이스 반영(`Frame.h`, INTERFACES 5.3): `RasterView::cullMaskOffset`(기본 `UINT32_MAX` = 마스크 없음), `DepthRasterRequest::cullMask`(BufferRef, raw), `cullTilePx`, 그리고 `cull`(아래 3). 비트 배치는 요청한 그대로다. 기본값이면 지금과 같은 동작이다.
- 확인 사항:
  1. 직교 뷰 LOD: V가 viewProj 마지막 행 (0,0,0,1)로 직교를 판별하고, 그때 `lodPixelsPerMetre`를 거리와 무관한 미터당 텍셀로 쓴다. 원근이면 거리 1에서의 미터당 텍셀(초점 거리). S가 직교 뷰에 1/τ_k를 넣는 것이 맞다.
  2. 렌더 타깃·깊이 없는 16384² 뷰포트: `depthTarget`이 무효이고 `pixelKernel`이 있으면 RT·DSV 없는 UAV 전용 래스터(1 표본, ForcedSampleCount 0)로 그린다. 뷰포트 16384²는 D3D12 한도 안이다.
  3. 컬 모드: `DepthRasterRequest::cull` 필드를 추가했고 기본값이 `D3D12_CULL_MODE_NONE`(양면)이다. `BACK`을 주면 one-sided 재질만 뒷면을 버린다.
  4. alpha test: 픽셀 커널 입력을 `struct DepthRasterPixel`(`Native/Render/Passes/Visibility/DepthRaster.hlsli`: `SV_Position`, `uv : TEXCOORD0`, `userData : USERDATA`, `material : MATERIAL`)로 정하고, 커널이 쓰기 전에 `depthRasterCovered(p)`를 불러 거짓이면 `discard`한다. 본 뷰·기준과 같은 규칙(8.1)이다.
- V 구현: 서비스 본체(클러스터 컬링 + 마스크 검사 + 메시 셰이더 + 요청자 픽셀 커널 PSO)는 V의 P1 작업으로 들어간다. 코어 세션은 V의 클러스터 파이프라인을 본 뷰와 이 서비스에 같이 쓰도록 짜고 있으며, 들어오면 12절과 이 파일에 적는다. 그 전까지 `rasterizeDepth`는 빈 구현이고, S의 정확성 검사는 지금처럼 S의 테스트용 래스터 서비스로 한다.

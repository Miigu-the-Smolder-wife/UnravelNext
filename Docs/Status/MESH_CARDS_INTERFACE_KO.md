# 메시 카드 표면 캐시: A(카드·캡처·아틀라스) ↔ S2(조명·읽기) 인터페이스

2026-10-02, A. 조율 08:20 지시(광선이 닿은 곳에만 셀을 만드는 대용품 폐기, 언리얼의 메시 카드로 다시). 이 한 장이 두 세션의 경계다. 값은 언리얼 기본값(cm → m 환산), 괄호 안이 원본 이름.

읽은 원본: 생성 `UE_5.8/Engine/Source/Developer/MeshUtilities/Private/MeshCardRepresentationUtilities.cpp`(전체), `MeshCardRepresentation.cpp`; 런타임 ue6-main `Lumen/LumenMeshCards.cpp`, `LumenSceneRendering.cpp`, `LumenSceneCardCapture.cpp`, `LumenSurfaceCache.cpp`, `LumenSurfaceCacheFeedback.cpp`, `LumenSceneGPUDrivenUpdate.cpp`, `LumenSceneData.h`, `Lumen.h`; 셰이더 `LumenCardCommon.ush`, `SurfaceCache/LumenSurfaceCacheSampling.ush`, `SurfaceCache/LumenSurfaceCache.ush`.

스위치: `surface_cache.mesh_cards`(기본 false, 판정 전까지). 매개변수는 `Config/quality/surface_cache.toml`의 `[mesh_cards]`.

## 1. 나눔

| | A | S2 |
|---|---|---|
| 카드 생성(메시당, 가져오기/로드) | ○ | |
| 인스턴스 → 메시 카드 등록, 카드·페이지 레코드, 페이지 표 | ○ | |
| 해상도 결정(거리), 아틀라스 할당·퇴출, 캡처 예산 | ○ | |
| 캡처 래스터 → 기하 아틀라스 4장(깊이·알베도·법선·방출) | ○ | |
| 피드백 버퍼 읽어 고해상도 페이지 요청 처리 | ○ | |
| 조명 아틀라스(직접·간접·최종), 타일 갱신 순서, radiosity | | ○ |
| hit에서 카드 읽기(가중·깊이 검사·합성), 피드백 쓰기 | 레이아웃·좌표 함수 제공 | 읽기 함수 본체 |
| 재할당 때 옛 조명을 새 해상도로 옮기기(ResampleLighting) | 옛/새 사각형 목록 제공 | 옮기는 패스 |

A의 패스는 시야·광선과 무관하다. 카메라 위치(거리)만 본다.

## 2. 카드 (메시당, 메시 공간)

- 방향 번호 d = 0..5: −X, +X, −Y, +Y, −Z, +Z (AxisAlignedDirectionIndex). 카드는 법선이 그 방향 쪽인 표면을 그 방향 바깥에서 안쪽으로 정사영해 담는다.
- 카드 축(메시 공간, 생성기의 ClusterBasis와 같음): d/2 = 0 → X축 (0,1,0), Y축 (0,0,1); 1 → (1,0,0), (0,0,1); 2 → (1,0,0), (0,1,0). Z축 = 방향 법선.
- 카드 = 상자(중심 origin, 반크기 extent: x·y는 카드 면, z는 깊이 반범위).
- 생성 규칙(원본 그대로): 경계 + 1 cm, 복셀 10 cm(긴 변 64칸 상한, 표면 조각 1만 개 넘으면 절반씩), 칸 기둥당 광선 32개, |n·d| ≥ 0.25(NormalTreshold), 반구 광선 32개로 내부 조각 제거(맞음 > 80 % 이고 뒷면 > 20 %), 가중 = 덮임 × (가시율 + 1). 방향마다 바깥 묶음(근평면 0) 하나 + 근평면을 옮겨 가며 가중 덮임이 가장 큰 묶음을 반복 추가(가중 덮임 ≥ 15칸, 밀도 > 0.2/3). 양면 삼각형이 1/4 이상이면 바깥 묶음만. 메시당 최대 12장(MaxLumenMeshCards), 넘치면 가중 덮임 작은 것부터 버림. 카드 상자 = 묶음 칸 범위 + 깊이 쪽 앞 0.5칸·뒤 1.5칸, 메시 경계로 자름(깊이는 ±10 cm 여유).
- 등록(인스턴스당): 상자 최대 면적 > (0.1 m)²(MeshCardsMinSize)이고 카드 면적 > (0.1 m)²인 카드만, 최대 32장. 우리 인스턴스는 회전 + 균일 배율 + 이동뿐이라 언리얼의 "직교 행렬만" 조건은 항상 참.
- 1단계 대상: 변형 없는 인스턴스(스킨·바람·모프·VAT 아님). 움직이는 강체는 카드가 변환을 따라간다(재캡처 없음). 변형 인스턴스는 카드 없음(언리얼도 같다: 스켈레탈 메시는 카드가 없다) → 조회가 "카드 없음"을 돌려준다.

## 3. GPU 자료 (A가 쓰고 S2가 읽음)

`FrameResources`에 실린다(꺼져 있으면 invalid). 헤더 `Passes/SurfaceCache/MeshCards.hlsli`가 로더를 준다.

```hlsl
// meshCardsInstanceMap: StructuredBuffer<uint>, 씬 인스턴스 번호(RtSurface.sceneInstance) → 메시 카드 번호, 없으면 0xFFFFFFFF
struct McMeshCards  // 80 B
{
    float4 worldToLocal[3];  // xyz = 단위 회전(월드 → 메시 카드 공간)의 행, w = 월드 원점의 그 성분. local = mul(rot, world - origin). 메시 카드 공간은 배율이 곱해진 미터 단위
    uint cardOffset;         // 첫 카드
    uint countFlags;         // 0..15 카드 수(<= 32), bit 17 = 대부분 양면(읽기 쪽 bias + 0.5 m: 원본 50 cm)
    uint cardLookup[6];      // 방향 d마다: bit i = 이 메시 카드의 i번째 카드가 그 방향
};
struct McCard  // 112 B
{
    float3 origin;   uint packed;       // 메시 카드 공간 상자 중심. packed: 0..2 방향, 4..7 resLevelBiasX, 8..11 resLevelBiasY, 16 visible
    float3 extent;   float texelSize;   // 카드 공간 반크기(x, y, z). texelSize = 상주 단계의 월드 텍셀 크기(m)
    uint sizeInPages; uint pageTableOffset;        // 상주(잠긴) 단계: x | y << 16, 페이지 표 첫 칸
    uint hiResSizeInPages; uint hiResPageTableOffset;  // 피드백으로 올라온 가장 고운 단계(없으면 상주 단계와 같은 값)
    float4 cardToWorld[3];   // xyz = 카드 축 X, Y, Z의 월드 성분(행 = 월드 x, y, z), w = 카드 중심의 월드 좌표. world = mul(rot, cardLocal) + centre
    uint meshCards; uint pad0, pad1, pad2;
};
struct McCardPage  // 64 B
{
    uint card; uint resLevelPageTableOffset; float2 sizeInTexels;  // sizeInTexels.x == 0 → 매핑 안 됨
    float4 cardUvRect;       // 이 페이지가 덮는 카드 uv (min xy, max zw)
    float4 atlasRect;        // 아틀라스 텍셀 (min xy, max zw)
    float2 cardUvTexelScale; uint resLevelSizeInTiles;  // x | y << 16 (8 텍셀 타일)
    uint pad;
};
// pageTable: Buffer<uint2>. x = (atlasX / 8) | (atlasY / 8) << 12 | resLevelX << 24 | resLevelY << 28, y = 카드 페이지 번호. resLevelX == 0 → 비어 있음
```

좌표(원본과 부호가 다른 곳은 여기 정의가 기준):
- 카드 지역 좌표 c = (메시 카드 공간 점 − origin)을 카드 축에 투영. uv = c.xy / extent.xy × 0.5 + 0.5. 깊이 = 0.5 − c.z / extent.z × 0.5 (0 = 카드 앞면 = 캡처 시점 쪽, 1 = 뒤).
- 텍셀 중심 (i + 0.5) / 해상도.

상수: 물리 페이지 128, 가상 페이지 127(페이지 사이 0.5 텍셀 경계), 최소 할당 8², resLevel 3..11, 128 미만은 한 페이지 안 부분 할당, 타일 8 텍셀. 아틀라스 4096²(AtlasSize).

아틀라스(기하, A 소유):

| 이름 | 형식 | 내용 |
|---|---|---|
| `cardDepth` | R16_UNORM | 위의 깊이. 1.0(0xFFFF) = 표면 없음(유효 최댓값 0xFFFE) |
| `cardAlbedo` | R8G8B8A8_UNORM | rgb = sqrt(확산 반사율). 반사율 = `baseColor × (1 − metallic) + 0.45 × 스페큘러 색`(지금 `scMark`에 주는 것과 같은 정의). a 미사용 |
| `cardNormal` | R8G8_UNORM | 카드 공간 법선 xy × 0.5 + 0.5, z = sqrt(1 − x² − y²) ≥ 0 |
| `cardEmissive` | R11G11B10_FLOAT | 재질 방출(nits) × 1/16 (`MC_EMISSIVE_SCALE`; float11 상한 65,024 → 약 1.04e6 nits) |

언리얼은 BC7/BC5/BC6H로 압축해 둔다(Compress 1). 1단계는 무압축: 4096² × 12 B = 201 MB. 압축은 뒤 단계(차이 목록 1).

## 4. hit에서 카드 찾기 (S2가 쓰는 본체, 원본 SampleLumenMeshCards 순서)

1. `mc = meshCardsInstanceMap[surface.sceneInstance]`; 0xFFFFFFFF면 카드 없음.
2. 메시 카드 공간으로: `p = mul(rot, world − origin)`, `n = mul(rot, normal)`. 축 가중 w = n².
3. 축마다 w > 0이면 `cardLookup[n < 0 ? 2k : 2k + 1]`의 비트를 모은다.
4. 카드마다: 상자 검사 `|p − card.origin| ≤ 메시 카드 공간 반크기 + 0.5 × bias`(메시 카드 공간 반크기 = extent를 카드 축에서 메시 축으로 되돌린 것; `mcCardLocal()`이 카드 지역 좌표와 함께 판정을 준다).
5. `mcCardSample(card, cardIndex, c.xy, hiRes)` → 페이지 표 → 아틀라스 uv, 4텍셀 쌍선형 가중, 카드 페이지 번호, 타일 좌표(원본 ComputeSurfaceCacheSample: 페이지 경계 0.5 텍셀 안쪽으로, 카드 가장자리 [0.5, 해상도 − 0.5 − 1/512]로 자름).
6. `cardDepth`를 4개 gather, 텍셀마다 유효(< 1)이고 `1 − saturate((|hitDepth − texelDepth| − bias / extent.z) / (0.25 × bias / extent.z))`로 가시 가중. 카드 가중 = 축 가중 × Σ(쌍선형 × 가시).
7. 조명 아틀라스를 같은 4텍셀 가중으로 읽어 카드 가중으로 합친다. 가중 합이 0이면 유효하지 않음.
8. 피드백: `mcFeedback(...)`(A 제공)이 16×16 화면 타일당 한 픽셀(TileSize 16, 프레임마다 지터)에서 `cardPageLastUsed[page] = 프레임`, 고해상도 요청 `(cardIndex | resLevel << 24, pageX | pageY << 8)`을 적는다. 원하는 단계 = `log2(max(extent.x, extent.y) / max(sampleRadius, 0.01 m)) − 0.5`(ResLevelBias −0.5)를 3..11로 자름.

A가 주는 것(`MeshCards.hlsli`): `mcLoadMeshCards / mcLoadCard / mcLoadCardPage`, `mcCardLocal`, `mcCardSample`, `mcDecodeAlbedo / mcDecodeNormal(card, xy) / mcDecodeEmissive`, `mcCardWorldPosition(card, uv, depth)`, `mcFeedback`. 2~7의 누적 함수와 조명 읽기는 S2.

## 5. 해상도·예산·상주 (A, 원본 값)

- 범위: 카메라에서 300 m 안(RayTracing.Culling.Radius 30000)의 인스턴스. 시야 무관.
- 카드 해상도 = min(100 × 최대 반크기 / 거리, 20 텍셀/m × 최대 반크기)(CardTexelDensityScale 100, CardMaxTexelDensity 0.2/cm), 거리는 카드 상자까지, 최소 1 m. 2의 거듭제곱으로 올림, 상한 512(CardMaxResolution). 4 미만이면 숨김(CardMinResolution 4), 할당 최소 8. 종횡비는 resLevelBias로 한쪽을 줄인다.
- 이 "상주 단계"는 범위 안이면 항상 있다(잠긴 페이지). 그 위 단계는 피드백이 요청할 때만(페이지 요청 16번 이상: MinPageHits), 256프레임 안 쓰이면 퇴출(NumFramesToKeepUnusedPages).
- 프레임당 캡처: 페이지 300개(CardCapturesPerFrame), 텍셀 4096² / 64 = 262,144(CardCaptureFactor 64). 가까운 것부터(거리 16단 통, 재할당은 새 카드보다 뒤). 그중 1/8(CardCaptureRefreshFraction)은 오래된 페이지 재캡처.
- 자리가 없으면 2프레임 넘게 안 쓰인 고해상도 페이지를 퇴출, 그래도 없으면 단계를 내려 할당.

레벨을 처음 열면 범위 안 카드가 이 예산으로 채워진다(컷과 무관: 한 번 채워지면 카메라가 어디를 보든 있다).

## 6. 프레임 순서와 S2 갈고리

A의 기록(반사·GI보다 먼저, `GiTrack` 앞):
1. `mc.update`(CPU): 거리 → 요청 → 할당/퇴출. 결과: 이번 프레임 캡처 목록.
2. `mc.capture`: 캡처 목록의 페이지를 임시 캡처 아틀라스(512² 층 4장)에 래스터. 그리는 것은 그 카드의 인스턴스 자신뿐(이웃 메시는 안 들어간다: 원본과 같음).
3. **S2 갈고리 `resample`**: 재할당된 카드(옛 페이지가 있는 것)의 옛 조명을 새 사각형으로. 이때 GPU의 페이지 표·카드 레코드는 아직 지난 프레임 것. A가 주는 것: `MeshCardCapture` 목록(CPU 벡터 + GPU 버퍼) = 페이지마다 {카드 페이지 번호, 카드 번호, 캡처 아틀라스 사각형, 새 아틀라스 사각형, 새 카드 uv 사각형, 옛 카드 있음 여부}.
4. `mc.copy`: 캡처 아틀라스 → 기하 아틀라스 4장(깊이 인코딩 포함).
5. `mc.upload`: 카드·페이지·페이지 표·인스턴스 표를 이번 프레임 것으로.
6. 그 뒤 S2의 조명 갱신(새로 캡처된 페이지 먼저)과 소비자의 읽기.

C++:
```cpp
#include "unx/refl/MeshCards.h"
const refl::MeshCardsFrame& mc = refl::meshCards(fc);   // 꺼져 있으면 mc.valid == false
// mc.instanceMap, mc.meshCards, mc.cards, mc.cardPages, mc.pageTable (BufferRef)
// mc.depth, mc.albedo, mc.normal, mc.emissive (TextureRef, 4096^2)
// mc.pageLastUsed, mc.pageHiResLastUsed, mc.feedback (BufferRef; 읽기 패스가 UAV로 잡는다)
// mc.captured (std::span<const MeshCardCapture>), mc.capturedBuffer, mc.cardPageCapacity, mc.atlasSize, mc.frameIndex
```
카드 페이지 번호는 페이지가 살아 있는 동안 안 바뀐다. S2의 페이지별 조명 상태(마지막 직접/간접 갱신 프레임 등)는 S2 자기 버퍼에 카드 페이지 번호로 둔다(`mc.cardPageCapacity` 칸). 새로 캡처된 페이지는 `mc.captured`로 안다.

## 7. 언리얼과 다른 점 (목록)

1. 기하 아틀라스 무압축(1단계).
2. 카드 공유(같은 메시의 인스턴스가 캡처를 나눠 씀: AllowCardSharing 1)는 아직 없음. 인스턴스마다 따로 캡처.
3. 같은 그룹 병합(RayTracingGroupId, 인스턴스 병합)은 없음(원본도 인스턴스 병합은 기본 0).
4. 높이 필드(지형) 카드, 원거리장 카드(FarField, 기본 0)는 없음. 지형 인스턴스는 일반 메시 카드로 처리.
5. 캡처 래스터는 V의 클러스터 경로가 아니라 메시의 원본 삼각형을 그리는 전용 패스(카드 뷰는 한 인스턴스만 그리고 수가 많아 뷰당 컬링 단계가 맞지 않음). 재질 평가는 M의 텍스처 읽기 함수를 그대로 쓴다. 층·데칼·지형 층 등 M의 추가 재질 기능 중 캡처에 안 들어가는 것은 구현하면서 여기 적는다.
6. 방출 저장에 고정 배율 1/16(언리얼은 사전 노출을 곱한다).
7. 1 텍셀 팽창(DilationMode 0이 기본: 끔)은 원본 기본대로 없음.
8. 생성기의 길이(복셀 10 cm 등)는 월드 미터 기준이다: 메시가 다른 단위로 만들어져 인스턴스 배율로 맞춰지는 경우(로비의 화분·탁자: 메시 공간 3 cm) 그 배율을 생성기에 넘긴다(`metresPerUnit`). 언리얼은 에셋 단위 = 월드 단위로 가정한다.
9. 열 광선은 BVH가 아니라 열마다 걸치는 삼각형 목록으로 계산한다(평행 광선이라 결과는 같은 정의, 삼각형 경계에 정확히 걸친 광선에서만 부동소수 차이). 반구 광선 방향은 원본의 난수열이 아니라 고정 해시의 층화 표본(개수 25, 분포 같음).

## 8. 단계

- ②: 생성기(`Native/Scene/src/MeshCards.cpp`, `unx/scene/MeshCards.h`, CPU) + 테스트 `unx_test_scene_meshcards`(인자로 .unxscene을 주면 메시별 보고). 완료. 생성 결과는 메시 내용 해시로 디스크에 캐시한다(③a에서: 로비 1.9초, 기차 라운지 98초 — 소나무 200그루가 메시당 반구 광선 370만 개 — 를 처음 한 번만 치른다).
- ③a: 등록·상주 단계 할당·캡처·복사·업로드 + `--capture-cards`(아틀라스 층 덤프). ③b: 피드백·고해상도 페이지·퇴출·재캡처·resample 목록.
- ④: S2 조명이 붙으면 로비 "데운 뒤 컷 f60·f61·f63".

## 9. S2 쪽: 조명 아틀라스·갱신·읽기 (2026-10-02, S2)

읽은 원본(ue6-main): `LumenSceneLighting.usf`(갱신 선택·합성·resample), `LumenSceneDirectLighting.usf`, `LumenSceneDirectLightingCulling.usf`, `LumenSceneDirectLightingHardwareRayTracing.usf`, `LumenCardTileShadowDownsampleFactor.ush`, `Radiosity/LumenRadiosity.ush`·`.usf`, `LumenRadiosity.cpp`, `LumenSceneLighting.cpp`. 값은 원본 기본값.

### 9.1 S2가 갖는 자료

| 이름 | 형식·크기 | 내용 |
|---|---|---|
| `cardDirect` | R11G11B10_FLOAT 4096² | 텍셀의 직접광 조도(국소광 + 태양), lux × `SC_STORE_SCALE`(1/64) |
| `cardIndirect` | R11G11B10_FLOAT 4096² | radiosity 조도(RadiosityAtlas, 다운샘플 1) |
| `cardFinal` | R11G11B10_FLOAT 4096² | (직접 + 간접) × 알베도 / π + 방출, nits × 1/16. hit이 읽는 값 |
| `cardRadiosityFrames` | R8_UNORM 512² | 8텍셀 타일당 누적 프레임 수 / 255 (최대 4) |
| `cardShadowUniform` | Buffer<uint> 512² × 8 | 타일 × 광원(번호 & 255) 비트: 지난 갱신에서 그 광원에 대한 가시성이 타일 전체에서 같았다(다음 갱신은 2×2에 광선 1개) |
| `cardPageLight` | Buffer 16 B × `cardPageCapacity` | 페이지별: 마지막 직접광 갱신 프레임, 마지막 간접광 갱신 프레임, 직접·간접 시간 번호(지터용) |
| 프레임 임시 | | 갱신 페이지 목록 2개, 카드 타일 목록, 광원 타일(타일당 ≤ 8), 그림자 추적 목록·마스크, radiosity 추적 아틀라스(프로브 × 4×4), 필터된 것, 프로브 SH 3장 |

메모리: 조명 아틀라스 3장 × 4 B × 4096² = 201 MB + 작은 것들. A의 기하 201 MB와 합쳐 약 410 MB(언리얼은 압축으로 더 작다 — 차이 목록에 추가).

### 9.2 프레임 순서 (A의 6번 뒤)

1. **resample**(A의 3번 갈고리): 재할당된 페이지의 `cardDirect`·`cardIndirect`·누적 프레임·균일 가시 비트를 옛 사각형에서 새 사각형으로(쌍선형), `cardFinal`은 새 알베도·방출로 다시 합성. 옛 카드가 없으면 0.
2. **갱신 선택**(페이지당 스레드): 우선순위 통 = 127 − clamp(log2(4 × 마지막 갱신 뒤 프레임 수 × 속도)), 한 번도 안 갱신된 페이지는 2048프레임으로 친다. 속도 = 1 / (1 + 거리 / 첫 클립맵 반크기) × (시야 절두체 근처면 2) 또는 최근 고해상도로 쓰인 페이지는 4(피드백). 히스토그램 → 예산까지 통을 채움 → 목록. 예산(8텍셀 타일 수): 직접 = 아틀라스 타일 / 32(`DirectLighting.UpdateFactor`), 간접 = / 64(`Radiosity.UpdateFactor`). **시야와 무관하게 범위 안 전부가 돈다**(절두체는 속도만 2배).
3. **직접광**: 목록의 타일마다 가장 센 8광원(가중 = 휘도 × 감쇠 × 면광원 적분, 타일 중심·타일 깊이 범위로 컬링) → (타일, 광원)마다 그림자 추적 목록: 균일 비트가 있으면 2×2당 1텍셀(4배 적음), 없으면 텍셀마다 → 광선 = 텍셀에서 광원 중심으로 1개(A의 `direct_pairs` 구조: 쌍마다 스레드 하나, 262,144 띠) → 텍셀 조도 = Σ 해석적 조도 × 가시. 태양도 광원 하나로 같은 목록에. 균일 비트 갱신.
4. **radiosity**: 목록의 페이지를 4텍셀 타일로, 타일당 프로브 1개(텍셀 위치는 시간 번호로 지터), 프로브당 4×4 균등 반구 광선(지터) → hit의 `cardFinal`을 9.3으로 읽음(없으면 0, miss는 하늘), 세기 상한 40(노출 단위) → 이웃 프로브 4개와 평면 가중 필터(가운데 가중 2) → SH2 → 텍셀마다 프로브 4개 쌍선형(1텍셀 확장) × 평면 가중 → 시간 누적 alpha = 1 / (1 + 프레임 수), 최대 4. 광선은 원점 5 cm 안 뒷면 hit을 건너뛴다(`AvoidSelfIntersections`).
5. **합성**: 갱신된 타일의 `cardFinal`.
6. 소비자(반사·GI·radiance cache·물/유리 hit)의 읽기와 피드백.

### 9.3 hit 읽기 (S2 본체, `Passes/SurfaceCache/CardLighting.hlsli`)

```hlsl
// A의 4절 1~7. 유효하지 않으면 valid = false(원본과 같이 0).
struct ScSample { bool valid; float3 direct, sun, indirect; float3 albedo, emission; };  // 지금과 같은 모양
ScSample scReadCards(McFrame mc, uint sceneInstance, float3 position, float3 normal, float sampleRadius, uint2 ditherCoord);
float3 scFinalLighting(ScSample s);
```
- `direct`에 태양이 들어 있다(`sun` = 0). `albedo`·`emission`은 카드의 것.
- **R·반사 호출부가 바꿀 것 한 줄**: `scRead(surfaceCache, layout, s.position, face)` → `scReadCards(mc, s.sceneInstance, s.position, face, footprint, coord)`. 위치만으로는 인스턴스를 알 수 없어서 서명이 달라진다. `scMark`는 카드 경로에서 필요 없다(피드백이 대신한다) — `surface_cache.mesh_cards`가 켜지면 호출부에서 건너뛴다. 옛 해시 셀 경로(`scRead`/`scMark`)는 스위치가 꺼졌을 때만 남는다.
- hit 셰이딩의 받는 값은 지금과 같다: `L.irradiance = cell.direct + cell.indirect`, 다만 태양이 `direct`에 들어 있으므로 hit에서 태양을 따로 더하던 코드는 카드가 유효할 때 건너뛴다(이중 계산 방지).

### 9.4 A에게 필요한 것 (3절에 더해)

- `McCardPage`에 그 페이지의 마지막 캡처 프레임(resample·새 페이지 우선에 씀)이 있거나 `mc.captured`로 충분한지 — `mc.captured`면 된다(목록에 든 페이지는 `cardPageLight`를 0으로 돌려 "한 번도 안 갱신됨"으로 만든다).
- 첫 클립맵 반크기에 해당하는 값(갱신 속도의 거리 척도): 없으면 S2가 `surface_cache.toml`에 `mesh_cards.update_distance_m = 25`(언리얼 첫 클립맵 2500 cm)로 둔다.
- 카드의 월드 상자(광원 컬링): `McCard.cardToWorld` + `extent`로 S2가 계산한다. 추가 요청 없음.
- 캡처에서 표면이 없는 텍셀은 깊이 1.0: S2는 그 텍셀을 조명·radiosity·읽기에서 뺀다.

### 9.5 단계 (S2)

- S-a: `CardLighting.hlsli`(읽기 본체) + 합성 패스. A의 ③a가 올라오면 방출만 있는 `cardFinal`로 읽기를 확인.
- S-b: 갱신 선택 + 직접광(쌍 구조, 균일 비트). S-c: radiosity. S-d: resample·피드백 쓰기.
- 판정: A의 ④와 같이 — 데운 뒤 컷 f60·f61·f63과 회전 중 그림(로비·라운지·기차).

### 9.6 A의 세 질문에 대한 답 (S2)

1. 페이지별 조명 상태는 **S2 자기 버퍼**(`cardPageLight`, 카드 페이지 번호로)로 둔다. 좋다. A의 `McCardPage`는 그대로.
2. 방출 1/16 고정: 좋다. 조명 아틀라스는 `cardDirect`·`cardIndirect` = lux × 1/64, `cardFinal` = nits × 1/16(방출과 같은 배율이라 합성에서 그대로 더한다). 사전 노출은 쓰지 않는다 — 노출이 바뀔 때 아틀라스를 다시 맞출 필요가 없다. radiosity 광선 세기 상한(40)만 읽을 때 현재 노출로 건다.
3. resample은 **③b로 미룬다**. ③a에서는 새 페이지가 조명 없이 시작하고 `mc.captured`에 든 페이지를 S2가 "한 번도 안 갱신됨"(우선순위 최상)으로 올린다. 그동안 그 페이지를 읽는 hit은 유효하지 않음(0)이다 — 원본과 같다.

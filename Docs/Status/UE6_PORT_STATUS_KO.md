# 언리얼 6 렌더러 자체 구현 — 진행 상태

사용자 지시(2026-10-02): 렌더러 전체를 언리얼 6(ue6-main)의 구조로 자체 구현한다. Lumen, 가상화 지오메트리, 그림자, 그 밖의 전부. 목표는 "화면만 봐도 예쁜" 품질과 4K 6.06 ms(1440p·1080p는 그보다 훨씬 빠르게). 금지는 하나: "이건가? 아니네 → 검증 → 이건가? 아니네" 반복. 검증은 최소, 코드 작업 위주.

- 브랜치 `lumen-ue6`, 작업 트리 `C:\Users\USER\UnravelNext-ue6`. `redesign-v2` + `redesign-v2-refl` + `redesign-v2-fix` 병합에서 시작.
- 빌드: `Tools\CI\Build.ps1 -Track all -Jobs 16`(PowerShell에서). GPU 실행: 2026-10-02에 두 번(6절).
- 언리얼 소스(읽기 전용): `C:\Users\USER\RendererResearch\UnrealEngine-ue6-main`, `...\UnrealEngine-5.8.2`.
- ue6 기본 경로 확인: `r.LumenRef.Supported = false` → 기본은 Lumen(카드 표면 캐시 + 화면 프로브 수집 + radiance cache + 반사). `r.Lumen.HardwareRayTracing.LightingMode = 0` → hit 조명은 표면 캐시.

표기: ✔ 코드 작성·빌드 통과, ◐ 일부, ☐ 미착수. GPU에서 돈 것과 안 돈 것은 6절에 따로 적는다.

## 1. Lumen

| 항목 | 상태 | 위치 |
|---|---|---|
| 세 브랜치 병합, 전체 빌드 | ✔ | 50af9e4 |
| 메시 카드 생성: 프레임 밖 작업 스레드 + 디스크 캐시(메시 내용 해시), 배율 1/4옥타브 묶음 | ✔ | `Passes/SurfaceCache/MeshCardCache.cpp` |
| 카드 씬: 인스턴스 추가·이동·숨김·제거·재질 변경(재캡처), 상주 해상도, 페이지 표 고정 크기 | ✔ | `MeshCardScene.cpp`, `SurfaceCacheCards.cpp` |
| 카드 캡처: 페이지마다 그 인스턴스의 원본 삼각형을 메시 셰이더로 정사영 → 알베도·법선·방출·깊이 | ✔ | `CardCapture.ms/.ps.hlsl`, 재질은 `CardCaptureMaterial.hlsli` |
| 재할당 카드의 조명 이어받기(resample), 캡처 → 아틀라스 복사, 새 페이지 조명 상태 | ✔ | `CardResample.hlsl`, `CardCopy.hlsl` |
| 오래된 페이지 재캡처(예산의 1/8, 재질 변경은 먼저) | ✔ | `MeshCardScene::update` |
| 카드 조명: 갱신 선택, 직접광(타일당 8광원 + 태양, 스레드당 광선 1개), radiosity, FinalLighting | ✔ | `CardLighting.cpp`, `Card*.hlsl` |
| 프레임 순서: 카드 갱신이 GI 앞(`tracks::surfaceCache(fc, main)`), `FrameResources::cards` | ✔ | `ReflectionTrack.cpp`, `FrameRenderer.cpp` |
| 화면 프로브 광선·radiance cache 광선의 hit이 카드를 읽음(해시 셀 읽기 제거) | ✔ | `LgTrace.hlsl`, `LumenRadianceCacheTrace.hlsl` |
| 반사 hit이 카드를 읽음(프레임의 카드 사용) | ✔ | `ReflectionSystem.cpp`, `ReflectionShade.hlsli` |
| 레벨 로드 때 여러 갱신을 한 프레임에(로드 직후 몇 프레임 안에 캐시가 참) | ✔ | `mesh_cards_load_rounds` |
| 표면 캐시 피드백(고해상도 페이지 요청·퇴출) | ✔ (실행 안 함) | 1.2 표 1번 |
| 반사 경로를 Lumen 반사 단독 구현으로(스레드당 광선 1개, 옛 K/G/M 경로와 분리) | ✔ | `reflection.lumen_only`: `ReflectionLumenTrace.hlsl`, `ReflectionLumenHit.hlsli`, 굴절 서비스 `RefractionLumenTrace.hlsl` |
| 최종 수집만 도는 GI(옛 월드 캐시·그 화면 프로브 없음, 캐시 메모리도 잡지 않음) | ✔ | `gi.lumen_only`: `GiSystem.cpp`, `LumenGather.cpp` |
| 최종 수집의 컷 프레임·이동 중 처리 대조 반영 | ✔ | 1.1 표 |
| Lumen translucency volume(시야 정렬 격자 32 px × 26단, 칸당 3×3 광선 → radiance cache, 분리형 필터, SH + 시간 누적) | ✔ | `Passes/GI/LumenTranslucencyVolume.*` |
| 옛 월드 GI 캐시를 읽던 곳을 translucency volume으로: 입자, 볼륨 입자(매질), 물 표면, 유리(반투명 합성), coverage 조각(가는 형상·가장자리), 평면 반사 뷰, 잎 뒷면 | ✔ | `Passes/GI/GiSource.hlsli`, `Frame.h`의 `GiSource` |
| M 합성을 언리얼의 DiffuseIndirectComposite 구조로: 확산 × 짧은 거리 AO(다중 반사), rough specular × 스페큘러 가림, 그 위에 반사를 몫만큼; 클리어코트는 윗층 = 코트 거칠기로 추적한 반사, 아랫층 = rough specular | ✔ | `ShadeOpaque.hlsl`(part 2), `ReflectionInternal.hlsli`의 `reflTopLayerRoughness` |
| 설정 묶음과 기본값(`Config/quality`): 아래 스위치가 전부 기본 켬 | ✔ | `surface_cache.enabled`·`mesh_cards`, `gi.lumen`·`lumen_only`, `reflection.lumen`·`lumen_only`, `lumen.radiance_cache`·`short_range_ao`·`translucency_volume`, `output.screen_trace_source = 0` |

옛 경로(월드 GI 해시 캐시, 그 화면 프로브, 반사의 K/G/M·rays buffer·inline·shade·combine 패스, 해시 셀 표면 캐시)는 스위치를 끄면 그대로 돈다. GPU 실행으로 새 경로를 확인한 뒤 코드에서 지운다.

### 1.2 2026-10-03 추가분 (브랜치 `w/cache`) — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 돌린 것은 없다(시험 실행 파일·캡처·게이트·furnace 포함). 확인은 `Tools\CI\Build.ps1 -Track all`의 `build ok`뿐이다.

| # | 항목 | 내용 | 스위치(기본) | 위치 |
|---|---|---|---|---|
| 1 | 표면 캐시 피드백(해상도) | 카드의 높은 단계를 읽는 hit(반사·굴절 광선, 카드 radiosity 광선)이 "어느 카드의 어느 단계·어느 페이지를 원했는지"를 GPU 해시 표(4096칸, 원소 = 카드 20비트·단계·페이지 좌표, 값 = hit 수)에 넣는다. 원한 단계 = log2(카드 반크기 / hit에서의 광선 원뿔 반지름) − 0.5. 화면 16×16 타일마다 프레임당 한 hit만 보고한다(자리는 프레임마다 옮겨 간다). 다음 프레임 첫머리에 표를 readback으로 복사하고 비우며, framesInFlight 프레임 뒤에 CPU가 읽어 hit 수 16 초과인 페이지를 요청으로 만든다. 요청은 상주 단계 요청보다 25~50 m 뒤 순위다. 페이지는 잠기지 않은 단계에 한 장씩 매핑·캡처하고(조명은 기존 단계에서 resample), 256프레임 동안 아무 hit도 원하지 않으면 나간다. 자리가 없으면 가장 오래 안 쓴 것부터 내보낸다. 높은 단계의 매핑 안 된 페이지는 페이지 표가 아래 단계의 매핑된 페이지로 보낸다. 최근 2회 갱신 안에 읽힌 페이지는 조명 갱신 속도 4(언리얼의 CardPageHighResLastUsedBuffer). | `surface_cache.feedback`(켬), `feedback_tile_size` 16, `feedback_res_level_bias` −0.5, `feedback_min_page_hits` 16, `feedback_keep_unused_frames` 256, `reflection.lumen_hi_res_surface`(켬), `surface_cache.feedback_gather`(끔) | `MeshCardScene.cpp`(setFeedback, evictOldest), `SurfaceCacheCards.cpp`(readback·clear), `CardLighting.hlsli`(clFeedback, clReadCardsAt), `CardLayout.hlsli`(mcCardSample hiRes, 카드 프레임 28워드), `CardFeedbackClear.hlsl`, `CardSelect.hlsl` |
| 2 | 클러스터 LOD로 카드 캡처 | V의 래스터 서비스(`FrameServices::rasterizeDepth`)를 있는 그대로 쓴다. 캡처 페이지 하나가 요청의 뷰 하나다(정사영, `lodPixelsPerMetre` = 페이지의 미터당 텍셀, 그 인스턴스만, 8텍셀 타일을 캡처 아틀라스의 제자리로 보내는 tile atlas 모드). 요청 하나에 255뷰, 묶음마다 서비스를 두 번 돈다: `r.card.vdepth`(가장 앞 표면의 깊이), `r.card.vmaterial`(그 깊이와 같은 조각만 재질을 UAV로 기록). 텍셀 법선은 깊이의 화소 간 차이로 만든 삼각형 법선이고, 법선 맵 기울기는 uv 차이로 만든 접선 틀에 얹는다. `Passes/Visibility`는 고치지 않았다. | `surface_cache.mesh_cards_capture_clusters`(**끔**) | `CardCaptureCluster.ps.hlsl`, `SurfaceCacheCards.cpp`(captureView, r.card.vcapture.begin), `CardCopy.hlsl`(뒤집힌 깊이) |
| 3 | 카드 캡처의 재질 층 | 층 있는 재질의 카드 알베도를 법선 방향에서 본 반사율로 캡처한다(언리얼 Substrate 캡처와 같은 규칙: V = N). 클리어코트: 코트 로브 알베도 + 코트를 두 번 지난 아랫층(코트 거칠기가 투과율을 정함), 시트(sheen): 틴트 × 시트 알베도 + 남은 아랫층, 박막: f0를 박막의 수직 입사 값으로. hit이 제 재질로 셰이딩하는 경로(수집·반사)는 원래 층을 계산했고, 카드의 final 조명을 읽는 경로(radiosity, translucency volume)가 이제 같은 양을 튕긴다. | 없음(항상) | `CardCaptureMaterial.hlsli` `ccLayered` |
| 4 | radiosity 다시 쏘기 | 첫 hit이 5 cm 안의 한 면 재질 뒷면이면 5 cm부터, 1 cm 안의 양면 재질이면 그 hit 바로 뒤부터 한 번 더 쏜다(루프 없음). 10 cm보다 가까운 hit은 광선을 막되 빛은 0이다. 스레드당 광선 상한 3 → 4, 디스패치당 스레드 87,381 → 65,536. | `surface_cache.radiosity_avoid_self_intersections`(켬), `radiosity_skip_back_face_m` 0.05, `radiosity_skip_two_sided_m` 0.01, `radiosity_min_trace_distance_m` 0.10 | `CardRadiosityTrace.hlsl`, `CardLighting.cpp` |
| 5 | 카드 없는 hit의 간접광 | 태양·국소광 표본에 더해 간접 irradiance를 준다. ① translucency volume: hit이 주 뷰 격자 안이고 그 칸이 추적된 빛을 가졌을 때(ambient의 알파 = 칸이 가진 추적 빛의 몫, 새로 넣음). ② 아니면 radiance cache의 irradiance 프로브: 추적된 프로브마다 6×6(+테두리) irradiance 맵을 만들고, hit 둘레 8개 프로브 중 hit을 보는 것(기존 깊이 검사)만 쓴다. 그런 hit은 16개 중 하나가 위치를 목록에 적고, 다음 프레임 캐시 표시 단계가 그 자리에 프로브를 요청한다(clipmap 2단계 거칠게). ③ 둘 다 없으면 전과 같이 0. 소스는 카드 프레임 워드 22~27로 전달한다: 표면 캐시·radiance cache·volume 자신의 광선은 이전 프레임 volume, 수집·반사는 이번 프레임 것. | `lumen.hit_indirect`(켬), `hit_indirect_radiance_cache`(켬), `hit_indirect_mark_clipmap_bias` 2 | `Passes/GI/LumenHitIndirect.hlsli`, `LumenRadianceCacheIrradiance.hlsl`, `LumenRadianceCache.hlsli`(lrcIrradiance, lrcHitMark), `LumenTranslucencyVolume.hlsli`(ltvSampleLit), `CardFrameSources.hlsl`, `LumenGather.cpp` |
| 6 | 대조에서 구현한 것 | 잎 화소(Foliage·Subsurface 재질 = 언리얼의 backface diffuse 화소): 수집 이력의 거리 임계 0.03(일반 0.01), 전용 반사 광선은 거칠기 0.2 미만에서만. 수집 이력의 법선 거부(45°)는 스위치만 넣고 끔(언리얼 기본 0). | `lumen.gather_temporal_distance_threshold_foliage` 0.03, `lumen.gather_temporal_reject_normal`(끔), `reflection.lumen_max_roughness_to_trace_foliage` 0.2 | `LgTemporal.hlsl`, `ReflectionClassify.hlsl`, `ReflectionReuseFilter.hlsl`, `ReflectionInternal.hlsli` |
| 7 | 표면 캐시 직접광의 광원 함수 | 점·스폿 광원의 함수(IES, 쿠키, 고보, 세기·색 키, 깜빡임)를 카드 텍셀/셀 방향으로 곱한다(footprint = 텍셀 크기 / 광원까지 거리). 표는 광선 씬의 광원 데이터 머리(바이트 80)가 가리키는 그 프레임의 표이고, 패스는 원래 `declareTraversal`로 선언하고 있었다. 시간에 따라 변하는 함수(회전, 키 2개 이상, 깜빡임)의 광원이 비추는 곳: 카드는 페이지 조명 상태에 표시해 직접광 갱신 속도 4, 셀은 피드백 목록에 넣어 먼저 다시 비춘다. 표면 캐시에는 광원별 개정 번호가 없다 — 저장값은 갱신 예산이 돌아올 때 새로 계산된다. | 없음(항상) | `SurfaceCacheLightFunction.hlsli`, `CardDirectStore.hlsl`, `CardDirectCull.hlsl`, `CardSelect.hlsl`, `SurfaceCacheLight.hlsl`, `SurfaceCacheLightPairsSelect.hlsl` |

2번을 켜려면 V에 필요한 것(인터페이스 요청, V 코드는 손대지 않음):

- `DepthRasterPixel`에 보간된 꼭짓점 법선(과 접선). 지금은 화소 커널이 uv·재질·인스턴스만 받는다. 없으면 매끈하게 셰이딩되는 저폴리곤 메시의 카드 법선에 면이 드러난다.
- 요청의 화소 커널이 그래프 리소스의 뷰 번호를 받을 길. 지금은 `pixelConstants`가 기록 시점 값 그대로 전달돼서, 캡처 쪽이 자체 업로드 버퍼(고정 SRV)에 패스 실행 때 번호를 적어 넘긴다.
- (성능) 깊이와 재질을 한 번에: 서비스의 화소 커널이 렌더 타깃에 쓸 수 있으면 컬 체인이 묶음당 2회 → 1회가 된다.

**6번 대조 — 옮기지 않은 것** (`Engine/Shaders/Private/Lumen/*`, `LumenScreenProbeGather.cpp`, `LumenReflections.cpp`, `LumenReflectionTracing.cpp`의 cvar 기준):

| 언리얼 | 상태 | 이유 |
|---|---|---|
| 수집: InterpolationDepthWeight.Foliage, TwoSidedFoliageBackfaceDiffuse, ScreenTraces SkipFoliageHits, TemporalFilterProbes, 중요도 표본 2단계, ShortRangeAO | 이미 있음 | |
| 수집: MaxRoughnessToEvaluateRoughSpecularForFoliage 0.8 | 따로 두지 않음 | 일반 화소 값(0.8)과 같다 |
| 수집: ShortRangeGI | 안 옮김 | 언리얼에서 실험 기능, 기본 끔 |
| 수집: IntegrateDownsampleFactor, GatherNumMips, SpatialFilterHalfKernelSize, FastUpdateModeUseNeighborhoodClamp, RadianceCache.SkyVisibility, ExtraAmbientOcclusion | 안 옮김 | 언리얼 기본값에서 꺼져 있거나 1 |
| 수집: ScreenTraces SkipHairHits·SkipUnlitHits, MinimumOccupancy, ThicknessScaleWhenNoFallback | 안 옮김 | 기본 false이거나 거리장 대체 경로용 |
| 수집·반사: HairStrands.ScreenTrace / VoxelTrace | 안 옮김 | 머리카락 트랙의 복셀·깊이 인터페이스가 필요 |
| 반사: ScreenSpaceReconstruction, BilateralFilter(표본 수·반경·깊이 가중·disocclusion 프레임), Temporal, DenoiserTonemapRange, GGXSamplingBias, RoughnessFadeLength, SampleSceneColorAtHit, DownsampleFactor, 수집의 rough specular 재사용 | 이미 있음 | |
| 반사: HiResSurface, SurfaceCacheFeedback | 1번에서 구현 | |
| 반사: DistantScreenTraces | 안 옮김 | 언리얼에서는 광선 씬이 컬링 반경에서 끝나는 곳부터 화면을 따라가는 장치이고, far field가 켜지면 돌리지 않는다. 여기는 컬링 반경으로 끊기는 근거리 씬이 따로 없다: 반사 광선이 광선 씬 전체를 `gi.ray_length_m`까지 추적한다 |
| 반사: RadianceCache(거친 반사 광선을 줄이고 캐시에서 읽기) | 안 옮김 | 언리얼 기본 0 |
| 반사: DownsampleCheckerboard, MaxRoughnessToTraceClamp, SmoothBias, SpecularScale, Contrast | 안 옮김 | 기본값에서 동작 없음 |
| 반사: 반투명 앞면 층 반사(front layer), HitLighting 모드·MaxBounces | 안 옮김 | 반투명 합성(M)과 재질 hit 셰이더가 필요. 기본은 표면 캐시 조명 |
| 반사: HierarchicalScreenTraces.IncludeTranslucencyDepth, MinimumOccupancy | 안 옮김 | 기본값에서 동작 같음 |

### 1.1 최종 수집·반사 대조 결과 (2026-10-02, 코드 대조)

구조와 상수는 언리얼과 같다(프로브 배치 16 px·적응 0.5, 8×8 추적, 중요도 표본, 세기 상한 10, 프로브 필터 3패스, 화소 시간 필터 10프레임, 반사 재구성 5표본·시간 12프레임·양방향 필터). **언리얼에 컷 프레임 전용 장치는 없다.** 컷 직후가 깨끗한 까닭은 필터에 들어가는 값이 조용하기 때문이다: 시야와 무관하게 남아 있는 표면 캐시·radiance cache, 그리고 청색 잡음. 고친 것(전부 코드 작성·빌드 통과, GPU 미실행):

| # | 내용 | 상태 |
|---|---|---|
| 1 | hit 조명을 결정적으로: 카드 읽기. 카드 없는 hit은 태양 원반 중심으로 그림자 광선 1개, 국소광 표본 없음 | ✔ |
| 2 | radiance cache 켬, 프로브 광선은 캐시를 8프로브 가중으로 읽음, hit 거리 = 캐시의 거리 | ✔ |
| 3 | 화면 추적을 캐시 범위 거리에서 끊음 | ✔ |
| 4 | 청색 잡음 타일(64×64 × 4채널, 프레임마다 황금비 회전): 광선 텍셀 중심, 화소 지터·프로브 고르기, 반사 광선 | ✔ |
| 5 | 거친 스페큘러 4표본을 Hammersley로, 0.9 편향을 극각 좌표에 | ✔ |
| 6 | 프로브 disocclusion 판정에 이력 유효성(같은 면) 검사 | ✔ |
| 7 | 보간 최소 가중 0.03 → 0.01 | ✔ |
| 8 | 화면 추적: hit의 이전 프레임 깊이 검사(보관한 장면 색의 알파에 그 프레임 깊이), 두께 단계 0 + 잎 hit 건너뜀, 입력은 업스케일 전 장면 색 | ✔ |
| 9 | 짧은 거리 AO를 시간 필터 뒤 M 합성에서(다중 반사 + 스페큘러 가림은 rough specular에만) | ✔ |
| 10 | 스냅 프레임의 노출 기준(수집이 잰 값)을 반사 필터의 상한·톤맵 평균에도 | ✔ |

## 2. 가상화 지오메트리 / 그림자 / TSR·후처리 / 그 밖 — 언리얼 대비 차이 (2026-10-02 조사)

### 2.1 지오메트리 (V) — 구조는 Nanite와 같다(클러스터 DAG, 그룹 오차 LOD, 2단계 HZB 컬링, vis buffer)

| Nanite | 우리 | 결과 |
|---|---|---|
| 소프트웨어 래스터(변 32 px 미만 클러스터) | 없음(메시 셰이더만) | 작은 삼각형이 주 뷰와 그림자 뷰에서 하드웨어 셋업 비용을 냄 |
| DAG 안의 복셀 클러스터(잎·가는 형상도 계속 단순화) | 없음(가는 그룹은 말단, 밴드 C 런타임 없음) | forest_thin: 원본 삼각형 1.6억, visible 758만 요청(상한 100만) → 8.98 ms |
| 래스터 빈·앞→뒤 깊이 버킷 | 고정 리스트 8개 | coverage 층이 가려진 조각을 저장(물가 61.8 %) |
| 클러스터 압축(양자화·스트립) | 무압축 | 메모리·대역폭 3~4배 [추정] |
| 페이지 스트리밍 + GPU 피드백 | 스트리머만 있고 연결 안 됨 | 전부 상주 |
| 그림자 뷰 HZB 가림 | 움직이는 캐스터만(정적 사본의 페이지 HZB; 2.2) — 코드 작성·빌드 통과, 실행 안 함 | 가려진 정적 캐스터는 여전히 래스터 |
| 레벨당 패스 1개(ping-pong) / persistent cull | 순회 전체가 디스패치 하나(노드 작업 큐, `visibility.traversal_work_queue`) — 코드 작성·빌드 통과, 실행 안 함 (`UE6_WORKPLAN_KO.md` 8.1 (1)); 끄면 레벨당 prepare + nodes 2패스 | 실행 전: 시간 미측정 |
| 용량 visible 4M | 1M | 숲에서 넘침 |

실측 [문서 인용]: city_block 4K V 합계 0.238 ms. 현재 게임(실내·가벼운 그래픽)에서는 병목이 아니다.

### 2.2 그림자 (S)

| 언리얼 | 우리 | 결과 |
|---|---|---|
| 화면 공간 접촉 그림자(4표본, 0.015 × 깊이) | 있음(2026-10-03, 코드 작성·빌드 통과, 실행 안 함): 그림자 맵이 밝다고 한 픽셀이 태양 쪽으로 깊이 버퍼를 8표본 걷는다(`shadow.vsm.screen_ray_length` 0.015 × 깊이, `screen_ray_steps`; `ShadowReceiver.hlsli` `shadowSunContact`). 지터는 픽셀 고정(프레임 항 없음) | 접촉부 그림자 |
| MegaLights 화면 추적·청색 잡음·LightPowerDelta | 없음 | 접촉 누설, 광원 변화 반응 느림 |
| 항상 상주하는 굵은 페이지 + 페이지 팽창 | 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.coarse_pages`, `coarse_level_first/last`, `page_dilation`; `VsmMarkCoarse.hlsl`, `VsmMark.hlsl`; `UE6_WORKPLAN_KO.md` 8.2 (8)) | 제 레벨에 페이지가 없는 조회가 굵은 레벨에서 그림자를 받는다 |
| 정적/동적 페이지 분리 + 병합 | 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.static_separate`; 정적 사본 아틀라스 + `s.vsm.merge`, V의 `RasterView::instanceSet`; `UE6_WORKPLAN_KO.md` 8.2 (7)) | 움직이는 캐스터 밑의 페이지는 정적 사본을 두고 움직이는 캐스터만 다시 그린다 |
| 페이지별 HZB, HZB로 거른 무효화 | 무효화: 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.cache_hzb_filter`; 페이지의 블록 계층이 HZB, `VsmCache.hlsl` MODE 2; `UE6_WORKPLAN_KO.md` 8.2 (10)). 그림자 뷰의 HZB 가림: 움직이는 캐스터에 대해 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.static_hzb_cull`; 정적 사본의 페이지별 HZB `VsmStaticHzb.hlsl`, V의 `tilesOcclude`; 8.2 (9)). 정적 캐스터끼리의 가림은 없음 | 저장된 표면 아래에서 움직이는 캐스터는 페이지를 다시 그리게 하지 않고, 정적 표면 아래의 움직이는 캐스터는 래스터하지 않는다 |
| SMRT 확률 광선 7×8 + TSR | 결정적 차단체 탐색 + 16탭 | 우리 쪽이 첫 프레임 잡음 없음(유지) |
| 16광원 one-pass 투영 | 슬롯 3 + 넘침 목록, MegaLights가 대체 | MegaLights 기본화로 해결 |

### 2.3 TSR·후처리·그 밖 (M)

| 영역 | 차이 | 보이는 결과 |
|---|---|---|
| 시간 업스케일 | 셰이딩 거부·disocclusion 검사·깜빡임 억제·컷 프레임 공간 AA 없음 | 움직임 뒤 잔상, 조명 변화 지연, 컷 직후 흐렸다가 선명해짐 |
| 톤 파이프라인 | 곡선은 같음. blue correction·gamut 확장·ACES glow/red modifier·장면 기준 그레이딩 LUT 없음 | 채도 높은 파랑·빨강의 색상 이동 |
| 국소 노출 | 없음 | 역광에서 창이 날아가거나 실내가 뭉개짐 |
| 블룸·비네트·그레인 | 코드는 있고 기본 끔. 렌즈 플레어·샤픈 없음 (2026-10-03: 렌즈 플레어·샤픈·색수차 코드 있음, 2.3.1) | 평평한 인상 |
| 높이 안개·볼류메트릭 안개·국소 안개 | 없음(프록셀에 대기·국소광 공기만) | 거리감 없음 |
| 짧은 거리 AO·스페큘러 가림 | 이식돼 있고 꺼져 있음 | 물체가 뜸 |
| 서브서피스(Burley) | 없음(Standard로 셰이딩) | 피부·왁스가 플라스틱 |
| 반투명 속도 벡터 | 없음 (2026-10-03: 레이어 벡터·마스크 코드 있음, 2.3.1) | 물·유리·입자 잔상 |
| 구름 | 1/4 해상도, 시간 재구성 없음, 3.27 ms | 사실상 못 씀 |
| 모션 블러 | 업스케일 전 내부 해상도, 32 px 제한 (2026-10-03: 업스케일 뒤 출력 해상도 경로 코드 있음, 2.3.1) | 짧고 거친 줄무늬 |
| 색 보정 | 장면 기준 그레이딩 없음(화이트 밸런스와 곡선 뒤 .cube LUT만) (2026-10-03: 결합 LUT 코드 있음, 2.3.1) | 게임이 룩을 못 바꿈 |

### 2.3.1 2026-10-02에 채운 것 (코드 작성·빌드 통과, 6절의 실행에서 돌았다)

| 항목 | 내용 | 위치 |
|---|---|---|
| 시간 업스케일 = TSR 구조 | 벡터 팽창(3×3 최근접 깊이) + 최근접 가림체 scatter로 시차 disocclusion, 저해상도 guide 이력과 그에 대한 셰이딩 거부(3×3 연산 사슬을 그룹 메모리에서 한 패스로), 거부·이력 없음 화소의 공간 AA(가장자리 8화소 탐색), 유효도 가중 이력 갱신(16표본, 1 px/프레임 이동 시 4, 거부 시 2), 이력 clamp는 거부가 말하는 만큼만 | `Passes/Shading/Tsr*.hlsl`, `Tsr.hlsli`, `Upscale.cpp` (`output.upscale_tsr`, 기본 켬) |
| 톤 파이프라인 | 필름 곡선 앞뒤로 gamut 확장(1.0), blue correction(0.6), ACES glow, red modifier | `ShadingCommon.hlsli` `shFilm` |
| 블룸·비네트 기본값 | 블룸 0.082(언리얼 기본 세기 0.675 × 6단 틴트 / 6의 몫), 비네트 0.4(모서리 원 기준 cos⁴) | `Config/quality/shading.toml`, `PostFinal.hlsl` |

깜빡임(moire) 휴리스틱도 들어 있다(`TsrFlicker.hlsl`, `output.upscale_tsr_flickering`): 서 있는 화소의 luma가 지터 주기로 뒤집히면(타일 줄눈·격자) 그 진폭 안에서는 이력을 버리지 않는다. 언리얼은 반투명 이전 색을 따라가고, 여기서는 최종 장면 색을 따라간다.
TSR에서 아직 없는 것(2026-10-02 기준): history resurrection, reprojection field(자코비안·경계), thin geometry 검출, 출력보다 큰 이력 해상도. 2026-10-03에 넷 다 코드로 넣었다(아래 표).
반사에는 `reflection.lumen_downsample`(기본 1, 2 = 2×2당 광선 1개 + 이웃 블록 광선으로 resolve)이 있다. 언리얼의 DownsampleFactor와 같은 손잡이로, 첫 실행에서 시간과 그림을 둘 다 재고 정한다.
국소 노출은 넣었다(`shading.post_local_exposure`, 언리얼의 bilateral 방식: `LocalExposure.hlsli`, 대비 0.8 / 0.8). 샤픈·렌즈 플레어는 언리얼에서도 기본 꺼짐이라 뒤로 두었다가 2026-10-03에 스위치 뒤에 넣었다(아래 표, 기본 끔).

#### 2026-10-03에 채운 것 — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 한 번도 돌리지 않았다. 아래는 코드에 적힌 내용이고, 그림과 시간은 재지 않았다. 스위치는 `Config/quality/output.toml`(`upscale_*`)과 `shading.toml`(`motion_blur_*`, `post_*`)에 있다.

| 항목 | 내용 | 위치 · 스위치 | 상태 |
|---|---|---|---|
| reprojection field | 내부 화소마다 벡터의 자코비안(같은 표면의 이웃 벡터, 깊이 가중)과, 양쪽이 다르게 움직이는 가장자리에서는 화소 안의 경계(깊이 가장자리를 양쪽 3화소 따라감)를 적는다. 이력 갱신은 출력 화소가 놓인 쪽의 벡터를 읽고 자코비안으로 그 화소 자리까지 옮긴다. 이력을 확대하는 재투영은 그만큼 이력 가중을 낮춘다. 언리얼의 hole filling(가려졌던 화소의 벡터를 가림체 벡터로 바꿈)은 넣지 않았다. | `TsrDilate.hlsl`, `TsrUpdate.hlsl`, `Tsr.hlsli` · `output.upscale_tsr_reprojection_field`(epic·high 켬, performance 끔), `…_aa_speed` 0.125 | 코드 작성·빌드 통과, 실행 안 함 |
| history resurrection | 이력과 guide를 4칸 고리에 둔다. 두 칸은 프레임을 번갈아 받고 두 칸은 31프레임마다 한 장씩 보관한다(뷰 행렬·노출 포함, 원점 이동 반영). 보관 프레임의 guide를 카메라만으로 재투영해 입력과 같은 방법으로 재고, 직전 이력보다 0.1 이상 잘 맞는 화소는 보관 프레임의 이력을 쓴다. 맞는 화소가 없는 타일은 일찍 끝낸다. 스스로 움직인 물체는 맞지 않으므로 되살리지 않는다. | `TsrResurrect.hlsl`, `TsrDecimate.hlsl`, `TsrReject.hlsl`, `TsrUpdate.hlsl`, `Upscale.cpp` · `output.upscale_tsr_resurrection`(켬. 언리얼 기본값은 0) | 코드 작성·빌드 통과, 실행 안 함 |
| thin geometry | 커버리지 레이어의 얇은 조각이 화소를 덮는 비율을 이력으로 평균하고(언리얼은 재질 표시의 적중 횟수를 평균), 화소 폭의 깊이 선을 찾아, 그 화소들에서 셰이딩 거부의 clamp 상자를 이력 자신의 이웃 범위로 넓힌다. 레이어가 조각을 알려 주는 화소는 5 × 5 조건 없이 완화한다. 언리얼의 밝기 선 검출과 깜빡임 휴리스틱 연동은 넣지 않았다. 거부 커널의 그룹 메모리는 26 KB가 됐다(한계 32 KB). | `TsrThin.hlsl`, `TsrReject.hlsl`, `TsrDecimate.hlsl` · `output.upscale_tsr_thin_geometry`(performance 끔), `…_error_multiplier` 200, `…_max_relaxation` 0.037 | 코드 작성·빌드 통과, 실행 안 함 |
| 출력보다 큰 이력 | 이력을 출력의 100~200 %로 두고 갱신을 그 해상도에서 한 뒤 Mitchell-Netravali 4 × 4로 출력에 내린다. 표본 수 상수는 이력 화소 기준으로 바꿨다. 4K 출력에서 200 %면 이력 한 칸이 253 MiB(2칸, resurrection이면 4칸)이고 갱신 패스의 화소 수가 4배다. | `TsrResolve.hlsl`, `TsrUpdate.hlsl`, `Upscale.cpp` · `output.upscale_tsr_history_percent`(epic 200, high·performance 100) | 코드 작성·빌드 통과, 실행 안 함 |
| 반투명·레이어 속도 | 불투명 표면 위 레이어에 자기 벡터와 깊이를 준다: 화소를 다 덮는 유리(자기 삼각형의 움직임), 수면(그 깊이의 정지점, 파도는 안 따라감), 커버리지 레이어의 얇은 불투명 조각(화소의 1/3 이상일 때 가장 가까운 조각). 벡터가 없는 입자와 반투명 조각은 불투명도만큼 이력 clamp를 유지하고 이력을 짧게 한다. 추적하는 유리·수면 뒤 배경이 1 px 넘게 다르게 움직이면 clamp를 유지한다. 유리의 투과율은 보지 않으므로 맑은 유리 너머 배경은 시차가 있을 때 거부 쪽에 맡긴다. | `UpscaleMotion.hlsl`, `TsrReject.hlsl`, `Upscale.cpp` · `output.upscale_layer_motion` | 코드 작성·빌드 통과, 실행 안 함 |
| 모션 블러를 업스케일 뒤로 | 업스케일된 뷰는 출력 해상도에서 블러한다. 내부 표본의 벡터를 속도(길이·각도·깊이)로 펴고 16 × 16 타일마다 최단·최장을 구해, 최장 속도가 닿는 타일로 퍼뜨린 뒤, 출력 16 × 16 그룹을 분류해 모은다(정지: 복사 / 한 방향: 단순 평균 / 속도가 섞임: 깊이·도달 가중 / 탭 수보다 긴 블러: 2 × 2 화소당 한 번). 노출은 프레임 시각을 중심으로 양쪽이다(예전 경로는 [t − s·dt, t]). 최대 길이는 화면 폭의 5 %, 탭 16. 회전 단계는 이 경로에 없다. 업스케일 없는 뷰는 예전 경로 그대로다. | `MotionFlatten.hlsl`, `MotionApply.hlsl`, `MotionBlur.cpp`, `ShadingSystem.cpp`(shade 끝부분) · `shading.motion_blur_after_upscale`, `…_max_percent`, `…_samples`, `…_half_res_gather` | 코드 작성·빌드 통과, 실행 안 함 |
| 샤픈·색수차 | 톤매퍼 샤픈(이웃 4개 평균과의 차, 밝은 곳 옆은 덜)과 장면 색 fringe(빨강·초록을 중심 쪽에서 읽음)를 최종 패스의 장면 색에 넣었다. | `PostFinal.hlsl`, `Post.cpp` · `shading.post_sharpen` 0, `post_fringe` 0, `post_fringe_start` 0 (기본 끔) | 코드 작성·빌드 통과, 실행 안 함 |
| 렌즈 플레어 | 블룸 사슬의 1/8 단계에서 문턱 넘는 밝은 부분을 조리개 모양(원 또는 다각형, 91탭)으로 펴고, 1/4 해상도에서 고스트 8개를 각자의 색과 중심 기준 배율로 더한다. 헤일로 고리는 우리가 덧붙인 것이고 기본 0이다. 값은 언리얼 포스트 프로세스 기본값으로 적었는데, 그 값이 있는 Engine 모듈이 참조 폴더에 없어 원본과 대조하지 못했다. | `PostFlare.hlsl`, `Post.cpp` · `shading.post_lens_flare`(기본 끔), `post_lens_flare_*` | 코드 작성·빌드 통과, 실행 안 함 |
| 장면 기준 색 보정 | 전체·암부·중간·명부의 채도·대비·감마·게인·오프셋과 색온도·틴트를 톤 곡선과 함께 32³ LUT 하나로 굽고 최종 패스가 한 번 읽는다. 값이 바뀐 프레임에만 다시 굽는다(디스패치 4번). 기본값이면 LUT를 만들지 않고 그림이 그대로다. 게임은 `FrameContext::grading`으로 프레임마다 준다. 호스트 내보내기(`UnxFrameSetColorGrading`)는 아직 없다. 언리얼은 gamut 확장 뒤에 보정하고, 여기서는 확장이 곡선 안에 있어 보정이 먼저다. | `PostGradeLut.hlsl`, `Post.cpp`, `FrameContext.h` · `shading.post_grading_*` | 코드 작성·빌드 통과, 실행 안 함 |

비교만 하고 바꾸지 않은 것: 필름 그레인(해시 그레인이 이미 동작한다. 언리얼의 암부·중간·명부별 세기와 그레인 텍스처는 없다), 피사계 심도(옥타브 피라미드 적분이라 언리얼의 조리개 날 모양·전경 분리 경로와 구조가 다르다), 자동 노출의 측광(백분위 사이의 로그 평균으로 언리얼과 같은 방식이다). 자동 노출의 적응에는 언리얼의 선형 구간을 스위치로 넣었다(`shading.exposure_linear_distance_ev`, 기본 0 = 지금처럼 지수 적응만. 언리얼 값은 1.5 — 코드 작성·빌드 통과, 실행 안 함).

### 2.4 작업 순서와 현재 위치

1. Lumen 마무리, TSR 구조, 톤 파이프라인·블룸·비네트 — 코드 완료, GPU에서 돌았다(6절).
2. 첫 실행에서 확인된 것 수정 — 완료(6.2).
3. 그 뒤 넣은 것: 국소 노출, MegaLights 화면 추적, 카드 없는 hit의 직접광(radiosity·translucency volume까지) — 두 번째 실행에서 돌았다.
4. 남은 것(순서는 6.4의 시간표와 6.3의 그림에서):
   - **간접광 에너지**: 로비에서 경로 추적 기준 대비 벽 0.26~0.33, 바닥 0.16(6.3). 원인 미확정. 추측으로 고치지 않는다(6.5).
   - **성능**: 4K 17.7~18.7 ms, 목표 6.06 ms(6.4). 같은 표본 수로는 닿지 않는다 — 내부 해상도와 표본 수는 사용자 결정(4절).
   - 반사 2×2 다운샘플 채택 여부, 표면 캐시 피드백(코드는 1.2에 있음, 실행 안 함), TSR 나머지(resurrection, reprojection field), 서브서피스(재질 파라미터가 먼저 필요), 그림자 페이지 구조(정적/동적 분리, 굵은 페이지, HZB), 지오메트리(소프트웨어 래스터, 압축, 스트리밍 연결), 모션 블러를 TSR 뒤로, 옛 경로 코드 삭제.

## 3. 언리얼과 다르게 둔 점

1. 카드 캡처는 클러스터 래스터가 아니라 메시의 원본 삼각형을 그린다. 클러스터 컷으로 그리는 경로를 썼다(`surface_cache.mesh_cards_capture_clusters`, 기본 끔; 코드 작성·빌드 통과, 실행 안 함): V의 래스터 서비스를 그대로 쓰며, 서비스가 화소에 법선을 주지 않아 카드 법선이 삼각형 법선이 된다(1.2의 인터페이스 요청).
2. 카드 아틀라스 무압축(언리얼은 BC 압축): 기하 201 MB + 조명 약 290 MB.
3. 표면 캐시 피드백(코드 작성·빌드 통과, 실행 안 함): 구조는 언리얼과 같다(hit이 원한 페이지와 단계, 한 프레임 넘게 늦게 CPU가 읽음, 잠기지 않은 페이지의 LRU 퇴출). 다른 점: ① GPU 쪽이 "목록 → 해시 표 → 압축" 3패스가 아니라 hit이 해시 표에 바로 넣는다(원소 32비트: 카드 20비트 — 카드 번호 1,048,576 이상은 보고하지 않음). 표가 넘치면(8칸 탐사 실패) 그 보고는 버려지고 머리의 카운터가 올라가며 CPU가 로그로 알린다(`McStats::feedbackDropped`). ② 읽기 지연은 framesInFlight 프레임. ③ 수집 광선은 언리얼처럼 상주 단계만 읽고 보고하지 않는다(`surface_cache.feedback_gather`로 켤 수 있음).
4. 카드 법선: 법선 맵의 평균 기울기까지 반영(언리얼과 같음). 재질 층(코트·시트·박막)은 캡처의 알베도에 들어간다(코드 작성·빌드 통과, 실행 안 함). 층의 거칠기·틴트를 따로 저장하는 아틀라스는 없다: 카드의 final 조명은 언리얼처럼 완전 확산으로 튕기고, 제 재질로 셰이딩하는 hit은 재질 기록에서 층을 읽는다.
5. radiosity 광선의 가까운 뒷면 다시 쏘기(코드 작성·빌드 통과, 실행 안 함): 언리얼의 retrace 모드와 같은 규칙·수치. 스레드당 광선 상한이 4가 됐다(광선, 다시 쏜 광선, 카드 없는 hit의 태양·국소광 그림자 광선).
6. 카드가 없는 hit(스킨·바람 인스턴스, 메시의 카드가 못 보는 면): 언리얼은 0. 여기서는 태양(그림자 광선 1개)과 국소광 표본 1개(그림자 광선 1개, `HitLocalSample.hlsli`)를 준다 — 수집·radiance cache·반사·카드 radiosity·translucency volume 전부. 간접광도 준다(코드 작성·빌드 통과, 실행 안 함): translucency volume의 irradiance, 없으면 radiance cache의 irradiance 프로브(1.2 표 5번). 둘 다 기존 독자와 같은 가림을 거친다(volume 칸의 표본점은 깊이 버퍼 앞, 프로브는 hit을 보는 것만). 로비에서 수집 광선의 표면 hit 중 20~35 %가 카드를 못 읽었다 [실측].
7. 레벨 로드 때 갱신을 프레임당 8회 돌린다(언리얼은 프레임당 1회로 약 100프레임에 걸쳐 채운다).
8. 메시 카드 생성 결과를 디스크에 캐시한다(언리얼은 쿡 때 만든다).
9. 잎 뒷면 확산광: 언리얼처럼 화면 프로브의 irradiance를 뒤집은 법선으로 읽어 확산 이력과 같은 가중으로 누적한다(`view.giBackfaceIrradiance`, 잎 재질이 있는 씬에서만). 평면 반사 뷰와 coverage 조각의 잎은 translucency volume에서 읽는다.
10. 평면 반사 뷰와 coverage 조각(가는 형상·가장자리)의 간접광: 주 뷰의 translucency volume에서 읽는다. 주 뷰 시야 밖의 점(거울 속 카메라 뒤쪽)은 가장 가까운 칸의 값을 받는다. 언리얼은 거울을 평면 카메라로 그리지 않고 반사 광선 + 표면 캐시로 그린다.
11. 화면 추적의 이전 깊이 검사는 언리얼의 수치(근평면 10 cm 기준 장치 깊이 차 0.005 × 0.5~2)를 그대로 옮겼다. 깊이는 fp16 알파에 둔다.
12. 클리어코트: 반사 분류·표본·필터가 코트 거칠기를 쓴다(언리얼의 TopLayerRoughness). 코트가 거울처럼 매끈하면 평면 반사 카메라 후보가 된다.

13. 대기는 카메라에서 100 m 밖부터 그린다(`AIR_VIEW_START_M`). 언리얼의 AerialPerspectiveStartDepth(0.1 km)와 같은 규칙이다. 그 안쪽의 국소광 공기 산란도 같이 빠진다(입자 매질은 그대로).
14. MegaLights 화면 추적은 별도 패스가 아니라 `m.ml.trace`의 스레드 안에서 월드 광선 앞에 돈다. 화면 추적이 못 맞히면 월드 광선은 표면에서 다시 시작한다(언리얼은 화면 추적이 끝난 거리에서 이어 쏜다).
15. 화면 추적이 읽는 이전 색은 음영 그룹 직후의 불투명 색에서 공기를 되돌려 뺀 것이다(`UpscaleSceneKeep.hlsl`). 언리얼은 안개 전에 추출한다. 입자가 덮인 화소는 입자 색이 섞여 있다.
16. radiance cache에 irradiance 프로브를 둔다(코드 작성·빌드 통과, 실행 안 함). 언리얼의 radiance cache는 같은 맵(`CalculateIrradiance`, 6×6)을 irradiance field gather에서만 만들고 화면 프로브 수집의 캐시에서는 끈다. 여기서는 수집의 캐시에 카드 없는 hit의 간접광용으로 만든다. 그런 hit이 프로브를 요청하는 목록(프레임당 2,048개까지, 넘으면 버리고 다음 프레임에 다시 요청)은 언리얼에 없다.
17. 표면 캐시 직접광에 광원 함수를 곱한다(코드 작성·빌드 통과, 실행 안 함). 시간에 따라 변하는 함수의 광원이 비추는 카드 페이지는 직접광 갱신 속도를 4로 올린다(매 갱신 보장은 아니다: 예산 안에서 다른 페이지와 겨룬다). 언리얼은 light function atlas를 갱신 때 읽고 따로 우선순위를 올리지 않는다.
18. 잎 화소 규칙의 대상: 언리얼은 "two-sided foliage 또는 subsurface 셰이딩 모델", 여기서는 Foliage·Subsurface 재질 클래스.

## 4. 품질을 내주는 값(언리얼 기본값에서 시작, 사용자 결정 대상)

- `surface_cache.radiosity_max_ray_intensity = 40`
- `surface_cache.radiosity_max_frames_accumulated = 4`
- `surface_cache.shadow_rays_opaque = false`(언리얼 기본은 true: 알파 마스크 무시)
- `gi.lumen_max_ray_intensity`, `reflection.lumen_max_ray_intensity = 40`, `reflection.lumen_max_roughness = 0.4`, `reflection.lumen_ggx_sampling_bias = 0.1`
- `reflection.lumen_max_roughness_to_trace_foliage = 0.2`(언리얼 기본: 잎·피부 화소는 거칠기 0.2 이상에서 전용 반사 광선 없음. 2026-10-03 추가, 실행 안 함)
- `surface_cache.radiosity_min_trace_distance_m = 0.10`(언리얼 기본: 10 cm 안의 radiosity hit은 빛 0 — 구석이 조금 어두워진다. 2026-10-03 추가, 실행 안 함)

## 5. 실행 방법

`powershell -File Tools\Verify\Run-Ue6Final.ps1 [-Only bt_lobby] [-Resolutions 1080p,1440p,4K] [-Layers gi,refl] [-SkipPictures] [-SkipTimings] [-Out DIR]` (GPU lock의 `HOLD`가 있으면 멈춘다. 스크립트는 `HOLD`를 지우지 않는다.)

- 장면: `C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes`의 bt_bath, bt_lobby, bt_lounge, te_lounge. 게임 경로(내부 해상도 → TSR), 자동 노출, `gi.deterministic=true`.
- 그림: **600프레임 정지**(월드 공간 캐시가 플레이 중처럼 찬 상태) → f600에서 90° 돌린 시야로 컷 → 이후 20°/s 회전. f599(컷 전), f600·f601·f603(컷 직후), f615·f660·f719(회전 중). `pfm_to_png.py`가 국소 노출 + 필름 곡선을 거쳐 PNG로 바꾼다(블룸·비네트·그레인은 없다).
  - 첫 실행은 정지 60프레임이었다. 콜드 스타트 직후라 카드 조명이 덜 찬 상태였고(같은 조건 두 실행의 f59에서 바닥 GI가 18배 달랐다), 그래서 600으로 바꿨다.
- 시간: 회전 600프레임, 정지 600프레임의 패스별 GPU 시간(`timing_*` 폴더; `Tools\Verify\pass_times.py`로 요약). 시간 실행에는 DRED를 켜지 않는다.
- 게이트가 실패로 끝나도(S 오류 비트 등) 나머지 실행은 이어지고 끝에 실패 목록을 낸다. 장치 제거가 로그에 보이면 즉시 멈춘다.
- 로비는 경로 추적 기준 영상이 있다(`UnravelNext-refl\Cache\Reference\lobby\host_ev4_480x270_4096_…`, 호스트 카메라, 4096 spp, EV 4). `Tools\Verify\ref_blocks.py`가 4×3 블록 평균 밝기 비를 낸다. 한계: 기준은 클리어코트·공기·햇빛 집광이 없고 로비 한 시점뿐이다(바닥은 코팅 대리석이라 수치를 그대로 믿기 어렵다).
- `Tools\Verify\Run-Ue6Still.ps1 -Name X -Set k=v,…`: 정지 카메라 한 번 + 기준 대비 블록 비(설정 하나를 바꿔 볼 때).

## 6. 실행 결과 (2026-10-02, RTX 4080)

### 6.1 실행

| 실행 | 빌드 | 결과 |
|---|---|---|
| 첫 실행: 4씬 × 1080p·1440p·4K | 53ce2c4 | 장치 제거 없음. te_lounge 2건에서 S 오류 비트 0x4(VsmMarkFragments 루프 상한) |
| 진단 실행 약 20건(로비 정지, 설정 하나씩) | 53ce2c4 | 장치 제거 없음 |
| 두 번째 실행: 4씬 × 3해상도 | dc5ee06 | 장치 제거 없음, S 오류 비트 전부 0 |
| 로비 1080p·4K | 4ea9dff | 장치 제거 없음 — 이 커밋의 두 변경은 효과가 없어 되돌렸다(6.5) |

### 6.2 첫 실행에서 확인해 고친 것

| 확인된 것 [실측] | 고친 것 |
|---|---|
| 로비 국소광 827개 중 699개가 그림자 없이 빛남(슬롯 128개). 국소 그림자 래스터가 요청 59개 × 패스 19개로 3.4~5.3 ms(해상도 무관) | `shading.mega_lights = true`. 로비 1080p 13.0 → 7.4 ms, 패스 1481 → 311개 |
| 화면 추적이 안개·유리·입자가 섞인 색(업스케일 입력)을 읽음 | 음영 그룹 직후의 불투명 색에서 공기를 뺀 것을 보관(`keepSceneColor`) |
| 실내 전체에 푸른 안개: 공기를 끄면 사라지고 자동 노출이 EV 4.0 → 2.7. 대기 다중 산란에는 캐스터 그림자가 없다 | 대기를 카메라 100 m 밖부터(언리얼 규칙) |
| 수집 광선의 표면 hit 20~35 %가 카드를 못 읽고 국소광 0 | 카드 없는 hit에 국소광 표본 1개 |
| te_lounge에서 느린 메시 406개 때문에 카드 갱신이 계속 프레임당 8회(r.card 3.3 ms) | 로드 페이스는 캡처할 카드나 남은 조명 라운드가 있을 때만. r.card 0.53 ms |
| te_lounge S 오류 비트 0x4 | 루프 상한 256 → 1024 페이지(1440행에서 필요 314) |
| 로비 천장 구름 무늬, bath 크림색 천장 | 이전 렌더러 캡처에도 똑같이 있음 — 씬 내용, 고치지 않음 |

### 6.3 그림 (두 번째 실행, 1080p)

- 푸른 안개가 없어지고 로비·라운지·bath가 따뜻한 조명색으로 나온다. te_lounge는 처음부터 좋았다.
- 컷 프레임 f600: MegaLights 첫 프레임의 입자 잡음이 전 화면에 보이고, 로비 천장에는 어두운 얼룩이 f600·f601에 있다. f603부터 없다. **컷 직후 두 프레임은 아직 깨끗하지 않다.**
- 회전 중(f615·f660·f719): 눈에 띄는 잔상·얼룩 없음.
- 로비 정지 f599, 경로 추적 기준 대비 블록 밝기 비: 천장 0.61~0.75, 벽 0.26~0.33, 바닥 0.16. 자동 노출이 EV 2.97로 올려서 그림은 어둡게 보이지 않지만 **빛의 양은 기준의 1/3 수준**이다. 같은 조건(공기 끔)에서 옛 경로는 벽 0.91~0.96, 바닥 0.63~0.72였다.

### 6.4 시간 (두 번째 실행, 회전 중 GPU 프레임 중앙값, ms)

| 씬 | 1080p | 1440p | 4K | 첫 실행 4K |
|---|---|---|---|---|
| bt_bath | 6.96 | 9.94 | 18.71 | 21.28 |
| bt_lobby | 7.51 | 10.33 | 18.47 | 24.95 |
| bt_lounge | 6.47 | 9.41 | 17.74 | 19.13 |
| te_lounge | 8.19 | 10.34 | 17.84 | 19.80 |

4K(내부 1440p)의 큰 항목: GI 3.4~3.7, MegaLights 2.0~3.2, TSR 2.16, 반사 1.5~2.9, 불투명 음영 0.8~1.1, 프록셀 0.8~1.3, 그림자 0.7~1.0, 재질 resolve 0.6~0.7, coverage 0.6~0.7(te_lounge 정지 시야는 coverage 층이 4.4 ms).
목표 6.06 ms와 3배 차이다. 언리얼 자체도 이 GPU에서 Epic 설정으로는 이 수치에 닿지 않는다 [추정]. 같은 표본 수·같은 내부 해상도로는 닿을 길이 보이지 않는다.

### 6.5 진단에서 알게 된 것과, 하지 말아야 할 것

- 공기 끈 로비, 기준 대비(천장 / 벽 / 바닥): 옛 경로 전체 1.2~1.8 / 0.91~0.96 / 0.63~0.72, Lumen GI + 슬롯 그림자 1.2~1.3 / 0.40~0.61 / 0.26, Lumen GI + MegaLights 0.55~0.76 / 0.28~0.43 / 0.16.
- 세기 상한을 하나씩 끔: radiosity 상한은 바닥 +0.13, 수집 상한은 천장만 올림, 반사 상한은 변화 없음, translucency volume 상한은 입자(증기) 밝기만 올림. **상한은 주원인이 아니다.**
- radiance cache를 끄면 GI가 1/10로 떨어진다(EV 1.8). 원인 미확정.
- MegaLights 가중치 상한을 유효 광원 수에 맞추는 변경과 TSR 거부 커널 타일 20은 **추측이었고 실행에서 효과가 없어 되돌렸다**(fd56292). 간접광 에너지는 원인을 코드에서 읽어 확정하기 전에는 손대지 않는다.

## 7. 배치 실행과 그 결과 (2026-10-02 오후, RTX 4080)

### 7.1 실행 방법 — 코드 작업 한 구간 뒤에 한 번

`powershell -File Tools\Verify\Run-Ue6Batch.ps1 [-Out Cache\Ue6Batch] [-Skip furnace,game,generated,variants]`
한 번에: 퍼니스 방 변형들 → 게임 씬 4개(1080p 컷 그림 + gi·direct 레이어, 1080p·1440p·4K 시간) → scenegen 씬 7개(1080p 그림·시간, 4K 시간) → 변형(high 티어, 안개 켬, 그림자 캐시 옛 방식). 끝나면 `<out>\summary.txt` 한 장(퍼니스 표, 실행별 GPU 프레임과 큰 패스 그룹, 실패한 게이트).

- **퍼니스 방**(`--scene furnace_room`, `furnace_room_day`; `Tools\Verify\furnace.py`): 8×4×8 m 닫힌 람베르트 방(알베도 0.5, 중앙 100 cd). 직접광 평균 4.909 lux, 모든 반사 4.909 lux, 벽 휘도 1.5625 nits가 닫힌 식으로 정해져 있어 단계별(카드 직접·카드 간접·수집·최종 그림)로 "측정/기대"를 낸다. `_day`는 같은 방에 바깥만 낮: 밤 방보다 많이 나온 빛은 전부 벽을 통과한 것이다(바깥이 방보다 수천 배 밝아 0.03 %의 누광도 보인다).
- 품질 티어: `output.tier = "epic"`(기본) / `"high"`(`Config/quality/tiers/high.toml`을 덮어씀). `--set output.tier=high`.
- 4K는 내부 1080p로 그린다(`output.render_height_max = 1080`). 1080p 출력은 내부 720p다.

### 7.2 첫 배치(빌드 0238b66a)에서 확인한 것과 고친 것

| 확인된 것 [실측] | 원인 [코드에서 읽음] | 고친 것 |
|---|---|---|
| 낮 퍼니스 방 수집 간접광 ×2.36(밤 ×1.08) | radiance cache 프로브의 광선이 중심에서 셀 대각선만큼 떨어진 곳에서 출발 — 벽 근처 프로브는 벽 **뒤**에서 출발 | 프로브 가림(`lumen.radiance_cache_probe_occlusion`): 출발점까지 막힌 텍셀은 비움, 조회는 질의 지점이 보이는 프로브만. ×1.08 |
| 낮 방 최종 그림 ×1.77(밤 ×1.10), 안개 끄면 ×1.09 | 프록셀 슬라이스가 벽을 지나 바깥까지 뻗고, 그 구간의 햇빛이 안개에 들어감 | `atmosphere.froxels.clip_at_surface`: 타일의 가장 먼 표면까지만 샘플. ×1.17 |
| 남은 ×1.17 − ×1.09 = 안개 간접광만 | 반투명 볼륨 셀 표본이 벽 뒤에 남음(원본의 임계 1 슬라이스: 벽 뒤 셀은 4프레임 중 3번 밖에서 추적) | 표본을 항상 표면 앞으로(`lumen.translucency_volume_depth_offset_threshold = 64`), 필터는 추적 안 된 셀 제외 — 7.4에서 측정 |
| 컷 첫 프레임 잡음(bath f600: 이웃 픽셀 0.07 / 0.29) | 픽셀마다 프로브 4개 중 1개를 뽑는 확률적 보간 + 히스토리 없음 | 히스토리 4프레임 전까지 4개 혼합, 히스토리 없는 픽셀은 같은 면 주변 평균 — 7.4에서 측정 |
| 호숫가 67~83 ms (s.vsm 54 ms: 레벨 12개 × 5 ms) | 바람에 흔들리는 나무가 매 프레임 모든 레벨의 페이지를 stale로 만듦 | 레벨별 "변화의 값"(`shadow.vsm.cache_min_change_texels`): 텍셀 반 개보다 작은 변화는 무시, 바람은 그만큼 움직일 시간이 지난 뒤에만 — 7.4에서 측정 |
| 숲 2개 씬 시작 실패(레벨 0이 4e8 클러스터) | 래스터 상한 계산이 바람 나무 110만 그루를 모든 뷰에 셈 | 바람 나무를 제자리(바람 한계만큼 키운 구)로 셈. 굵은 레벨은 여전히 110만 × 1 > 용량 104만일 수 있음 → 인스턴스 배치가 필요(남은 일) |
| ridge_sunset S 오류 비트 0x4 | 먼 평면의 coverage 기록이 마지막 레벨 페이지를 수천 개 걸음 | 걸음을 레벨 창 안으로 자름(구조적으로 2 × 128 이하) |
| high 티어 실행 실패 | `--set output.tier=high`의 맨 단어를 문자열로 못 읽음 | 문자열 키는 맨 단어를 받음 |
| 안개를 기본으로 켠 그림: 하늘·먼 땅이 회색으로 덮이고 실내 천장에 띠 | 높이 안개가 하늘 픽셀까지 덮음(원본은 SkyAtmosphere가 있으면 불투명 픽셀에만), 국소광이 많은 곳은 프록셀 해상도가 띠로 보임 | **기본 끔**(씬이 요청할 때만). 하늘 제외·띠는 남은 일 |

### 7.3 시간 (첫 배치, 회전 중 GPU 프레임 중앙값, ms)

| 씬 | 1080p (내부 720p) | 1440p | 4K (내부 1080p) |
|---|---|---|---|
| bt_bath | 7.00 | 10.14 | 12.33 |
| bt_lobby | 7.58 | 10.63 | 12.72 |
| bt_lounge | 6.67 | 9.66 | 11.61 |
| te_lounge | 8.41 (정지 14.22) | 10.61 | 11.69 (정지 16.85) |
| city_block | 7.62 | – | 10.76 |
| city_night | 9.30 | – | 13.72 |
| interior | 4.66 | – | 8.77 |
| ridge_sunset | 4.68 | – | 7.12 |
| waterside | 66.92 | – | 66.66 |

- 4K는 17.7~18.7 → 11.6~12.7 ms(내부 해상도 상한). 4K의 큰 항목: GI 2.1~2.3, MegaLights 1.5, TSR 1.23, 반사 0.85~1.6, coverage 0.7, 음영 0.6.
- te_lounge 정지 시야: coverage 층(m.coverage 2.85 + v.coverage 2.5)과 s.shadow 1.94가 크다 — 남은 일.
- 야외: s.vsm이 가장 크다(city_night 3.0, city_block 4K 2.4). 움직이는 물체 1,024개가 페이지를 매 프레임 다시 그리게 한다.

### 7.4 두 번째·세 번째 배치 (빌드 13148698, 9b71ee8a + 대역 C 모드) — 7.2의 수정이 실제로 한 일

퍼니스 방, 최종 그림 (기대 1.5625 nits 대비):

| 변형 | 수집 간접광 | 최종 그림 |
|---|---|---|
| 밤 | ×1.08 | ×1.10 |
| 낮 (현재 기본 + 안개 켬) | ×1.08 | ×1.11 |
| 낮, 프로브 가림 끔 | ×2.28 | ×1.70 |
| 낮, 슬라이스 클립 끔 | ×1.08 | ×1.67 |
| 낮, 반투명 볼륨 임계 = 원본의 1 | ×1.08 | ×1.19 |
| 낮, 안개 끔 | ×1.08 | ×1.09 |
| 밤, radiance cache 끔(전체 길이 광선) | ×0.97 | ×1.05 |

낮 방과 밤 방의 차이가 0.011 nits다(수정 전 1.05 nits). 세 누광 경로가 각각 끄면 다시 나타나므로 원인과 수정이 일대일로 확인된다.

컷 직후 GI 레이어의 픽셀 잡음(3×3 평균 대비 상대 편차 평균, bath 1080p; 정지 상태 0.012):

| | f600 | f601 | f603 |
|---|---|---|---|
| 수정 전 | 0.075 | 0.048 | 0.026 |
| 4개 혼합 + 히스토리 없는 픽셀 평균 | 0.020 | 0.033 | 0.020 |
| + 히스토리 4프레임 미만 픽셀 평균 | 0.020 | 0.014 | 0.009 |

시간(회전 중 GPU 프레임 중앙값, ms):

| 씬 | 1080p | 1440p | 4K | 바뀐 이유 |
|---|---|---|---|---|
| bt_bath | 6.86 | 9.57 | 11.62 | 안개 끔 |
| bt_lobby | 7.26 | 10.05 | 12.11 | |
| bt_lounge | 6.24 | 9.07 | 10.99 | |
| te_lounge | 7.90 (정지 13.47 → 대역 C 모드 12.48) | 9.97 | 11.53 | |
| city_block | 5.28 (7.62) | – | 8.74 (10.76) | 그림자 캐시 |
| city_night | 6.09 (9.30) | – | 10.56 (13.72) | 그림자 캐시(옛 방식으로 되돌리면 9.28) |
| interior | 4.62 | – | 8.71 | |
| ridge_sunset | 4.68 | – | 7.08 | 오류 비트 없음 |
| waterside | 22.39 (66.92) | – | 26.61 | 그림자 캐시(옛 방식 66.39). s.vsm 54 → 2.0 ms |

- 호숫가의 남은 22 ms: coverage 층(v.coverage.raster 7.8 + m.coverage 3.3). 메인 뷰가 클러스터 21만 개를 그린다 — 얇은 지오메트리(갈대·잎)는 LOD로 줄이지 않는 규칙(`visibility.lod_max_relative_width_error`) 때문이다. 대역 C(폭 0.25 px 미만)를 vis buffer로 보내는 모드(`visibility.coverage_band_c_visbuffer`, 기본 켬)는 te_lounge 정지 15.0 → 12.5 ms, 호숫가 23.3 → 22.0 ms: **대부분은 대역 B(0.25~1.5 px)의 비용**이다. 남은 일.
- 숲 2개 씬: 인스턴스 배치(`RasterView::instanceFirst/End`)를 넣었으나 상한이 "창 안의 모든 캐스터 × 그 레벨의 절단"이라 중간 레벨에서 용량의 수천 배 → 요청 5,146개로 실행 불가였다. 배치 4개까지만 나누고 그 이상은 요청 하나로(목록이 넘치면 V가 보고) 바꿨다 — 7.5.
- high 티어(첫 정의: 반사 반해상도 + TSR flicker 끔)는 로비 4K 12.11 → 11.59 ms뿐이었다. 원본의 High 묶음(프로브 간격 32 px, radiance cache 프로브 16², MegaLights 표본 2)을 더했다 — 7.5.
- 안개 켠 그림은 `Cache\Ue6Batch2\fog\`에 있다(기본은 끔).

### 7.5 다섯 번째·여섯 번째 배치 (빌드 1876d242, 78d5df55) — 안개 완성, 격자의 각도화, 티어, 병합

실행 방식: 배치는 스냅샷 워크트리(`UnravelNext-ue6-run`)에서, 코드는 개발 워크트리에서. 구현 네 건(얇은 지오메트리 LOD, 피부 A·B 단계, 머리카락 음영)은 각자의 워크트리에서 에이전트가 만들고 여기서 검토·병합했다. 남은 일과 설계는 `UE6_WORKPLAN_KO.md`.

**측정에서 배운 것 (먼저 읽을 것)**

- 배치 5의 시간 실행은 다른 워크트리의 빌드·테스트와 겹쳤다(GPU 잠금 기록의 `cpu-contended`). 프레임 중앙값이 약 1 ms 부풀었고(로비 4K 11.86 → 12.95) 패스 중앙값의 합은 그대로였다(11.79 → 11.88): 코드의 비용은 변하지 않았다. 요약표에 **패스 중앙값의 합**(`sum`)을 함께 낸다 — 겹침에 강한 값이다.
- 패스 CSV에는 같은 이름의 패스가 한 프레임에 여러 번 나온다. 행 단위 중앙값을 더하면 틀린다(이 실수로 "MegaLights 패스가 두 상태를 오간다"는 틀린 해석을 한 번 했다가 프레임별 값을 직접 보고 거뒀다).
- 프레임 = 해상도와 무관한 일 3.3 ms + 내부 100만 픽셀당 4.1 ms [로비, 내부 720p와 1080p 두 실행].

**티어 (로비 4K, 안개 켬은 epic만; GPU 프레임 중앙값 / 패스 중앙값의 합, ms)**

| 티어 | 정지 | 회전 | 내부 해상도 | 내주는 것 |
|---|---|---|---|---|
| epic (기본) | 11.76 / 11.44 | 11.91 / 11.53 | 1080p | — |
| high | 9.98 / 9.64 | 10.21 / 9.84 | 1080p | 프로브 32 px, 반사 2×2당 1광선, MegaLights 표본 2, radiance cache 프로브 16², TSR flicker 끔 |
| performance (신규) | 7.44 / 7.25 | 7.60 / 7.33 | 720p (×3 업스케일) | 컷·가려졌다 드러난 곳에서 세부가 늦게 온다, 반사 2×2당 1광선, TSR flicker 끔 |

- 프록셀 타일과 반투명 볼륨 셀이 기준 높이(720) 위에서 각도를 유지하게 했다: 4K에서 s.froxel 0.91 → 0.46 ms, r.gi 2.32 → 2.13 ms. 1080p 출력(내부 720p)은 그대로다.
- 컷 직후 4K 프레임(f601, f603)에서 작은 밝은 세부에 내부 픽셀 격자가 비쳤다(×3에서 더 뚜렷) [그림: Batch6 tier_high, tier_performance]. 두 번째 프레임부터 커널이 출력 픽셀 폭이 되어, 그 프레임 표본 옆의 출력 픽셀만 선명해지기 때문이다. 표본 n개의 간격은 1/√n 입력 픽셀이므로 커널이 그 속도로 좁아지게 했다(`output.upscale_tsr_kernel_by_samples`, ×2에서 4프레임·×3에서 9프레임) — **실행 확인 전**.

**안개 (켠 상태; 기본은 끔)** — 구조는 작업 계획 1절.

- 퍼니스 방: 낮 = 밤 = 1.7194 nits(×1.10, 안개 없는 방 1.7090): 원거리 GI·안개 추가 뒤에도 누광 없음.
- 비용: 로비 7.60 → 7.53, city_block 6.19 → 6.15, city_night 7.23 → 7.19, ridge 5.07 → 4.97 (1080p 정지; 차이는 잡음 범위) — 하늘·원거리 그림자·잡음·반사 광선·거울 뷰를 더한 뒤에도 0.1 ms 안.
- 그림 [Batch6 `fog*`]: 능선은 안개 낀 하늘 앞에 어두운 실루엣으로 서고, 능선 왼쪽 절벽의 그림자가 먼 안개에 쐐기 모양으로 드리운다(`far_shadows` 끄면 능선이 안개 빛에 하얗게 뜬다). 밤 도시는 물웅덩이 반사가 안개만큼 흐려지고(`on_rays`), 호수는 안개 낀 하늘과 건너편을 비춘다(`secondary_views`). 밀도 잡음(0.3)은 이 씬들에서 눈에 띄는 차이를 만들지 않았다(아티팩트도 없음).
- 구름 + 안개: 구름 그림자 아래 안개와 땅이 어두워진다. 구름 뒤의 태양 원반이 그대로 하얗게 그려지던 것을 고쳤다(원반·달·별이 구름의 투과율을 받는다) — **실행 확인 전**.
- 국소 안개 볼륨(`FrameContext::fogVolumes`, `UnxFrameSetFogVolumes`, 게이트 `--fog-volume`)을 추가했다 — **실행 확인 전**.

**캐릭터 음영**

- 피부 A단계(이중 GGX 로브, 얇은 부분 투과광), B단계(화면 공간 SSS: Burley 프로파일, 접평면 표본, 알베도는 산란 뒤 적용)를 병합했다. GPU 단위 테스트 통과(`subsurface*` 4/4), 프레임 테스트 10개 항목 통과(다른 클래스 픽셀 407,612개 비트 동일). 평균 자유 경로 기본값을 왁스 수준(12 mm)에서 피부 실측(1.30 / 0.95 / 0.67 mm)으로 바꿨다: 산란은 근접 장면에서만 보인다(`shading_ball`에 근접 카메라 `skin_close` 추가; 켜면 명암 경계가 붉게 풀린다 [그림: `Cache\Ue6Diag\skin_on`, `skin_off`]).
- 머리카락: 가닥 기록이 섬유 모델(폭 평균 + 이중 산란, 밀도 볼륨 64³)로 음영된다. `visibility.coverage_hair` 기본 켬. 진단 씬 `hair_ball`(가닥 기록 373만 개, 1080p)에서 3.56 → 21.3 ms: 기록 음영 6.6, 합성(정렬·라운드) 7.5. 머리카락은 아직 그림자를 드리우지 않고 반사·GI에 없다.

**얇은 지오메트리 LOD** (`visibility.lod_thin_preserve_area`, 병합·런타임 연결 완료)

- 빌더 실측: 잎 4만 장 수관의 가장 거친 절단 80,000 → 4,568 삼각형(면적 1.000). forest_thin 잎 전체 클러스터 4.07억 → 2,530만(그중 2,500만은 풀: 방향 부류별 DAG라 줄지 않는다 — `sheet_orientation_min_width` 변형으로 A/B 중).
- 호숫가 1080p 정지 22.44 → 20.26 ms(v.coverage 9.33 → 7.48). 남은 비용은 가까운 갈대·풀의 coverage 층(대역 B)이다 — 대역 A 폭 변형으로 A/B 예정.

**알려진 실패 테스트** (오늘의 병합 전 커밋에서도 같은 값으로 실패: 병합이 원인이 아님)

- `unx_test_shading_shadingtests` 3건: 디스플레이 인코딩(941 LSB), emissive 패널 2건. `unx_test_visibility_fragmenttests` 2건(전환 정확성, 날아가는 조각). 대역 C 설정과 무관함을 A/B로 확인. 전체 테스트(113개 실행 파일)의 분류·수리는 별도 에이전트가 진행 중(`UE6_TESTS_KO.md`에 기록 예정).

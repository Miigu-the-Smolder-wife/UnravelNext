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

옛 경로(월드 GI 해시 캐시, 그 화면 프로브, 반사의 K/G/M·rays buffer·inline·shade·combine 패스, 해시 셀 표면 캐시)는 스위치를 끄면 그대로 돈다. GPU 실행으로 새 경로를 확인한 뒤 코드에서 지운다. 기본값에서 닿지 않는 파일·커널·설정 키와 그것을 참조하는 시험의 목록은 1.3.5에 있다(지운 것 없음).

### 1.2 2026-10-03 추가분 (브랜치 `w/cache`) — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 돌린 것은 없다(시험 실행 파일·캡처·게이트·furnace 포함). 확인은 `Tools\CI\Build.ps1 -Track all`의 `build ok`뿐이다.

| # | 항목 | 내용 | 스위치(기본) | 위치 |
|---|---|---|---|---|
| 1 | 표면 캐시 피드백(해상도) | 카드의 높은 단계를 읽는 hit(반사·굴절 광선, 카드 radiosity 광선)이 "어느 카드의 어느 단계·어느 페이지를 원했는지"를 GPU 해시 표(4096칸, 원소 = 카드 20비트·단계·페이지 좌표, 값 = hit 수)에 넣는다. 원한 단계 = log2(카드 반크기 / hit에서의 광선 원뿔 반지름) − 0.5. 화면 16×16 타일마다 프레임당 한 hit만 보고한다(자리는 프레임마다 옮겨 간다). 다음 프레임 첫머리에 표를 readback으로 복사하고 비우며, framesInFlight 프레임 뒤에 CPU가 읽어 hit 수 16 초과인 페이지를 요청으로 만든다. 요청은 상주 단계 요청보다 25~50 m 뒤 순위다. 페이지는 잠기지 않은 단계에 한 장씩 매핑·캡처하고(조명은 기존 단계에서 resample), 256프레임 동안 아무 hit도 원하지 않으면 나간다. 자리가 없으면 가장 오래 안 쓴 것부터 내보낸다. 높은 단계의 매핑 안 된 페이지는 페이지 표가 아래 단계의 매핑된 페이지로 보낸다. 최근 2회 갱신 안에 읽힌 페이지는 조명 갱신 속도 4(언리얼의 CardPageHighResLastUsedBuffer). | `surface_cache.feedback`(켬), `feedback_tile_size` 16, `feedback_res_level_bias` −0.5, `feedback_min_page_hits` 16, `feedback_keep_unused_frames` 256, `reflection.lumen_hi_res_surface`(켬), `surface_cache.feedback_gather`(끔) | `MeshCardScene.cpp`(setFeedback, evictOldest), `SurfaceCacheCards.cpp`(readback·clear), `CardLighting.hlsli`(clFeedback, clReadCardsAt), `CardLayout.hlsli`(mcCardSample hiRes, 카드 프레임 28워드), `CardFeedbackClear.hlsl`, `CardSelect.hlsl` |
| 2 | 클러스터 LOD로 카드 캡처 | V의 래스터 서비스(`FrameServices::rasterizeDepth`)로 그린다. 캡처 페이지 하나가 요청의 뷰 하나다(정사영, `lodPixelsPerMetre` = 페이지의 미터당 텍셀, 그 인스턴스만, 8텍셀 타일을 캡처 아틀라스의 제자리로 보내는 tile atlas 모드). 요청 하나에 4096뷰, 묶음마다 서비스를 **한 번** 돈다(`r.card.vcapture`): 깊이 타깃 = 캡처 깊이, 렌더 타깃 = 알베도·법선·방출, 깊이 테스트가 카드 앞쪽에 가장 가까운 표면을 남긴다. 텍셀 법선·접선은 서비스가 주는 보간된 꼭짓점 값이다(2026-10-03 `w/opt` 둘째 묶음: `UE6_WORKPLAN_KO.md` 8.3 (e)). | `surface_cache.mesh_cards_capture_clusters`(**끔**: A/B 한 번 뒤에 켠다) | `CardCaptureCluster.ps.hlsl`, `SurfaceCacheCards.cpp`(captureView, r.card.vcapture.begin), `CardCopy.hlsl`(뒤집힌 깊이), `DepthRaster.h`(pixelNormals, colorTargets, pixelViews) |
| 3 | 카드 캡처의 재질 층 | 층 있는 재질의 카드 알베도를 법선 방향에서 본 반사율로 캡처한다(언리얼 Substrate 캡처와 같은 규칙: V = N). 클리어코트: 코트 로브 알베도 + 코트를 두 번 지난 아랫층(코트 거칠기가 투과율을 정함), 시트(sheen): 틴트 × 시트 알베도 + 남은 아랫층, 박막: f0를 박막의 수직 입사 값으로. hit이 제 재질로 셰이딩하는 경로(수집·반사)는 원래 층을 계산했고, 카드의 final 조명을 읽는 경로(radiosity, translucency volume)가 이제 같은 양을 튕긴다. | 없음(항상) | `CardCaptureMaterial.hlsli` `ccLayered` |
| 4 | radiosity 다시 쏘기 | 첫 hit이 5 cm 안의 한 면 재질 뒷면이면 5 cm부터, 1 cm 안의 양면 재질이면 그 hit 바로 뒤부터 한 번 더 쏜다(루프 없음). 10 cm보다 가까운 hit은 광선을 막되 빛은 0이다. 스레드당 광선 상한 3 → 4, 디스패치당 스레드 87,381 → 65,536. | `surface_cache.radiosity_avoid_self_intersections`(켬), `radiosity_skip_back_face_m` 0.05, `radiosity_skip_two_sided_m` 0.01, `radiosity_min_trace_distance_m` 0.10 | `CardRadiosityTrace.hlsl`, `CardLighting.cpp` |
| 5 | 카드 없는 hit의 간접광 | 태양·국소광 표본에 더해 간접 irradiance를 준다. ① translucency volume: hit이 주 뷰 격자 안이고 그 칸이 추적된 빛을 가졌을 때(ambient의 알파 = 칸이 가진 추적 빛의 몫, 새로 넣음). ② 아니면 radiance cache의 irradiance 프로브: 추적된 프로브마다 6×6(+테두리) irradiance 맵을 만들고, hit 둘레 8개 프로브 중 hit을 보는 것(기존 깊이 검사)만 쓴다. 그런 hit은 16개 중 하나가 위치를 목록에 적고, 다음 프레임 캐시 표시 단계가 그 자리에 프로브를 요청한다(clipmap 2단계 거칠게). ③ 둘 다 없으면 전과 같이 0. 소스는 카드 프레임 워드 22~27로 전달한다: 표면 캐시·radiance cache·volume 자신의 광선은 이전 프레임 volume, 수집·반사는 이번 프레임 것. | `lumen.hit_indirect`(켬), `hit_indirect_radiance_cache`(켬), `hit_indirect_mark_clipmap_bias` 2 | `Passes/GI/LumenHitIndirect.hlsli`, `LumenRadianceCacheIrradiance.hlsl`, `LumenRadianceCache.hlsli`(lrcIrradiance, lrcHitMark), `LumenTranslucencyVolume.hlsli`(ltvSampleLit), `CardFrameSources.hlsl`, `LumenGather.cpp` |
| 6 | 대조에서 구현한 것 | 잎 화소(Foliage·Subsurface 재질 = 언리얼의 backface diffuse 화소): 수집 이력의 거리 임계 0.03(일반 0.01), 전용 반사 광선은 거칠기 0.2 미만에서만. 수집 이력의 법선 거부(45°)는 스위치만 넣고 끔(언리얼 기본 0). | `lumen.gather_temporal_distance_threshold_foliage` 0.03, `lumen.gather_temporal_reject_normal`(끔), `reflection.lumen_max_roughness_to_trace_foliage` 0.2 | `LgTemporal.hlsl`, `ReflectionClassify.hlsl`, `ReflectionReuseFilter.hlsl`, `ReflectionInternal.hlsli` |
| 7 | 표면 캐시 직접광의 광원 함수 | 점·스폿 광원의 함수(IES, 쿠키, 고보, 세기·색 키, 깜빡임)를 카드 텍셀/셀 방향으로 곱한다(footprint = 텍셀 크기 / 광원까지 거리). 표는 광선 씬의 광원 데이터 머리(바이트 80)가 가리키는 그 프레임의 표이고, 패스는 원래 `declareTraversal`로 선언하고 있었다. 시간에 따라 변하는 함수(회전, 키 2개 이상, 깜빡임)의 광원이 비추는 곳: 카드는 페이지 조명 상태에 표시해 직접광 갱신 속도 4, 셀은 피드백 목록에 넣어 먼저 다시 비춘다. 표면 캐시에는 광원별 개정 번호가 없다 — 저장값은 갱신 예산이 돌아올 때 새로 계산된다. | 없음(항상) | `SurfaceCacheLightFunction.hlsli`, `CardDirectStore.hlsl`, `CardDirectCull.hlsl`, `CardSelect.hlsl`, `SurfaceCacheLight.hlsl`, `SurfaceCacheLightPairsSelect.hlsl` |

2번이 V에 요청했던 것 — 셋 다 들어갔다(코드 작성·빌드 통과, 실행 안 함; `UE6_WORKPLAN_KO.md` 8.3 (e)). 스위치는 끈 채로 두었다: 이 경로는 한 번도 돌지 않았고 캡처는 모든 GI 결과의 입력이라, 배치의 A/B 한 번 뒤에 켠다.

- `DepthRasterPixel`의 보간된 꼭짓점 법선과 접선: `DepthRasterRequest::pixelNormals`(`DEPTH_RASTER_NORMALS 1`).
- 화소 커널이 그래프 리소스의 뷰 번호를 받는 길: `DepthRasterRequest::pixelViews`(래스터 패스가 실행될 때 `pixelConstants`에 써 준다). 캡처 쪽의 자체 업로드 버퍼는 없앴다.
- 깊이와 재질을 한 번에: `DepthRasterRequest::colorTargets`(화소 커널의 렌더 타깃). 컬 체인이 묶음당 2회 → 1회.

**6번 대조 — 옮기지 않은 것** (`Engine/Shaders/Private/Lumen/*`, `LumenScreenProbeGather.cpp`, `LumenReflections.cpp`, `LumenReflectionTracing.cpp`의 cvar 기준):

| 언리얼 | 상태 | 이유 |
|---|---|---|
| 수집: InterpolationDepthWeight.Foliage, TwoSidedFoliageBackfaceDiffuse, ScreenTraces SkipFoliageHits, TemporalFilterProbes, 중요도 표본 2단계, ShortRangeAO | 이미 있음 | |
| 수집: MaxRoughnessToEvaluateRoughSpecularForFoliage 0.8 | 따로 두지 않음 | 일반 화소 값(0.8)과 같다 |
| 수집: ShortRangeGI | 안 옮김 | 언리얼에서 실험 기능, 기본 끔 |
| 수집: IntegrateDownsampleFactor, GatherNumMips, SpatialFilterHalfKernelSize, FastUpdateModeUseNeighborhoodClamp, RadianceCache.SkyVisibility, ExtraAmbientOcclusion | 안 옮김 | 언리얼 기본값에서 꺼져 있거나 1 |
| 수집: ScreenTraces SkipHairHits·SkipUnlitHits, MinimumOccupancy, ThicknessScaleWhenNoFallback | 안 옮김 | 기본 false이거나 거리장 대체 경로용 |
| 수집·반사: HairStrands.ScreenTrace / VoxelTrace | 머리카락 트랙이 구현(`RayTracing/HitHair.hlsli`: 밀도 볼륨의 첫 가닥) | 1.3에서 남은 두 커널(카드 radiosity, 굴절 서비스)에도 넣었다 |
| 반사: ScreenSpaceReconstruction, BilateralFilter(표본 수·반경·깊이 가중·disocclusion 프레임), Temporal, DenoiserTonemapRange, GGXSamplingBias, RoughnessFadeLength, SampleSceneColorAtHit, DownsampleFactor, 수집의 rough specular 재사용 | 이미 있음 | |
| 반사: HiResSurface, SurfaceCacheFeedback | 1번에서 구현 | |
| 반사: DistantScreenTraces | 1.3에서 구현(기본 설정에서는 걷는 구간이 0) | 언리얼에서는 광선 씬이 컬링 반경에서 끝나는 곳부터 화면을 따라가는 장치이고, far field가 켜지면 돌리지 않는다. 여기는 컬링 반경으로 끊기는 근거리 씬이 따로 없다: 반사 광선이 광선 씬 전체를 `gi.ray_length_m`까지 추적한다. 그래서 광선 길이가 2 km보다 짧게 설정됐을 때만 돈다 |
| 반사: RadianceCache(거친 반사 광선을 줄이고 캐시에서 읽기) | 안 옮김 | 언리얼 기본 0 |
| 반사: DownsampleCheckerboard, MaxRoughnessToTraceClamp, SmoothBias, SpecularScale, Contrast | 안 옮김 | 기본값에서 동작 없음 |
| 반사: 반투명 앞면 층 반사(front layer) | 굴절 서비스의 반사 job이 그 역할이다(1.3 9번에서 hit 처리를 반사 광선과 맞춤) | 거칠기 있는 로브 표본과 디노이저는 없다(1.3.2) |
| 반사: HitLighting 모드·MaxBounces | 안 옮김 | 재질 hit 셰이더가 필요. 기본은 표면 캐시 조명 |
| 반사: HierarchicalScreenTraces.IncludeTranslucencyDepth, MinimumOccupancy | 안 옮김 | 기본값에서 동작 같음 |

### 1.3 2026-10-03 두 번째 구간 (브랜치 `w/cache`) — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 돌린 것은 없다(시험 실행 파일·캡처·게이트·furnace 포함). 확인은 `Tools\CI\Build.ps1 -Track all`의 `build ok`뿐이다. 빌드한 것은 마지막 커밋의 트리다: 항목별 커밋 7개는 그 트리를 나눠 담은 것이고, 중간 커밋을 따로 빌드하지 않았다. DXIL 크기는 가장 큰 것이 153 KB(`LgTrace`, 한도 200 KB).

#### 1.3.1 구현한 것

| # | 항목 | 내용 | 스위치(기본) | 위치 |
|---|---|---|---|---|
| 8 | 카드 radiosity 광선의 머리카락 | 머리카락 프록시(밀도 볼륨의 첫 가닥)를 쓰지 않던 마지막 GI 광선이었다. hit보다 앞에 가닥이 있으면 그 가닥이 hit이다(태양 그림자 광선 1개 + 국소광 표본 1개로 조명, 텍셀이 그 안에 들어 있는 머리는 제외). 스레드당 광선 상한 4는 그대로다(표면 hit의 그림자 광선과 프록시의 그림자 광선은 둘 중 하나만 쏜다). 표면 캐시가 `RayScene::recordHair`를 부른다 — 그 프레임의 밀도 볼륨이 표면 캐시 기록 시점에 이미 발행돼 있을 때만. 발행이 표면 캐시 기록보다 늦는 프레임 순서라면 이 광선은 머리카락을 보지 못한다(순서는 확인하지 않았다). | `raytracing.hair`(기존) | `CardRadiosityTrace.hlsl`, `SurfaceCacheCards.cpp` |
| 9 | 굴절 서비스(= front layer 반사 + ray-traced translucency) | `RefractionLumenTrace.hlsl`(물·유리·coverage 층이 만든 반사/굴절 job을 추적)이 경로의 끝을 반사 광선과 같은 방식으로 처리한다. ① 마지막 hit이 화면에 보이면 이전 프레임 색(언리얼 SampleSceneColorAtHit; 깊이 두께·법선 임계는 주 반사와 같은 값, 법선은 G-buffer가 아니라 hit 면의 광선 쪽 법선) — 수면이 카드 텍셀이 아니라 뷰가 보여 주는 조명을 비춘다. ② 매질 밖 구간의 머리카락 프록시(태양만: 스레드당 그림자 광선 1개 유지). ③ 경로 throughput의 최대 채널이 임계 아래면 끝낸다. ④ 결과 상한(노출 단위). ⑤ 음영을 구간 루프 밖으로 뺐다(루프 안에 그림자 광선 없음). | `reflection.lumen_refraction_scene_color_at_hit`(켬), `lumen_refraction_path_throughput_threshold` 0.001, `lumen_refraction_max_ray_intensity` 0(상한 없음) | `RefractionLumenTrace.hlsl`, `ReflectionSystem.cpp`(recordRefraction) |
| 10 | hit 규칙을 카드 프레임으로 | 카드 프레임이 128 B → 160 B(워드 28~39). hit 커널들의 루트 상수가 꽉 차서, 모든 hit이 같이 쓰는 값을 카드 프레임에 싣는다: far field 시작 거리, skylight leaking 색·거리·반사 몫, distant screen trace 구간·허용치·오프셋, 잎 투과 스위치. | — | `CardLayout.hlsli`, `CardFrame.hlsl`, `CardLighting.cpp`, `CardSet.h`(`CardHitRules`), `LumenHitIndirect.hlsli`(`lhiRules`) |
| 11 | skylight leaking | 언리얼의 LumenSkylightLeaking(색조, 전체 거리 포함). 화면 프로브 광선과 translucency volume 광선의 hit: 광선 방향의 하늘 radiance × 색 × min(hit 거리 / 전체 거리, 1). 반사 hit: hit 법선 쪽 하늘 radiance × 색 × 평균 알베도 0.25. radiance cache 광선에는 넣지 않았다(언리얼도 화면 프로브용 캐시에서는 끈다). 언리얼은 거칠기 0.3으로 흐린 하늘 큐브를 읽고, 여기는 하늘 radiance를 그대로 읽는다. **가림이 없는 빛이다 — 0보다 크면 닫힌 방이 낮에 더 밝다. 기본 0이고, furnace 규칙은 0에서만 성립한다.** | `lumen.skylight_leaking` 0, `skylight_leaking_tint` [1,1,1], `skylight_leaking_full_distance_m` 10, `skylight_leaking_reflection_average_albedo` 0.25 | `LumenHitIndirect.hlsli`, `LgTrace.hlsl`, `LumenTranslucencyVolumeTrace.hlsl`, `ReflectionLumenHit.hlsli` |
| 12 | 카드 없는 hit의 far field(반사·radiosity) | 카드 끝(300 m) 밖에서 volume도 irradiance 프로브도 답하지 않는 hit은 hit 법선 쪽 하늘 irradiance를 받는다. 화면 프로브·radiance cache·volume 광선에는 이미 있던 규칙이고, 반사와 카드 radiosity의 카드 없는 hit에만 빠져 있었다(거울 속 먼 지형이 태양만 받았다). | `lumen.radiance_cache_far_field`(기존, 켬) | `ReflectionLumenHit.hlsli`, `CardRadiosityTrace.hlsl` |
| 13 | distant screen traces | 반사의 월드 광선이 아무것도 못 맞혔을 때, 광선 끝에서부터 깊이 버퍼를 16걸음 선형으로 걷고(slope compare tolerance 2.0, 걸음 오프셋 = 화소 잡음 + bias), 맞으면 이전 프레임 색. 걷는 구간 = (최대 거리 2 km − 광선 길이). 기본 광선 길이가 10 km라 구간이 0이고 아무것도 걷지 않는다. | `reflection.lumen_distant_screen_traces`(켬), `_max_distance_m` 2000, `_depth_threshold` 2.0, `_step_offset_bias` 0 | `ScreenTrace.hlsli`(`sctDistantTrace`), `ReflectionLumenTrace.hlsl` |
| 14 | 잎을 지나는 빛(카드로 조명된 hit) | Foliage 재질의 hit이 카드에서 조명을 받으면 제 면의 (1 − t)만 남고 t 몫이 사라졌다. 카드는 양면 표면의 두 면을 각각의 카드에 갖고 있으므로, 반대쪽 면의 카드 조명을 법선을 뒤집어 읽어 투과 항으로 더한다. irradiance를 읽는 hit(화면 프로브, radiance cache, 반사): + base colour × (1 − metallic) / π × t × 반대쪽 irradiance. final 조명을 읽는 hit(radiosity, translucency volume): (1 − t) × 이쪽 + t × 반대쪽. 반대쪽 값도 카드 조명이라 그림자·가림은 카드와 같다. | `surface_cache.foliage_transmission`(켬) | `LumenHitIndirect.hlsli`(`lhiFoliageThrough`, `lhiFoliageFinal`), hit 커널 5개 |
| 15 | 수집 보간의 잎 화소 | 보간의 깊이 가중에는 잎용 배율(× 0.25)이 있었는데, 화소 적분과 적응 프로브 표시가 `foliage = false`를 넘기고 있었다. 이제 M의 재질 워드에서 화소의 클래스를 읽는다(Foliage·Subsurface). 그런 재질이 씬에 있을 때만 워드를 묶는다. | 없음(항상) | `LgIntegrate.hlsl`, `LgAdaptiveMark.hlsl`, `LumenGather.cpp` |
| 16 | 발광 광원의 카드 규칙 | 언리얼 bEmissiveLightSource: 카드 최소 면적 × 0.2, 카드 해상도 1까지 보임. 작은 조명 기구의 발광이 카드(발광 텍스처 포함)에 남는다 — 카드가 없으면 radiosity·volume 광선의 hit은 재질 상수(텍스처 없는 emissive 인자)로 발광을 읽는다. 재질이 발광을 시작하거나 멈추면 그 인스턴스를 다시 넣는다. | `surface_cache.mesh_cards_emissive_light_sources`(켬) | `MeshCardScene.cpp/.h`, `SurfaceCacheCards.cpp` |

#### 1.3.2 대조 결과 — 같은 것, 다른 것, 옮기지 않은 것

| 언리얼 | 상태 | 내용 |
|---|---|---|
| RayTracedTranslucency: MaxRayIntensity, PathThroughputThreshold | 9번에서 구현 | 상한은 기본 0(없음)으로 뒀다: job은 거울 방향 광선 1개이고 뒤에 디노이저가 없어, 상한은 반사된 밝은 부분을 어둡게만 한다(4절) |
| RayTracedTranslucency: MaxPrimaryHitEvents / MaxSecondaryHitEvents | 형식이 다름 | 여기서는 job이 전반사 횟수(2비트)를 갖고, 경로는 표면 4개까지(`REFRACT_SEGMENTS`). 값은 job을 만드는 쪽(물·유리 합성)이 정한다 |
| RayTracedTranslucency: SampleTranslucentReflectionInReflections, ForceOpaque, UseRayTracedRefraction | 안 옮김 | 반사·굴절 광선이 다른 반투명 메시를 불투명 표면으로 맞힌다(1.3.3) |
| front layer 반사: 거칠기 로브 표본 + 재구성·시간·양방향 필터 | 안 옮김 | job 형식(48 B: 원점, 방향, 매질, 흡수, 굴절률)에 거칠기가 없고 결과가 화소 격자가 아니라 job 배열이다. job을 만드는 쪽(`WaterSurface.hlsli`, `TranslucentComposite.hlsl`)과 형식을 같이 바꿔야 한다. 지금은 거울 방향 1개 |
| 수집: FullResolutionJitterWidth | 같음 | 폭 1 타일, 5~10 m 밖에서 절반, 옮긴 화소가 같은 평면일 때만(상대 평면 거리 가중 > 0.01) |
| 수집: 프로브 공간 필터(3패스, hit 각도 10°, 위치 가중 1000), StochasticInterpolation | 같음 | |
| 수집: 보간 가중의 bFoliage | 15번에서 고침 | 다른 점은 이것 하나였다 |
| radiance cache / far field | 구조가 다름 | 언리얼: HLOD로 만든 별도 TLAS + far field 카드(표면 캐시 조명이 있다). 가까운 광선이 놓친 뒤 far field 광선을 쏜다. 여기: 광선 씬 하나를 10 km까지 추적하고, 카드 끝(300 m) 밖의 hit은 태양(그림자 광선) + **가려지지 않은** 하늘 irradiance를 받는다 — 저장된 바운스가 없다. 먼 실내·협곡의 hit이 실제보다 밝을 수 있다 |
| 발광면이 광원이 되는 방식 | 같음 | 발광은 카드의 emissive → final 조명 → radiosity·수집 광선이 맞혀서 읽는다. 발광면을 향한 표본(next-event)은 언리얼 Lumen에도 없다. `MATERIAL_EMISSIVE_VISIBLE_ONLY`: 카드 캡처와 GI hit 커널 4개(화면 프로브, radiance cache, volume, radiosity)가 0으로 두고 반사 hit은 보인다 — 코드로 확인 |
| bEmissiveLightSource | 16번에서 구현 | 언리얼은 아티스트가 프리미티브에 표시, 여기는 발광 재질(뷰 전용 제외)을 가진 인스턴스 전부 |
| Skylight leaking | 11번에서 구현 | |
| 카드 직접광의 잎 투과 | 언리얼에 없음 | 언리얼은 카드 직접광에서 투과를 끄고(`bUseSubsurfaceTransmission = false`) 카드 알베도에 SubsurfaceColor를 더한다. 여기는 14번(hit이 반대쪽 카드를 읽음) |
| 스킨·애니메이션 메시의 카드 | 언리얼에 없음 | 1.3.4 |

#### 1.3.3 유리·물 메시와 Lumen 광선 — 고쳤다 (코드 작성·빌드 통과, 실행 안 함)

고치기 전: 재질 클래스가 인스턴스 마스크에 들어가지 않았고 any-hit는 알파만 봤다. Glass·Water 메시는 GI·반사·그림자 광선 모두에 불투명 표면이었다: 유리창 안쪽의 화면 프로브·radiosity 광선이 유리에서 멈췄고, 캐스터가 아닌 유리 뒤의 바닥은 뷰에서는 햇빛, 카드에서는 그늘이었다. 언리얼의 Lumen 광선은 반투명 메시를 보지 않는다(`RAY_TRACING_MASK_OPAQUE`, `SkipTranslucent`).

고친 것:

1. (00d39675, 주 세션) Glass/Water만으로 된 인스턴스를 GI·그림자 마스크에서 뺐다(`raytracing.see_through_translucent`). hit과 카드 텍셀에서 쏘는 그림자 광선은 캐스터만 본다(`RT_MASK_HIT_SHADOW` = `RT_MASK_SHADOW`, 뷰의 그림자 맵과 같은 집합).
2. (37713ee0) 그 마스크의 오류: "GI·SHADOW 비트만 뺀 전부"는 emitter 비트(0x4)를 남겼는데, 화면 프로브 광선과 radiance cache 광선의 마스크는 `RT_MASK_GI | RT_MASK_EMITTER`다 — 그 두 광선은 유리만으로 된 인스턴스를 여전히 맞혔다(translucency volume·radiosity 광선은 GI 비트만 써서 통과했다). 이제 그런 인스턴스는 반사 비트만 갖는다(그림자를 드리우면 `RT_MASK_SHADOW_TINT`도).
3. (37713ee0) 섞인 메시의 유리(창틀과 유리가 한 메시): Glass/Water 서브메시의 지오메트리를 모든 BLAS(메시, 스킨 인스턴스의 프록시와 원본, 런타임 메시)에서 non-opaque로 만들고, any-hit 셰이더가 see-through 광선에서 그 후보를 무시한다. 어느 광선이 see-through인지는 마스크에서 읽는다: `RT_MASK_REFLECTION`을 요구하지 않는 광선(GI 광선, 그림자 광선). `rtTraceClosest`·`rtVisible`이 payload의 pad 워드에 표시하므로 호출부는 바뀌지 않았다. inline RayQuery 커널 2개의 후보 루프도 같은 규칙이다(`rtCandidateStops`; 둘 다 해시 셀 표면 캐시의 커널이라 기본값에서 돌지 않는다). `ReflectionTraceInline`은 DXIL 한도에 붙어 있고 광선이 전부 반사 마스크라서 옛 any-hit를 그대로 둔다(`RT_NO_SEE_THROUGH`; 203,852 B, 한도 204,800 B).
4. (5968d4f2) hit의 그림자 광선이 유리가 남기는 빛만 받는다(`rtShadowTransmittance`): any-hit 셰이더가 payload에 광학 깊이를 더한다 — closest-hit 음영도 추가 광선도 없다. pane(양면 재질): 뷰 합성(`TranslucentComposite.hlsl`)의 T_p = (1 − F)² t / (1 − F² t²), F는 광선과 면이 이루는 각의 Fresnel, t는 base colour. 덩어리(한 면 재질): 면마다 (1 − F), 안을 지난 길이 × 흡수 계수(들어가는 면과 나오는 면의 거리 차로, 만나는 순서와 무관). 유리 지오메트리는 `NO_DUPLICATE_ANYHIT`으로 만든다. 유리만으로 된 캐스터는 그림자 마스크 밖이므로 이 광선은 `RT_MASK_SHADOW_TINT`를 더한다. 쓰는 곳: Lumen hit 라이브러리 6개(화면 프로브, radiance cache, translucency volume, 카드 radiosity, 반사, 굴절 서비스)의 태양·국소광 표본·머리카락 프록시 그림자 광선.

남은 차이:

- **카드 직접광**(표면 캐시 바운스의 주된 원천)은 텍셀·광원당 1비트를 저장해 색을 실을 수 없다: 유리를 투명(T = 1)으로 본다. 색유리 창을 지난 햇빛이 바닥에서 튕길 때 뷰는 물든 빛, 카드는 흰 빛이다. 실으려면 타일·광원마다 평균 투과색을 저장해야 한다(`CardDirectTrace`·`CardDirectStore`의 형식 변경).
- 뷰의 국소광 그림자 광선(`MegaLightsWorld.hlsli`, M 소유)은 유리를 그대로 통과한다(T = 1). `rtShadowTransmittance`는 모든 광선 라이브러리에 있다.
- 물(Water)은 그림자 광선을 그대로 통과시킨다. 틴트 텍스처는 읽지 않는다(재질 상수만).
- 덩어리 유리 안에서 시작하거나 끝나는 광선은 그 덩어리의 흡수를 받지 않는다.
- `FORCE_OPAQUE` 광선(`surface_cache.shadow_rays_opaque = true`; 기본 false)은 any-hit가 돌지 않아 섞인 메시의 유리에 막힌다.
- 재질 override로 불투명 서브메시가 유리가 된 인스턴스는 `FORCE_NON_OPAQUE`로 돌고 `NO_DUPLICATE_ANYHIT`이 없다: 투과가 두 번 세어질 수 있다.
- 뷰의 그림자 맵이 유리 캐스터를 깊이로 그리는지 투과로 다루는지는 이 작업에서 확인하지 않았다.

7절의 "로비 간접광 부족"의 원인이라고 말하지 않는다: 로비의 유리와 그 캐스터 플래그는 이 작업에서 보지 않았다.

#### 1.3.4 스킨·애니메이션 인스턴스의 카드 — 구현하지 않았다

지금: 그런 인스턴스의 hit은 카드가 없고, 제 위치에서 태양(그림자 광선 1개) + 국소광 표본 1개(그림자 광선 1개) + 간접 irradiance(translucency volume 또는 irradiance 프로브)를 제 재질(텍스처 포함)로 음영한다.

인스턴스마다 거친 카드(포즈된 경계 상자의 6면, 또는 뼈마다 캡슐)를 두는 안을 따졌다:

- 여기서는 hit이 제 재질로 음영하므로, 그런 카드가 가질 것은 알베도가 아니라 **irradiance뿐**이다 — 캡처(변형된 메시를 매 프레임 6방향으로 다시 그리기)가 필요 없다. 상자 6면의 중심에서 카드 직접광과 같은 방식(광원별 그림자 광선)으로 irradiance를 갱신하고, hit은 법선으로 6면을 섞어 읽으면 된다.
- 얻는 것: 그런 hit에서 그림자 광선 2개가 사라진다. 국소광 표본 1개의 잡음도 사라진다.
- 잃는 것: 조명의 위치 해상도. 지금은 hit 자리에서 정확한 그림자를 받는데, 상자 한 면에 값 하나면 반쯤 그늘에 선 캐릭터가 평균 밝기가 된다. 거울 속 캐릭터에서 바로 보인다. 캡슐로 쪼개면 덜하지만 자기 그림자는 여전히 없다.
- 표면에 붙은 카드(텍셀 ↔ 표면 점의 대응이 고정)는 변형 메시에서 성립하지 않는다: 포즈가 바뀔 때마다 대응이 바뀌어 radiosity의 시간 누적(4프레임)이 틀린 점의 값을 섞는다.

결론: 지금 방식이 품질은 더 높고 비용(그런 hit당 그림자 광선 2개)만 더 든다. 그 비용이 문제라는 측정이 없다 — 캐릭터 hit은 보통 광선의 작은 몫이고, 디스패치 상한은 이미 그 광선을 포함해 잡혀 있다. 군중 장면에서 비용이 측정되면 GI 광선(화면 프로브·radiance cache·radiosity)에만 "인스턴스 상자 6면 irradiance"를 쓰고 반사는 지금대로 두는 것이 후보다.

#### 1.3.5 기본값에서 닿지 않는 코드 — 삭제 결정을 위한 목록 (지운 것 없음)

조건(현재 `Config/quality` 기본값): `gi.lumen = true`, `gi.lumen_only = true`, `reflection.lumen = true`, `reflection.lumen_only = true`, `surface_cache.enabled = true`, `surface_cache.mesh_cards = true`. 목록은 C++의 패스 기록 조건과 셰이더의 `#include` 그래프를 읽어 만들었다(실행으로 확인한 것 아님).

**A. 월드 GI 해시 캐시와 그 화면 프로브** — `GiSystem::record`가 `recordLumen` 뒤에 돌아간다(`GiSystem.cpp` 828행 부근). 생성자도 캐시를 만들지 않는다.

- 커널(`Passes/GI/`, 39개): `GiAccFix`, `GiAccFold`, `GiAccumulate`, `GiAdmissionInit`, `GiAdmissionMark`, `GiAdmissionMerge`, `GiAdmissionPrepare`, `GiAdmissionPublish`, `GiAdmissionScan`, `GiAdmissionShift`, `GiAgeHistogram`, `GiBackgroundList`, `GiBegin`, `GiCarry`, `GiDetAnchors`, `GiDetBackground`, `GiDetClear`, `GiDetDigits`, `GiDetFold`(C++ 어디에서도 이름으로 부르지 않는다 — 스위치와 무관하게 죽어 있다), `GiDetResolve`, `GiEvict`, `GiGuide`, `GiIntegrate`, `GiInvalidate`, `GiLayerTemporal`, `GiPrior`, `GiProbeFilter`, `GiProbeGather`, `GiProbeMapOwners`, `GiProbeMaps`, `GiProbePlace`, `GiRehash`, `GiScreenFilter`, `GiScreenIrradiance`, `GiSelect`, `GiShift`, `GiTableClear`, `GiTrace`, `GiUpdateSetup`.
- 그 커널만 쓰는 헤더: `GiAccPool.hlsli`, `GiAdmission.hlsli`, `GiProbeWide.hlsli`, `GiSplitHistory.hlsli`. `GiCacheTile.hlsli`는 게이트 커널만 쓴다.
- 살아 있는 커널이 include하지만 그 분기가 돌지 않는 헤더: `GiInternal.hlsli`(`LgTrace.hlsl`만), `GiCache.hlsli`(`LumenRadianceCacheTrace.hlsl`, 그리고 다른 트랙의 `ShadeOpaque.hlsl`, `TranslucentComposite.hlsl`, `FxLayerSetup.hlsl`, `VolumeSetup.hlsl`, `WaterMedia.hlsl`, `WaterSurface.hlsli`), `ScreenProbes.hlsli`(`ShadeOpaque.hlsl`, `CoverageShade.hlsli`): `giCache`와 `view.screenProbes`가 무효일 때 읽지 않는 분기다. 지우려면 그 커널들의 분기를 먼저 지워야 한다. `GiScreenHistory.hlsli`, `GiScreenInputs.hlsli`, `GiSky.hlsli`, `GiSource.hlsli`는 이름과 달리 새 경로가 쓴다(살아 있다).
- 살아 있는 커널 안의 죽은 분기: `LgTrace.hlsl`, `LumenRadianceCacheTrace.hlsl`의 월드 캐시 읽기(`gi.lumen_hit_fallback`은 lumen_only에서 강제로 false).
- C++: `GiSystem.cpp`에서 `record`의 early return 뒤 전부, `recordScreen`, `recordSecondaryScreen`, `ensureAdmission`, `recordAdmission`, `ensureProbeHistory`, `ensureScreenHistory`, `readLookupStats`. 살아 있는 것은 설정 읽기와 `recordLumen`(`LumenGather.cpp`).
- 발행되지 않는 프레임 리소스: `giCache`, `giAccumulator`, `view.screenProbes`, `view.screenProbeMaps`. 읽는 쪽: `Shading/ShadingSystem.cpp`, `FX/ParticleLayer.cpp`, `Shadow/FroxelSystem.cpp`, `Volume/VolumePass.cpp`, `Reflection/ReflectionSystem.cpp`(다른 트랙 포함).
- 설정 키(`gi.toml`, 60개): `rays_per_frame`, `cache_entries`, `cache_octahedral_texels`, `screen_probe_spacing_px`, `near_occlusion_radius_m`, `near_occlusion_taps`, `screen_occlusion_history_frames`, `screen_occlusion_spatial`, `screen_filter_cells`, `screen_update_frames`, `cache_cell_angle_deg`, `cache_cell_min_m`, `cache_levels_max`, `cache_max_age_frames`, `jacobi_updates`, `history_updates_max`, `split_bounce_history`, `bounce_history_updates`, `bounce_split`, `bounce_split_updates`, `bounce_visibility`, `miss_closure`, `hit_oriented_lights`, `anchor_resample`, `history_window_rule`, `lighting_recent_frames`, `path_guiding`, `path_guiding_uniform_share`, `screen_filter_adaptive`, `screen_wide_filter`, `screen_wide_passes`, `screen_wide_sigma_lo`, `screen_wide_sigma_hi`, `screen_temporal_frames`, `anchor_centroid`, `hit_accumulator`, `hit_accumulator_pool`, `hit_accumulator_pool_slots`, `hit_accumulator_alpha`, `hit_accumulator_fine_scale`, `hit_accumulator_min_samples`, `hit_accumulator_levels`, `hit_accumulator_window`, `hit_accumulator_window_recent`, `hit_accumulator_cell_scale`, `hit_accumulator_frame`, `hit_accumulator_ratio`, `history_updates_max_static`, `update_tiers`, `young_update_share`, `parent_prior`, `parent_delta_initial`, `relight_restart`, `hit_light_footprint_scale`, `light_invalidation`, `hit_light_footprint`, `hit_update_share`, `hit_cell_footprint_scale`, `anchor_visibility`; 그리고 `lumen_hit_fallback`. `relight_frames_max`는 읽는 코드가 없다. 살아 있는 키: `lumen`, `lumen_only`, 나머지 `lumen_*`, `ray_length_m`, `experiment_disable`, `deterministic`. 죽은 키도 `GiSettings::fromQuality`가 값 검사는 한다(없으면 실패하는 키가 있다).

**B. 반사의 옛 경로** — `ReflectionSystem::record`의 `if (lumenOnly) … else { … }`의 else 쪽(`ReflectionSystem.cpp` 1454~1707행 부근)과 그 밖의 조건.

- 커널(`Passes/Reflection/`): `ReflectionArgs`, `ReflectionTrace`, `ReflectionSceneColorAtHit`, `ReflectionRayArgs`, `ReflectionLocalShadow`, `ReflectionTraceInline`, `ReflectionShadeRays`, `ReflectionShadow`, `ReflectionPenumbra`, `ReflectionCombine`(이상 else 쪽), `RefractionTrace`(`recordRefraction`의 lumen_only 아닌 쪽), `ReflectionAccumulate`(`reflection.lumen`이면 ray-reuse 파이프라인이 대신 돈다).
- `Passes/Reconstruct/LayerDenoise`, `LayerTemporal`, `LayerCompose`: 부르는 곳이 반사의 `layers` 경로뿐이고 `layers = reflection.layers && !lumen`이다.
- 그 커널만 쓰는 헤더: `ReflectionHit.hlsli`, `ReflectionRay.hlsli`, `ReflectionShade.hlsli`, `ReflectionValue.hlsli`.
- 살아 있는 것: `ReflectionBegin`, `Classify`, `Jobs`, `PlanarApron`, `HistoryClear`, `Hzb`, `ScreenTrace`, `LumenArgs`, `LumenTrace`, `Resolve`, `ReuseResolve`, `ReuseTemporal`, `ReuseFilter`, `RefractionArgs`, `RefractionLumenTrace`.
- 설정 키(`reflection.toml`): `batch_gi_corners`, `temporal_history_max`, `temporal_lobe_shift`, `hit_cone_lobes`, `hit_strict_read`, `hit_oriented_lights`, `hit_accumulator`, `lumen_surface_cache_view`, `lumen_surface_cache_view_component`, `layers`, `layer_filter`, `layer_history_frames`, `layer_residual_whole`, `layer_mirror_lobe`, `layer_whole_value`, `layer_cross_mode`, `layer_history_bound`, `layer_view`. `cache_lobe_half_angle_min_deg`, `g_sample_spacing_px`, `g_rays_per_sample`은 살아 있는 `ReflectionClassify`에 전달되지만 `reflection.lumen`에서는 K 임계·G 모드 분기가 돌지 않는다(`g_rays_per_sample`은 버퍼 크기 계산에는 여전히 들어간다). `exact_set_min_hits`, `planar_max_wave_slope_deg`는 읽는 코드가 없다.

**C. 해시 셀 표면 캐시** — `mesh_cards = true`면 버퍼를 만들지 않고, 그 패스 블록은 B의 else 안에도 있다(두 조건 모두로 막힌다).

- 커널(`Passes/SurfaceCache/`): `SurfaceCacheBegin`, `SurfaceCacheUpdate`, `SurfaceCacheLight`, `SurfaceCacheLightPairsSelect`, `SurfaceCacheLightPairsStore`, `SurfaceCacheLightPairsTrace`, `SurfaceCacheLightPairsTraceInline`. 헤더: `SurfaceCache.hlsli`, `SurfaceCacheLightPairs.hlsli`.
- C++: `Passes/Reflection/SurfaceCacheLightPairs.cpp` 전체, `ReflectionSystem.cpp`의 셀 캐시 버퍼와 `r.sc.*` 블록.
- 설정 키(`surface_cache.toml`): `entries_log2`, `max_unused_frames`, `capture_bounces`, `remainder_light`, `direct_stochastic`, `direct_stochastic_max_frames`, `direct_stochastic_min_sample_weight`, `direct_analytic`, `bilinear_read`, `debug_skip`, `direct_pairs`, `direct_pairs_inline`, `direct_shadow_inline`, `debug_count`, `base_cells`.
- 1.2의 7번(광원 함수)은 `SurfaceCacheLight.hlsl`과 `SurfaceCacheLightPairsSelect.hlsl`에도 넣었는데, 이 둘은 기본값에서 돌지 않는다. 도는 쪽은 `CardDirectStore`, `CardDirectCull`, `CardSelect`다.

**D. 옛 경로는 아니지만 기본값에서 돌지 않는 것**: `CardCaptureCluster.ps.hlsl`(`mesh_cards_capture_clusters = false`), `LgProbeTemporal.hlsl`(`gi.lumen_temporal_filter_probes = false`, 언리얼 기본), distant screen traces(구간 0), skylight leaking(0), `lumen.gather_temporal_reject_normal`(끔).

**E. 위 경로를 참조하는 시험·게이트**

| 파일 | 참조 |
|---|---|
| `Passes/GI/Tests/GiAnalytic.cpp` | 설정을 `gi.lumen=false`로 덮어써서(853행) 월드 캐시와 그 화면 프로브를 돌린다. 닫힌 식 시험 전부가 옛 경로 위에 있다. 시험 커널 `GiTestEval`, `GiTestPrimary`, `ProbeTileCompare` |
| `Passes/GI/Tests/Admission.cpp` | `GiAdmissionMark`·`Merge`·`Prepare`·`Publish`·`Scan`을 직접 돌린다. 시험 커널 `AdmissionFind` |
| `Passes/GI/Tests/ProbeMapsAtlas.cpp` | 옛 화면 프로브의 맵 아틀라스(`ScreenProbes.hlsli`, `GiCache.hlsli`). 시험 커널 `ProbeMapsAtlasCompare`, `ProbeMapsAtlasFill` |
| `Passes/GI/Gates/GiGate.cpp` | `readLookupStats`, 게이트 커널 `GiLookupStats`, `ProbeLookupBench`, `ProbeLookupBenchTile`(`GiCacheTile.hlsli`) |
| `Passes/Reflection/Tests/ReflectionAnalytic.cpp` | 월드 캐시 검사(`GiCacheScan`, `gi.settings().capacity`). 설정은 기본값을 읽는다 |
| `Passes/Reflection/Tests/PlanarMirror.cpp` | `resources.giCache`. 시험 커널 `PlanarTestView`가 `ReflectionHit`·`ReflectionShade`·`ReflectionValue.hlsli`, `SurfaceCache.hlsli`, `GiAccPool.hlsli`를 include한다 |
| `Passes/Shadow/Gates/RendererGate.cpp` | `giCache` 계열 리소스와 `reflection.g_rays_per_sample`(다른 트랙의 게이트) |

설정을 덮어쓰는 것은 `GiAnalytic.cpp`뿐이다. 나머지가 기본 설정(lumen_only)에서 무엇을 검사하게 되는지는 실행하지 않아 모른다.

### 1.4 2026-10-03 세 번째 구간 (브랜치 `w/cache`) — 광선 씬의 far field (코드 작성·빌드 통과, 실행 안 함)

GPU에서 돌린 것은 없다. 확인은 `build ok`뿐이다(이 구간의 커밋 넷 가운데 유리 둘은 한 번, far field는 한 번 빌드했다). 공유 any-hit·교차 셰이더가 커져서 한도에 가까운 광선 라이브러리: `GiTrace.SKY0.SPLIT1` 200,024 B, `ReflectionTraceInline.SKY0.JOB2.CORNERS1` 203,852 B(한도 204,800 B; 뒤의 것은 `RT_NO_SEE_THROUGH`·`RT_NO_FAR_FIELD`로 새 코드를 뺐다). 기본값은 **꺼짐**(`raytracing.far_field = false`)이고, 꺼져 있으면 광선 씬은 전과 같다(달라지는 것: 씬 인스턴스의 마스크에서 0x40 비트가 빠지고, Lumen 광선의 마스크에 그 비트가 붙는다 — 그 비트를 가진 인스턴스가 없으므로 맞는 것이 없다).

문제: 숲 씬은 정적 인스턴스 110만 개가 정적 TLAS 하나(212 MB)에 있고, 모든 Lumen 광선이 `gi.ray_length_m`(10 km)까지 그것을 걷는다. 언리얼은 광선 씬의 인스턴스를 거리·입체각으로 덜어내고(`r.RayTracing.Culling` 3, 반경 300 m, 각 1°), 먼 곳은 HLOD를 합친 far field TLAS로 본다.

구조(`RayScene.cpp` `buildFarField`, `selectNear`; `RayShaders.hlsli` `rtFarIntersect`; `HitFarField.hlsli`):

- **그룹**: 경계 반지름이 32 m 미만인 정적 인스턴스는 모두 (크기 등급, 셀)의 그룹에 든다. 등급 = 반지름의 2의 거듭제곱 구간(0.25 m부터 7등급), 셀 = 등급별 격자(16 m 이상, 등급 반지름의 4배 이상). 32 m 이상(지형, 건물)은 그룹이 없고 항상 TLAS에 있다.
- **near 판정**: 그룹은 그 셀 중심이 기준점(anchor)에서 등급 반경 안에 있으면 near다. 등급 반경 = min(300 m, 등급 최소 반지름 / tan 1°) + 셀 반대각선 + 재선택 거리. 그래서 언리얼 규칙으로 카메라 가까이에 있어야 할 인스턴스는 셀 안 어디에 있든, 카메라가 기준점에서 얼마나 벗어났든 near다. 등급별 값(기본 설정): 0.25 m → 14 m, 0.5 → 29, 1 → 57, 2 → 115, 4 → 229, 8 이상 → 300 (여기에 셀·재선택 여유 30~70 m가 붙는다).
- **near TLAS**: 정적 TLAS는 "항상 near인 것 + near 그룹의 멤버"만 갖는다. 카메라가 기준점에서 16 m 넘게 벗어나면 기준점을 카메라로 옮기고 다시 고른다(16 m 안에서 왔다 갔다 하면 다시 만들지 않는다). 정적 인스턴스가 바뀔 때와 원점 이동 때도 다시 고른다. 상한 262,144개(TLAS 크기): 넘으면 작은 등급부터 빠지고 로그에 센다. 고르기는 CPU에서 그룹 목록을 한 번 훑는다(인스턴스 110만 개가 아니라 그룹 수만큼의 거리 검사).
- **far 프록시**: 그룹마다 상자 하나 — 멤버 경계의 합, 불투명도 = 1 − exp(−멤버들의 평균 투영 면적 합 / 상자의 평균 투영 면적), 멤버 면적의 과반을 덮는 재질. 메시의 삼각형 면적은 메시마다 한 번 CPU에서 더한다(양면·Foliage 재질 메시는 면적의 1/2, 닫힌 메시는 1/4을 평균 투영 면적으로). 불투명도 2% 미만인 그룹은 상자가 없다(멀리 가면 그냥 없어진다). 상자 전부가 procedural AABB BLAS 하나, 동적 TLAS의 인스턴스 하나다(id `RT_INSTANCE_FAR`, 마스크 `RT_MASK_FAR`, 면광원과 같은 hit group — 새 셰이더 export가 없다).
- **교차**: 상자의 그룹이 near면 그 상자는 광선에 없다(같은 검사, 같은 기준점 — 프레임마다 헤더로 전달). 그래서 광선은 인스턴스나 그 프록시 중 하나만 만난다. 그 밖의 상자: 광선이 상자에 들어가는 점이 hit이고(상자 안에서 출발한 광선은 그냥 나간다), 불투명도만큼의 광선만 맞는다 — 어느 광선인지는 (상자, 광선 원점·방향)의 해시다.
- **hit 음영**: 상자의 들어간 면을 무광 표면으로, 색 = 대표 재질의 base colour × 텍스처 마지막 mip(평균색) × (1 − metallic). 빛 = 태양(그림자 광선 1개: 캐스터 + 다른 프록시) + 가려지지 않은 하늘 irradiance — 카드 끝 밖의 모든 hit에 쓰는 far field 규칙과 같다. Lumen 커널 6개가 그 hit을 처리한다(화면 프로브, radiance cache, translucency volume, 카드 radiosity, 반사, 굴절 서비스). 스레드당 광선 수는 그대로다(프록시 hit의 그림자 광선은 카드 없는 hit의 태양 그림자 광선 자리).
- 그룹과 상자는 소스 좌표(업로드된 씬의 좌표)에 있고, 원점 이동은 far 인스턴스의 변환이 따라간다(면광원 인스턴스와 같은 방식).
- 인스턴스 편집으로 `RayScene`이 다시 만들어질 때, 정적 집합이 같으면 그룹·상자·BLAS·near 상태를 이전 객체에서 물려받는다.

| 설정 | 기본 | 뜻 |
|---|---|---|
| `raytracing.far_field` | false | 전체 스위치 |
| `far_field_cull_radius_m` | 300 | 언리얼 Culling.Radius |
| `far_field_cull_angle_deg` | 1.0 | 언리얼 Culling.Angle |
| `far_field_rebuild_distance_m` | 16 | near 집합을 다시 고르는 카메라 이동 거리 |
| `far_field_near_instances_max` | 262144 | 정적 TLAS의 상한 |
| `far_field_proxy_size_m` | 16 | 그룹 셀의 최소 크기 |
| `far_field_proxy_max_radius_m` | 32 | 이보다 큰 인스턴스는 상자가 되지 않는다 |
| `far_field_proxy_min_opacity` | 0.02 | 이보다 옅은 그룹은 상자가 없다 |
| `far_field_proxies_max` | 1048576 | 상자 수 상한(불투명한 것부터 남긴다) |

언리얼과 다른 점:

- 언리얼의 far field는 HLOD로 합친 메시와 그 카드(표면 캐시 조명)다. 여기는 상자와 재질 평균색, 태양 + 하늘이다 — 프록시에는 저장된 바운스가 없다.
- 언리얼은 인스턴스마다 실제 반지름으로 판정한다. 여기는 등급의 최소 반지름과 셀 중심으로 판정한다(인스턴스와 프록시의 판정이 정확히 맞물리게 하려고). 등급 안의 큰 인스턴스는 언리얼보다 조금 일찍 프록시가 된다 — 여유(셀 반대각선 + 재선택 거리)가 그 반대 방향으로 30~70 m를 더한다.
- 큰 인스턴스(반지름 32 m 이상)는 거리와 무관하게 TLAS에 남는다(언리얼은 300 m 밖이면 HLOD로 넘긴다).
- 상자의 불투명도는 광선마다 확률로 적용된다(해시). 언리얼의 HLOD 메시는 형태가 있다.
- 태양 그림자 광선(카드 직접광, hit의 태양 광선, 머리카락 프록시)은 프록시를 본다: far로 넘어간 인스턴스가 상자의 불투명도만큼 그림자를 드리운다. 해시의 입력은 광선 원점(12.5 cm로 양자화)과 방향(1/1024)이라, 카드 텍셀의 태양 광선은 갱신마다 같은 답을 받는다(고정된 디더). 국소광 그림자 광선(카드, hit, 뷰의 MegaLights)은 프록시를 보지 않는다: far로 넘어간 인스턴스는 국소광 그림자를 드리우지 않는다(언리얼도 컬링된 인스턴스는 RT 그림자가 없다).

켜면 달라지는 화질(결정 대상): 반사에 비치는 먼 작은 물체가 상자가 된다. 뷰의 국소광 그림자(MegaLights의 그림자 광선)에서, 카메라에서 등급 반경 밖의 작은 캐스터(반지름 0.25 m면 약 45 m 밖)가 그림자를 잃는다. 광선 씬을 외부에서 프레임 없이 추적하는 시험은 far field를 켜면 정적 인스턴스를 보지 못한다(near 집합은 첫 프레임의 카메라로 정해진다).

**첫 실행에서 볼 것**:

1. 로드 로그의 `RayScene far field:` 줄 — 프록시 수, 그룹 수, 항상 near인 인스턴스 수, BLAS 크기, 만드는 데 걸린 시간(110만 인스턴스 분류는 로드 때 CPU 한 번).
2. `near set` 줄 — near 인스턴스 수, 상한을 넘어 빠진 수(0이 아니면 상한을 올리거나 각을 키운다), 고르는 CPU 시간.
3. 카메라가 16 m를 넘을 때마다 도는 `r.as.tlas.static` 패스의 GPU 시간(near TLAS 재빌드)과 그 프레임의 끊김.
4. 광선 패스 시간(`r.gi.lg.trace`, `r.gi.rc.trace`, `r.gi.ltv.trace`, `r.card.radiosity.trace`, `r.refl.lumen.trace`)을 `far_field = false`와 나란히. 정적 TLAS 메모리(로그의 TLAS static MB).
5. 프록시의 확률 불투명도가 프로브·거울에 만드는 잡음, 인스턴스가 프록시로 넘어가는 거리에서의 밝기 변화(카드 조명 → 태양 + 하늘).
6. 숲 바닥의 간접광이 `far_field = false`와 얼마나 다른지(먼 나무가 하늘을 가리는 양: 불투명도 식의 검증).
7. furnace 방: 프록시 hit은 가려지지 않은 하늘을 받는다(기존 far field 규칙). 닫힌 방 안에는 프록시가 없지만, 방이 반지름 32 m 미만의 메시들로 지어졌고 카메라가 300 m 밖에 있으면 방 자체가 상자가 된다.

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
| 소프트웨어 래스터(변 32 px 미만 클러스터) | 대역 A·그림자 뷰: 없음(메시 셰이더만). coverage 층: 8픽셀 이하 삼각형의 compute 래스터(`visibility.coverage_compute_raster`, 기본 꺼짐; 2026-10-03 코드·빌드만, 실행 안 함) | 작은 삼각형이 주 뷰와 그림자 뷰에서 하드웨어 셋업 비용을 냄 |
| DAG 안의 복셀 클러스터(잎·가는 형상도 계속 단순화) | 없음(가는 그룹은 말단, 밴드 C 런타임 없음) | forest_thin: 원본 삼각형 1.6억, visible 758만 요청(상한 100만) → 8.98 ms |
| 래스터 빈·앞→뒤 깊이 버킷 | 고정 리스트 8개. coverage 층(대역 B 리스트)만 깊이 버킷 4개 + 픽셀·타일 덮개(`visibility.coverage_depth_buckets`; 2026-10-03 코드·빌드만, 실행 안 함 — `UE6_WORKPLAN_KO.md` 2 (c)) | coverage 층이 가려진 조각을 저장(물가 61.8 %) — 버킷 A/B 뒤에 다시 잰다 |
| 클러스터 압축(양자화·스트립) | 무압축 | 메모리·대역폭 3~4배 [추정] |
| 페이지 스트리밍 + GPU 피드백 | 스트리머만 있고 연결 안 됨 | 전부 상주 |
| 그림자 뷰 HZB 가림 | 움직이는 캐스터: 정적 사본의 페이지 HZB로(2.2). 정적 캐스터: 2단계 — 유지된 굵은 페이지의 HZB를 추측으로, 1단계가 그린 것의 HZB로 확정(`shadow.vsm.static_occlusion_two_phase`; `UE6_WORKPLAN_KO.md` 8.3 (c)) — 코드 작성·빌드 통과, 실행 안 함 | 실행 전: 시간 미측정 |
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
| 국소광(큐브/스폿 페이지)의 정적/동적 분리와 HZB 가림 | 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.local_static_separate`; `UE6_WORKPLAN_KO.md` 8.3 (f)). 변화 판정은 광원 단위 | MegaLights 기본 설정에서는 S가 국소 페이지를 그리지 않아 해당 없음 |
| 반투명 캐스터의 색 투과(언리얼: 광선 추적 그림자의 `bTranslucentShadow`; VSM에는 없음) | 태양: 있음 — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.translucent_tint`; 유리 캐스터의 틴트 아틀라스 `VsmTint.hlsli`; 8.3 (f)). 얇은 캐스터의 투과 층(`VsmLayer.hlsli`)은 스칼라이고 채워지지 않는다(V의 coverage 모드 래스터가 없다) | 유리 아래가 검지 않고 유리 색으로 물든다(불투명 음영); 광선 hit·물·반투명 층의 태양 조회와 국소광 페이지에는 없음 |
| 프리미티브별 그림자 플래그(`bCastHiddenShadow` 등) | `scene::InstanceShadowOnly`, `scene::InstanceNoSelfShadow` — 코드 작성·빌드 통과, 실행 안 함 (8.3 (f)) | NoSelfShadow는 불투명 뷰의 태양 슬롯에만 |
| 굵은 레벨의 작은 캐스터 | 프록시(인스턴스당 사각형 하나, 청크 단위) — 코드 작성·빌드 통과, 실행 안 함 (`shadow.vsm.aggregate_small_casters`; 8.3 (f)). 언리얼에는 대응하는 장치가 없다(Nanite는 복셀 클러스터로 계속 단순화) | 레벨 텍셀보다 작은 정적 캐스터의 그림자가 밀도로 남는다 |

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
| 피사계 심도 | 옥타브 피라미드 적분(원판만, 전경·배경 분리 없음) (2026-10-03 2차: 조리개 구조 경로 코드 있음. 3차: squeeze·Petzval·경통과 매트 박스·depth blur 코드 있음, 2.3.1) | 전경 흐림이 초점면 물체 위로 번지지 않음, 보케 모양 없음 |
| 동적 해상도 | 없음. 내부 높이는 티어마다 고정(`output.render_scale`, `render_height_max`) (2026-10-03 3차: 컨트롤러 코드 있음, 기본 끔, 2.3.1) | 무거운 장면에서 프레임 시간이 예산을 넘어도 화소 수가 그대로임 |
| 게임이 읽는 프레임 시간 | 프레임 GPU 시간과 패스별 시간만 있음. 해상도 배율·그룹 합계 없음 (2026-10-03 3차: `UnxFrameGetStatistics` 코드 있음, 2.3.1) | 게임이 프레임 조절에 쓸 값을 한 번에 못 읽음 |
| 렌즈 투영 | 없음 (2026-10-03 2차: Panini 코드 있음, 2.3.1) | 넓은 시야각에서 화면 가장자리가 늘어남 |
| HDR 출력 | 종이 흰색 = 1 선형 값까지만 쓰고 인코딩은 호스트 몫 (2026-10-03 2차: scRGB·ST 2084 인코딩 코드 있음, 2.3.1) | 호스트가 직접 변환해야 함 |

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
| reprojection field | 내부 화소마다 벡터의 자코비안(같은 표면의 이웃 벡터, 깊이 가중)과, 양쪽이 다르게 움직이는 가장자리에서는 화소 안의 경계(깊이 가장자리를 양쪽 3화소 따라감)를 적는다. 이력 갱신은 출력 화소가 놓인 쪽의 벡터를 읽고 자코비안으로 그 화소 자리까지 옮긴다. 이력을 확대하는 재투영은 그만큼 이력 가중을 낮춘다. 언리얼의 hole filling(가려졌던 화소의 벡터를 가림체 벡터로 바꿈)은 2차 구간에 넣었다(아래 표). | `TsrDilate.hlsl`, `TsrUpdate.hlsl`, `Tsr.hlsli` · `output.upscale_tsr_reprojection_field`(epic·high 켬, performance 끔), `…_aa_speed` 0.125 | 코드 작성·빌드 통과, 실행 안 함 |
| history resurrection | 이력과 guide를 4칸 고리에 둔다. 두 칸은 프레임을 번갈아 받고 두 칸은 31프레임마다 한 장씩 보관한다(뷰 행렬·노출 포함, 원점 이동 반영). 보관 프레임의 guide를 카메라만으로 재투영해 입력과 같은 방법으로 재고, 직전 이력보다 0.1 이상 잘 맞는 화소는 보관 프레임의 이력을 쓴다. 맞는 화소가 없는 타일은 일찍 끝낸다. 스스로 움직인 물체는 맞지 않으므로 되살리지 않는다. | `TsrResurrect.hlsl`, `TsrDecimate.hlsl`, `TsrReject.hlsl`, `TsrUpdate.hlsl`, `Upscale.cpp` · `output.upscale_tsr_resurrection`(병합 때 끔으로 바뀜. 언리얼 기본값도 0) | 코드 작성·빌드 통과, 실행 안 함 |
| thin geometry | 커버리지 레이어의 얇은 조각이 화소를 덮는 비율을 이력으로 평균하고(언리얼은 재질 표시의 적중 횟수를 평균), 화소 폭의 깊이 선을 찾아, 그 화소들에서 셰이딩 거부의 clamp 상자를 이력 자신의 이웃 범위로 넓힌다. 레이어가 조각을 알려 주는 화소는 5 × 5 조건 없이 완화한다. 언리얼의 밝기 선 검출과 깜빡임 휴리스틱 연동은 2차 구간에 넣었다(아래 표). 거부 커널의 그룹 메모리는 26 KB가 됐다(한계 32 KB). | `TsrThin.hlsl`, `TsrReject.hlsl`, `TsrDecimate.hlsl` · `output.upscale_tsr_thin_geometry`(performance 끔), `…_error_multiplier` 200, `…_max_relaxation` 0.037 | 코드 작성·빌드 통과, 실행 안 함 |
| 출력보다 큰 이력 | 이력을 출력의 100~200 %로 두고 갱신을 그 해상도에서 한 뒤 Mitchell-Netravali 4 × 4로 출력에 내린다. 표본 수 상수는 이력 화소 기준으로 바꿨다. 4K 출력에서 200 %면 이력 한 칸이 253 MiB(2칸, resurrection이면 4칸)이고 갱신 패스의 화소 수가 4배다. | `TsrResolve.hlsl`, `TsrUpdate.hlsl`, `Upscale.cpp` · `output.upscale_tsr_history_percent`(병합 때 기본 파일 100으로 바뀜, high·performance 100) | 코드 작성·빌드 통과, 실행 안 함 |
| 반투명·레이어 속도 | 불투명 표면 위 레이어에 자기 벡터와 깊이를 준다: 화소를 다 덮는 유리(자기 삼각형의 움직임), 수면(그 깊이의 정지점, 파도는 안 따라감), 커버리지 레이어의 얇은 불투명 조각(화소의 1/3 이상일 때 가장 가까운 조각). 벡터가 없는 입자와 반투명 조각은 불투명도만큼 이력 clamp를 유지하고 이력을 짧게 한다. 추적하는 유리·수면 뒤 배경이 1 px 넘게 다르게 움직이면 clamp를 유지한다. 유리의 투과율은 보지 않으므로 맑은 유리 너머 배경은 시차가 있을 때 거부 쪽에 맡긴다. | `UpscaleMotion.hlsl`, `TsrReject.hlsl`, `Upscale.cpp` · `output.upscale_layer_motion` | 코드 작성·빌드 통과, 실행 안 함 |
| 모션 블러를 업스케일 뒤로 | 업스케일된 뷰는 출력 해상도에서 블러한다. 내부 표본의 벡터를 속도(길이·각도·깊이)로 펴고 16 × 16 타일마다 최단·최장을 구해, 최장 속도가 닿는 타일로 퍼뜨린 뒤, 출력 16 × 16 그룹을 분류해 모은다(정지: 복사 / 한 방향: 단순 평균 / 속도가 섞임: 깊이·도달 가중 / 탭 수보다 긴 블러: 2 × 2 화소당 한 번). 노출은 프레임 시각을 중심으로 양쪽이다(예전 경로는 [t − s·dt, t]). 최대 길이는 화면 폭의 5 %, 탭 16. 회전 단계는 2차 구간에 이 경로에도 넣었다(아래 표). 업스케일 없는 뷰는 예전 경로 그대로다. | `MotionFlatten.hlsl`, `MotionApply.hlsl`, `MotionBlur.cpp`, `ShadingSystem.cpp`(shade 끝부분) · `shading.motion_blur_after_upscale`, `…_max_percent`, `…_samples`, `…_half_res_gather` | 코드 작성·빌드 통과, 실행 안 함 |
| 샤픈·색수차 | 톤매퍼 샤픈(이웃 4개 평균과의 차, 밝은 곳 옆은 덜)과 장면 색 fringe(빨강·초록을 중심 쪽에서 읽음)를 최종 패스의 장면 색에 넣었다. | `PostFinal.hlsl`, `Post.cpp` · `shading.post_sharpen` 0, `post_fringe` 0, `post_fringe_start` 0 (기본 끔) | 코드 작성·빌드 통과, 실행 안 함 |
| 렌즈 플레어 | 블룸 사슬의 1/8 단계에서 문턱 넘는 밝은 부분을 조리개 모양(원 또는 다각형, 91탭)으로 펴고, 1/4 해상도에서 고스트 8개를 각자의 색과 중심 기준 배율로 더한다. 헤일로 고리는 우리가 덧붙인 것이고 기본 0이다. 값은 언리얼 포스트 프로세스 기본값으로 적었는데, 그 값이 있는 Engine 모듈이 참조 폴더에 없다. 패스 구조와 상수는 2차 구간에 대조했고, 기억에 의존한 값은 아래 표에 적었다. | `PostFlare.hlsl`, `Post.cpp` · `shading.post_lens_flare`(기본 끔), `post_lens_flare_*` | 코드 작성·빌드 통과, 실행 안 함 |
| 장면 기준 색 보정 | 전체·암부·중간·명부의 채도·대비·감마·게인·오프셋과 색온도·틴트를 톤 곡선과 함께 32³ LUT 하나로 굽고 최종 패스가 한 번 읽는다. 값이 바뀐 프레임에만 다시 굽는다(디스패치 4번). 기본값이면 LUT를 만들지 않고 그림이 그대로다. 게임은 `FrameContext::grading`으로 프레임마다 준다. 호스트 내보내기(`UnxFrameSetColorGrading`)는 2차 구간에 넣었다(아래 표). 언리얼은 gamut 확장 뒤에 보정하고, 여기서는 확장이 곡선 안에 있어 보정이 먼저다. | `PostGradeLut.hlsl`, `Post.cpp`, `FrameContext.h` · `shading.post_grading_*` | 코드 작성·빌드 통과, 실행 안 함 |

비교만 하고 바꾸지 않은 것: 필름 그레인(해시 그레인이 이미 동작한다. 언리얼의 암부·중간·명부별 세기와 그레인 텍스처는 없다), 자동 노출의 측광(백분위 사이의 로그 평균으로 언리얼과 같은 방식이다). 피사계 심도는 2차 구간에 언리얼 구조로 따로 넣었다(아래 표). 자동 노출의 적응에는 언리얼의 선형 구간을 스위치로 넣었다(`shading.exposure_linear_distance_ev`, 기본 0 = 지금처럼 지수 적응만. 언리얼 값은 1.5 — 코드 작성·빌드 통과, 실행 안 함).

#### 2026-10-03 2차 구간에 채운 것 (브랜치 `w/tsr`, lumen-ue6 3f55a3ef 위) — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 한 번도 돌리지 않았다. 아래는 코드에 적힌 내용이고, 그림과 시간은 재지 않았다. 병합 때 `output.upscale_tsr_resurrection`은 끔, `output.upscale_tsr_history_percent`는 기본 파일 100으로 바뀌었다(배치에서 재기 전까지). 위 표의 그 두 줄은 이 값으로 읽는다.

| 항목 | 내용 | 위치 · 스위치 | 상태 |
|---|---|---|---|
| 피사계 심도(조리개 구조) | 언리얼 DiaphragmDOF의 패스 구조를 내부 해상도에서, 업스케일 앞에 둔다. 렌즈는 예전 경로와 같은 값(조리개 지름, 초점 거리, 투영)을 받는다. 순서: 반해상도 색·착란원 반지름(2 × 2 중 가장 가까운 표면의 반지름) → 지터가 있는 뷰는 반해상도 시간 누적(카메라 움직임만으로 재투영, 3 × 3 범위 clamp) → 8 × 8 타일의 반지름 범위(flatten, 가장 넓은 흐림만큼 dilate) → reduce(거친 단계 최대 4개, 밝고 반지름 큰 화소는 스프라이트 목록으로 빼냄) → 전경(Jimenez 2층, 거울 표본으로 가려진 쪽 채움)·hole filling·약한 초점 이탈·배경(고리 단위 버킷, 가까운 표면이 뒤 흐림을 가림, 좁은 반지름이 섞인 타일은 안쪽을 다시 표집) 수집 → 전경·배경 3 × 3 중앙값 → 스프라이트를 메시 셰이더 쿼드로 두 레이어에 더함(배경 스프라이트는 수집된 반지름 평균·분산으로 가려짐) → 전해상도 recombine(반지름 3 미만은 전해상도에서 직접 수집하고 반해상도 결과 범위로 붙잡음). 조리개 모양은 원·직선 날·호 모양 날(32 × 32 표 두 장). 넣지 않은 것: anamorphic squeeze, Petzval, 경통·매트 박스 비네팅, depth blur 항, 알파 채널. 언리얼은 반해상도 누적에 속도 버퍼를 쓰는데 여기서는 그 시점에 속도가 없어 카메라 재투영만 쓴다. 예전 옥타브 경로는 `dof_diaphragm = false`와 테스트 산출물 요청 시 그대로 남는다. | `DiaphragmDof.cpp`, `Ddof*.hlsl`, `DdofCommon.hlsli`, `DepthOfField.cpp` · `shading.dof_diaphragm`(켬), `dof_diaphragm_*` | 코드 작성·빌드 통과, 실행 안 함 |
| TSR hole filling | 직전 프레임에 가려져 있던 화소는 자기 벡터 대신 가림체의 벡터(가림체 scatter에 실려 온 것)로 이력을 읽는다. 그 자리는 가림체 옆에 있던 배경이다. decimate가 갱신용 벡터 사본을 쓰고(마스크 비트 8), 갱신은 그 화소의 자코비안을 쓰지 않는다. guide 재투영과 거부 측정은 화소 자신의 벡터 그대로다. | `TsrDecimate.hlsl`, `TsrReject.hlsl`, `TsrUpdate.hlsl`, `Upscale.cpp` · `output.upscale_tsr_hole_filling`(켬) | 코드 작성·빌드 통과, 실행 안 함 |
| thin geometry의 밝기 선 | 깊이 선과 같은 규칙으로 화소 폭의 밝기 선(guide 공간 luma가 양옆보다 0.30 이상 밝음)을 찾는다. 찾은 선은 커버리지 이력의 한 비트로 남고(커버리지는 7비트가 됨), 선이 아닌 프레임마다 확률 0.1(얇은 지오메트리 안에서는 0.03)로 지워진다. 남은 선의 5 × 5 안에서 이번 프레임에 선이 아닌 화소는 가중 0.6으로 완화한다(클러스터 가중이 있으면 그쪽이 우선). 언리얼은 반투명 이전 색의 luma를 보고, 여기서는 최종 장면 색을 본다. | `TsrThin.hlsl`, `Upscale.cpp` · `output.upscale_tsr_thin_geometry_line_contrast` 0.30(0이면 끔), `…_line_fade_rate` 0.1, `…_line_fade_rate_inside` 0.03, `…_line_weight` 0.6 | 코드 작성·빌드 통과, 실행 안 함 |
| 깜빡임 휴리스틱 안의 thin geometry | 깜빡임 측정이 거부 패스와 같은 완화된 clamp 상자를 쓰고, 완화 화소에서는 이력 부호화 오차보다 작은 gradient도 깜빡임으로 센다(둘 중 하나가 7e-5보다 클 때). 그 화소의 오차는 주기 3배짜리 깜빡임의 누적 횟수에서 완전히 들어온다. 커널의 영역이 34 × 34가 되어 그룹 메모리는 32,368 B다(한계 32,768 B). | `TsrFlicker.hlsl`, `Upscale.cpp` · `output.upscale_tsr_thin_geometry_anti_flickering`(켬; high·performance 티어는 깜빡임 휴리스틱 자체가 꺼져 있다) | 코드 작성·빌드 통과, 실행 안 함 |
| 업스케일 뒤 모션 블러의 회전 단계 | 화면 중심에서 16 출력 화소가 넘는 카메라 회전은 벡터에서 회전 성분을 빼고(flatten에서 Q^T와 지터 없는 투영으로 계산, 뷰 모델은 자기 벡터 유지) 나머지만 모은 뒤, 그 결과를 출력 해상도의 회전 지도에서 호를 따라 평균한다. 노출은 수집과 같이 프레임 시각 중심이다. 지도는 RGBA32F이고 4K 출력에서 100 MB 안팎이다. 이 단계는 언리얼에 없는 우리 것이다. | `MotionBlur.cpp`, `MotionFlatten.hlsl`, `MotionRotation.hlsl` · `shading.motion_blur_after_upscale_rotation`(켬) | 코드 작성·빌드 통과, 실행 안 함 |
| Panini 렌즈 투영 | TSR 이력을 렌즈 투영된 그림으로 둔다. 이력 화소마다 렌더된(직선 투영) 그림에서의 자리를 구해 거기서 표본과 벡터를 읽고, 이전 자리는 그 프레임의 렌즈로 다시 투영해 이력을 읽는다(시야각은 고리 칸마다 보관, d·s가 바뀌면 이력을 새로 시작). 언리얼은 변위 표를 읽고 여기서는 투영식과 역식을 직접 계산한다. 그림은 렌더 폭에 맞춰 키우며, 중심 확대가 1~2 밖이면 언리얼처럼 꺼진다. 업스케일 뒤 모션 블러는 내부 해상도 자료를 렌즈를 거쳐 읽고 회전 단계는 뺀다. 렌즈가 켜지면 이력은 화면 추적의 이전 색으로 쓰지 않는다. TSR 경로의 업스케일된 뷰에서만 동작한다(네이티브 해상도·한 패스 업스케일에는 없다). 월드 기준 UI 위치 보정은 게임 몫이다. | `Lens.hlsli`, `TsrUpdate.hlsl`, `Upscale.cpp`, `MotionApply.hlsl`, `MotionBlur.cpp` · `output.lens_panini_d` 0(끔), `lens_panini_s` 0 | 코드 작성·빌드 통과, 실행 안 함 |
| HDR 출력 인코딩 | 경로를 끝까지 확인했다: 톤 곡선은 displayPeak까지 가고, 최종 패스는 종이 흰색 = 1인 선형 Rec.709 값을 RGBA16F에 쓰며, 스왑 체인용 인코딩은 호스트 몫으로 적혀 있었는데 이 저장소 안에는 그 인코딩 코드가 없다(스왑 체인은 Unity 소유). 그래서 최종 패스에 인코딩을 넣었다: 0 = 지금처럼, 1 = scRGB(1 = 80 cd/m²), 2 = Rec.2020 + ST 2084(출력 텍스처는 R10G10B10A2도 받음, 1코드 디더). 종이 흰색 휘도는 품질 파일(203 cd/m²)이나 호스트가 준다. 스왑 체인의 색 공간 설정(SetColorSpace1)과 HDR UI 합성은 여전히 호스트가 한다. ACES 출력 변환은 없다(곡선이 최고 휘도에서 끝난다). | `PostFinal.hlsl`, `Post.cpp`, `FrameContext.h`, `HostRenderer.cpp` · `output.hdr_encoding` 0, `output.hdr_paper_white_nits` 203, `UnxFrameSetDisplayEncoding` | 코드 작성·빌드 통과, 실행 안 함 |
| 렌즈 플레어 값 대조 | 참조 폴더의 `PostProcessLensFlares.cpp`·`.usf`와 대조해 확인한 것: 플레어 8개, 배율 (틴트 알파 − 0.5) × 7, 세기 × 블룸 세기, r + g + b 문턱, 반 배율 원판 마스크, 합성의 원판 마스크 둘, 보케 크기 = 플레어 뷰 폭의 %, `r.LensFlareQuality` 2. 문턱은 마스크를 곱한 색에 거는 것으로 고쳤다. 기억에 의존한 채 남은 값(참조 폴더에 없는 Engine 모듈의 `FPostProcessSettings` 기본값): LensFlareIntensity 1, LensFlareTint 흰색, LensFlareBokehSize 3, LensFlareThreshold 8, LensFlareTints 8개, 블룸 세기 0.675, 기본 보케 텍스처가 원판이라는 것. | `PostFlare.hlsl`, `Config/quality/shading.toml` | 코드 작성·빌드 통과, 실행 안 함 |
| 호스트 내보내기 | 게임이 실행 중에 바꾸는 값을 `UnxFrameSetFog`와 같은 방식(바꿀 때까지 유지, null이면 품질 파일)으로 내보냈다. `UnxFrameSetColorGrading`(색 보정), `UnxFrameSetPost`(노출 보정, 측광 범위 EV, 블룸 세기, 비네트, 모션 블러 양, 조리개 날 수·최대 개방 지름; NaN이면 품질 파일 값), `UnxFrameSetDisplayEncoding`(HDR 인코딩, 종이 흰색). DOF의 조리개·초점 거리는 기존 `UnxFrameSetLens`다. 헤더와 ABI 파일 끝에 붙였고, `HostRenderer`에는 패킷 필드·설정 함수·10비트 HDR 출력 허용을 넣었다. 관리 코드(C#) 쪽 바인딩과 크기 검사는 이 저장소에 없어 건드리지 않았다. | `UnravelNextHost.h`, `RendererAbi.cpp`, `HostRenderer.h/.cpp`, `FrameContext.h`(`PostSettingsDesc`), `Exposure.cpp`, `Post.cpp`, `MotionBlur.cpp`, `DiaphragmDof.cpp` | 코드 작성·빌드 통과, 실행 안 함 |

병합 때 `shading.dof_diaphragm`은 끔으로 바뀌었다(배치에서 보기 전까지. 돌아 본 것은 옥타브 경로다). 위 표의 첫 줄은 이 값으로 읽는다.

#### 2026-10-03 3차 구간에 채운 것 (브랜치 `w/tsr`, lumen-ue6 020db7af 위) — 전부 코드 작성·빌드 통과, 실행 안 함

GPU에서 한 번도 돌리지 않았다. 아래는 코드에 적힌 내용이고, 그림과 시간은 재지 않았다. 전부 기본값에서는 꺼져 있거나(동적 해상도, 렌즈 모양) 조리개 DOF 경로 안에 있다(그 경로가 기본 끔).

| 항목 | 내용 | 위치 · 스위치 | 상태 |
|---|---|---|---|
| 동적 해상도 컨트롤러 | 업스케일되는 주 뷰의 내부 높이를 GPU 프레임 시간에 맞춘다. 프로파일러가 돌려주는 완료 프레임의 시간을, 그 프레임을 렌더한 해상도 비율과 짝지어 이력에 넣는다(시간은 몇 프레임 늦게 오므로 프레임마다 결정을 보관). 프레임마다 제안 = 비율 × √(목표 / 시간), 목표 = 예산 × (1 − 여유 %), 최근 16프레임을 프레임당 0.9로 가중. 최근 2프레임이 연속으로 예산을 넘으면 그 제안을 바로 적용하고 이력을 비운다. 그 밖에는 8프레임마다, 2 % 넘게 다를 때만 바꾸고, 올릴 때는 제안의 0.9만큼만 간다. 높이는 출력의 50~100 % 안, 고정 설정의 높이 이하. 우리 것 둘: 높이는 72줄 단위로 내림(한 번 바뀔 때마다 아래 줄의 다른 시스템 이력이 다시 시작하므로 굵게), 화소 수와 무관한 고정 시간을 빼고 계산하는 항(`…_fixed_ms`, 기본 0. 로비 측정 모델은 3.3 ms + 4.1 ms/백만 화소). 고정 설정이 네이티브 해상도인 뷰에는 적용하지 않는다. 4K 출력에서는 고정 높이 1080줄이 이미 50 %라 `min_percent`를 낮춰야 움직일 범위가 생긴다. **규칙과 기본값은 기억에 의존했다**: 언리얼의 `DynamicResolutionState`·`r.DynamicRes.*`는 Engine 모듈에 있고 그 모듈이 참조 폴더에 없다. | `DynamicResolution.cpp/.h`, `ShadingTrack.cpp`, `Tracks.h`(`tracks::dynamicResolutionHeight`, `dynamicResolutionStatus`), `FrameRenderer.cpp`(`setupUpscale`), `Stubs/TrackM.cpp` · `output.dynamic_resolution_target_ms` 0(끔), `…_headroom_percent` 10, `…_min_percent` 50, `…_max_percent` 100, `…_history_frames` 16, `…_frame_weight` 0.9, `…_change_period_frames` 8, `…_change_threshold_percent` 2, `…_increase_blend` 0.9, `…_over_budget_frames` 2, `…_step_lines` 72, `…_fixed_ms` 0 | 코드 작성·빌드 통과, 실행 안 함 |
| 크기가 바뀔 때 M의 이력 | 다시 시작하지 않는다. 내부 해상도 텍스처(guide 고리, 깜빡임·thin 커버리지 이력, 보관하는 장면 색, DOF 반해상도 누적)는 칸마다 그 칸을 쓴 프레임의 크기를 갖는다. 이번 프레임이 쓰는 칸만 이번 크기로 다시 만들고, 읽는 쪽은 이전 텍스처 자신의 크기로 UV 재투영한다(guide는 Catmull-Rom, 나머지는 최근접·bilinear). 업스케일 출력과 그 이력은 출력 해상도라 그대로다. 이전 프레임 지터는 이번 내부 화소 단위로 환산한다. 고정 최대 크기 텍스처 + 뷰포트 방식(언리얼의 방식)은 쓰지 않았다: `g_viewWidth/Height`를 읽는 HLSL이 74개, 뷰 크기 텍스처에 GetDimensions를 쓰는 파일이 12개쯤, UV로 표집하는 파일이 46개이고 대부분 다른 트랙 파일이라 전부 고쳐야 한다. | `Upscale.cpp`(`UpscaleState`), `TsrDecimate.hlsl`(P[3].w, P[4].w), `DiaphragmDof.cpp`, `DdofStabilize.hlsl`, `FrameRenderer.cpp/.h` | 코드 작성·빌드 통과, 실행 안 함 |
| DOF 렌즈 모양 | squeeze(보케가 가로로 1/squeeze: 넓은 수집의 표본 위치, 약한 초점 이탈 수집의 거리, 스프라이트 쿼드·교차·에너지, squeeze < 1일 때 타일 도달 거리), Petzval(수집 커널마다·스프라이트 블록마다 행렬 하나. 중심에서 멀수록 중심 방향으로 눌림, 음수면 접선 방향. 제외 상자와 모서리 반지름), 경통과 매트 박스(스프라이트의 보케를 경통 끝 평면에서 자름: 조리개를 채운 광선 다발이 경통 끝에서 축에서 벗어난 원이 되고 경통 테두리와 깃발 3개의 끝 선이 그것을 자른다), depth blur(초점과 무관하게 거리로 흐림: 반지름 × (1 − 2^(−거리/기준 거리)), 렌즈 쪽 반지름보다 클 때). 언리얼과 다르게 한 것: Petzval의 축은 화소 방향으로 잡는다(언리얼은 정규화된 정사각형에서 잡아 넓은 화면에서 타원이 기운다), 경통에서 다발이 좁아지는 양은 경통 길이로 계산한다(언리얼 식에는 그 자리에 경통 반지름이 있다), 전경은 조리개 위치를 뒤집는다(언리얼은 전경·배경을 같은 쪽으로 자른다), 비네팅 자료는 목록에 화소마다 저장하지 않고 메시 커널이 블록마다 계산한다, 깃발의 roll은 그림 오른쪽에서 위쪽으로 잰다. 넣지 않은 것: dynamic radius offset, 알파 채널, 호스트 프레임별 값(품질 파일 키만 있다). **기본값은 기억에 의존했다**(Engine 모듈의 `FPostProcessSettings`): squeeze 1, Petzval 0·falloff 1·상자 0, 경통 반지름 5 cm·길이 0, 깃발 없음, depth blur 반지름 0·1 km. | `DdofCommon.hlsli`(`ddofCoc`, `ddofPetzval`), `DdofGather.hlsl`, `DdofRecombine.hlsl`, `DdofReduce.hlsl`, `DdofSetup.hlsl`, `DdofScatter.hlsli/.ms/.ps.hlsl`, `DiaphragmDof.cpp` · `shading.dof_diaphragm_squeeze` 1, `…_petzval` 0, `…_petzval_falloff` 1, `…_petzval_box_x/y` 0, `…_petzval_box_radius` 0, `…_barrel_radius` 0.05, `…_barrel_length` 0, `…_matte_box_0..2_roll/pitch/length` 0, `…_depth_blur_radius` 0, `…_depth_blur_km` 1 | 코드 작성·빌드 통과, 실행 안 함 |
| DOF 반해상도 누적의 속도 | 업스케일의 벡터 패스(`m.upscale.motion`)를 함수로 떼어(`upscaleMotion`) DOF 누적이 요청하면 업스케일보다 먼저 기록하고, 업스케일은 같은 텍스처를 쓴다. 누적은 화소 주변 8개 중 가장 가까운 표면을 벡터가 가리키는 깊이(레이어가 있으면 레이어 깊이)에서 찾아 그 화소의 벡터로 이력을 읽는다. 움직이는 물체·변형·레이어가 자기 움직임으로 따라간다. 끄면 예전처럼 카메라 재투영만 쓴다. | `Upscale.cpp/.h`(`UpscaleMotion`, `upscaleMotion`), `DdofStabilize.hlsl`, `DiaphragmDof.cpp` · `shading.dof_diaphragm_prefilter_velocity`(켬) | 코드 작성·빌드 통과, 실행 안 함 |
| `UnxFrameGetStatistics` | 읽기 전용 선택 export. 구조체 하나(480 B, version 1)에: GPU가 끝낸 마지막 프레임의 GPU 시간과 그 프레임의 내부 해상도, 가장 최근에 기록한 프레임의 출력·내부 해상도와 배율, 동적 해상도 상태(동작 여부, 예산, 마지막 결정에 쓴 가중 시간, 높이 범위), 그 프레임의 패스를 그룹(패스 이름의 첫 '.' 앞: v, m, r, s, w, fx, hair …)별로 합한 시간(큰 것부터 최대 16개, 넘으면 마지막이 other). 이미 있던 것: `UnxFrameStatsLatest`(프레임 GPU 시간), `UnxFramePassTimingsLatest`(패스별). 새로 생긴 것은 해상도·컨트롤러 상태·그룹 합계다. 관리 코드(C#) 바인딩은 이 저장소에 없어 건드리지 않았다. | `UnravelNextHost.h`, `RendererAbi.cpp`(둘 다 파일 끝), `HostRenderer.h/.cpp`(`FramePacing`, 슬롯별 내부 크기) | 코드 작성·빌드 통과, 실행 안 함 |

**해상도가 한 단계 바뀔 때 아직 다시 시작하는 시스템** (다른 트랙 파일이라 적어만 두고 고치지 않았다. 전부 "저장한 크기 ≠ 뷰 크기면 다시 만들고 이력 없음으로 표시"하는 방식이고, 재투영 커널이 이전 크기 = 현재 크기를 가정하고 정수 좌표로 읽는다. 줄 번호는 lumen-ue6 020db7af 기준):

| 시스템 | 다시 시작하는 곳 | 한 단계에서 보이는 것 |
|---|---|---|
| GI 화면 프로브·수집 이력 | `GiSystem::ensureScreenHistory`(GiSystem.cpp:411), 레이어 이력(:624), 프로브 가림 이력 `ensureProbeHistory`(:386), `ensureLumen`(LumenGather.cpp:33: 텍스처 12장, 유효 프레임 0, 확률 보간 없이 4프레임), 잎 뒷면 이력(:152) | 간접광이 몇 프레임 노이즈에서 다시 수렴 |
| 반투명 GI 볼륨 | `TvState::ensure`(LumenTranslucencyVolume.cpp:136), 격자 크기가 바뀔 때 | 볼륨 이력 없음 |
| 짧은 거리 AO | `ShortRangeAoState::ensure`(LumenShortRangeAO.cpp:41) | AO 이력 없음 |
| MegaLights 시간 누적 | `MegaLightsState::ensure`(Shading/MegaLights.cpp:60) | 국소광 노이즈 |
| 반사 이력 | `ReflectionSystem::ensureHistory`(ReflectionSystem.cpp:580), 이력 지움 패스 | 반사 노이즈 |
| 프록셀: MegaLights 볼륨·안개 볼륨 | `SampledLocalState`(FroxelSystem.cpp:1227), `FogState::ensure`(:1009), 격자 크기가 바뀔 때. 안개 셀은 16 px 고정, 프록셀 타일은 720줄 이하 24 px·그 위는 각도 기준이라 720줄 위에서는 약 30줄마다 `froxelTilePx`가 바뀌고 대기 LUT도 다시 만든다(AtmosphereSystem.cpp:338, :534) | 안개·공기 이력 없음 |
| HZB | `ensureHiz`(VisibilityTrack.cpp:773) | 한 프레임 가림 컬링 없음(그림은 같고 시간이 늘어남) |
| 커버리지 레이어 이력 | `ensureCoverage`(VisibilityTrack.cpp:1332) | 초기화 패스, 재투영 이력 없음 |
| 구름 | CloudSystem.cpp:126 | 한 프레임 전체 march |
| 렌더 그래프 계획 | 텍스처 크기가 계획 키에 들어 있다(RenderGraph.cpp:268, 캐시 8개) | 새 크기마다 계획 컴파일. 72줄 단위면 576~1080줄 사이가 8가지라 캐시에 들어간다 |

다시 시작하지 않는 것: 업스케일 출력 이력, guide 고리, 깜빡임·thin 커버리지 이력, 보관 장면 색, DOF 누적(위 표), 그림자(화면 크기 이력 없음. 페이지 레벨은 화소 크기를 따라 옮겨 간다), 머리카락·물(뷰 크기 상태 없음), 월드 공간 캐시·radiance cache·카드 아틀라스·VSM 페이지. 다른 시스템을 다시 시작하지 않게 하려면 각 시스템이 M처럼 칸마다 크기를 갖고 UV로 읽거나, 전부 고정 최대 크기 텍스처 + 뷰포트로 가야 한다. 그 전까지는 단계가 굵고(72줄) 드물어야 한다(8프레임 간격, 2 % 문턱).

### 2.4 작업 순서와 현재 위치

1. Lumen 마무리, TSR 구조, 톤 파이프라인·블룸·비네트 — 코드 완료, GPU에서 돌았다(6절).
2. 첫 실행에서 확인된 것 수정 — 완료(6.2).
3. 그 뒤 넣은 것: 국소 노출, MegaLights 화면 추적, 카드 없는 hit의 직접광(radiosity·translucency volume까지) — 두 번째 실행에서 돌았다.
4. 남은 것(순서는 6.4의 시간표와 6.3의 그림에서):
   - **간접광 에너지**: 로비에서 경로 추적 기준 대비 벽 0.26~0.33, 바닥 0.16(6.3). 원인 미확정. 추측으로 고치지 않는다(6.5).
   - **성능**: 4K 17.7~18.7 ms, 목표 6.06 ms(6.4). 같은 표본 수로는 닿지 않는다 — 내부 해상도와 표본 수는 사용자 결정(4절).
   - 반사 2×2 다운샘플 채택 여부, 표면 캐시 피드백(코드는 1.2에 있음, 실행 안 함), 서브서피스(재질 파라미터가 먼저 필요), 그림자 페이지 구조(정적/동적 분리, 굵은 페이지, HZB), 지오메트리(소프트웨어 래스터, 압축, 스트리밍 연결), 옛 경로 코드 삭제.
   - TSR·후처리(M)는 코드가 다 들어갔고 전부 실행 전이다(2.3.1의 2026-10-03 표 둘): resurrection, reprojection field, thin geometry(밝기 선·깜빡임 연동 포함), hole filling, 출력보다 큰 이력, 레이어 속도, 업스케일 뒤 모션 블러와 회전 단계, 조리개 구조 DOF, Panini, HDR 인코딩, 색 보정 LUT, 렌즈 플레어·샤픈·색수차, 호스트 내보내기. 다음은 배치에서 돌려 그림과 시간을 재는 것이다 — 먼저 볼 것: 조리개 DOF와 예전 경로의 A/B(`shading.dof_diaphragm`), resurrection과 이력 200 %의 비용, 깜빡임 커널의 그룹 메모리(한계까지 400 B).
   - 2026-10-03 3차(2.3.1의 세 번째 표, 전부 실행 전): 동적 해상도 컨트롤러(기본 끔), 크기가 바뀌어도 이어지는 M의 이력, DOF 렌즈 모양(squeeze·Petzval·경통과 매트 박스·depth blur)과 누적의 속도 벡터, `UnxFrameGetStatistics`. 배치에서 먼저 볼 것: 동적 해상도를 켜고 한 단계 바뀌는 프레임의 그림(다른 시스템 이력이 다시 시작하는 것이 얼마나 보이는지 — 2.3.1의 목록), 컨트롤러가 예산 근처에서 오르내리지 않는지, `…_fixed_ms`에 측정 모델의 3.3 ms를 넣었을 때와 0일 때의 차이.
   - 동적 해상도에서 남은 것: 다른 트랙의 화면 이력이 단계마다 다시 시작한다(2.3.1 목록. 각 트랙이 칸별 크기 + UV 재투영이나 고정 최대 크기 텍스처로 바꿔야 한다). CPU 시간 예산과 프레임 시간 전체(GPU + 표시 간격)에 대한 조절은 없다(GPU 시간만 본다).
   - M에서 코드로도 없는 것: DOF의 dynamic radius offset·알파 채널·렌즈 모양의 호스트 프레임별 값, thin geometry 커버리지의 반투명 비트, TSR 밖(네이티브 해상도·한 패스 업스케일)에서의 Panini, ACES 출력 변환, 관리 코드 쪽 새 내보내기 바인딩.

## 3. 언리얼과 다르게 둔 점

1. 카드 캡처는 클러스터 래스터가 아니라 메시의 원본 삼각형을 그린다. 클러스터 컷으로 그리는 경로를 썼다(`surface_cache.mesh_cards_capture_clusters`, 기본 끔; 코드 작성·빌드 통과, 실행 안 함): V의 래스터 서비스 한 번으로 깊이와 재질을 그리고, 법선·접선은 서비스가 주는 보간된 꼭짓점 값이다(1.2의 2번).
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
19. 카드로 조명된 잎 hit의 투과(코드 작성·빌드 통과, 실행 안 함): 반대쪽 면의 카드 조명을 읽어 더한다(`surface_cache.foliage_transmission`). 언리얼의 카드에는 투과가 없고 카드 알베도에 SubsurfaceColor를 더한다.
20. skylight leaking(코드 작성·빌드 통과, 실행 안 함): 언리얼은 거칠기 0.3으로 흐린 하늘 큐브, 여기는 하늘 radiance 그대로. 기본 0.
21. distant screen traces(코드 작성·빌드 통과, 실행 안 함): 언리얼은 광선 씬의 컬링 반경에서 시작, 여기는 광선의 끝(`gi.ray_length_m`)에서 시작. 기본 광선 길이에서는 돌지 않는다.
22. 굴절 서비스(front layer 반사 + ray-traced translucency의 여기 형태): job은 거울 방향 광선 1개이고 디노이저가 없다. 언리얼의 front layer 반사는 거칠기 로브를 표본해 반사 디노이저를 지난다(1.3.2).
23. 발광 광원 카드 규칙의 대상: 언리얼은 아티스트가 표시한 프리미티브, 여기는 발광 재질(뷰 전용 제외)을 가진 인스턴스 전부(코드 작성·빌드 통과, 실행 안 함).
24. 유리·물 메시는 GI·그림자 광선이 통과한다(코드 작성·빌드 통과, 실행 안 함; 1.3.3). 언리얼과 다른 점: hit의 그림자 광선은 유리의 투과색을 받는다(언리얼의 Lumen 그림자 광선은 반투명을 그냥 통과). 카드 직접광은 유리를 투명으로 본다.
25. far field: 광선 씬 하나를 10 km까지 추적하고, 카드 밖 hit은 태양 + 가려지지 않은 하늘 irradiance. 언리얼은 HLOD TLAS와 far field 카드의 표면 캐시 조명(1.3.2).
26. 광선 씬의 인스턴스 컬링과 far field 프록시(`raytracing.far_field`, 기본 끔; 코드 작성·빌드 통과, 실행 안 함): 언리얼의 컬링 규칙(300 m, 1°)을 크기 등급과 셀 단위로 적용하고, 먼 인스턴스는 그룹마다 확률 불투명도를 가진 상자 하나로 대신한다. 언리얼은 HLOD 메시와 그 카드(1.4).

## 4. 품질을 내주는 값(언리얼 기본값에서 시작, 사용자 결정 대상)

- `surface_cache.radiosity_max_ray_intensity = 40`
- `surface_cache.radiosity_max_frames_accumulated = 4`
- `surface_cache.shadow_rays_opaque = false`(언리얼 기본은 true: 알파 마스크 무시)
- `gi.lumen_max_ray_intensity`, `reflection.lumen_max_ray_intensity = 40`, `reflection.lumen_max_roughness = 0.4`, `reflection.lumen_ggx_sampling_bias = 0.1`
- `reflection.lumen_max_roughness_to_trace_foliage = 0.2`(언리얼 기본: 잎·피부 화소는 거칠기 0.2 이상에서 전용 반사 광선 없음. 2026-10-03 추가, 실행 안 함)
- `surface_cache.radiosity_min_trace_distance_m = 0.10`(언리얼 기본: 10 cm 안의 radiosity hit은 빛 0 — 구석이 조금 어두워진다. 2026-10-03 추가, 실행 안 함)
- `reflection.lumen_refraction_max_ray_intensity = 0`(상한 없음. 언리얼: front layer 반사 40, ray-traced translucency 1000. 디노이저 없는 거울 광선이라 상한은 밝은 반사를 어둡게만 한다. 2026-10-03 추가, 실행 안 함)
- `reflection.lumen_refraction_path_throughput_threshold = 0.001`(언리얼 기본. 그보다 약한 경로는 끊는다. 2026-10-03 추가, 실행 안 함)
- `lumen.skylight_leaking = 0`(언리얼 기본. 올리면 실내가 밝아지는 대신 가림 없는 빛이 샌다 — furnace 규칙이 깨진다. 2026-10-03 추가, 실행 안 함)
- `raytracing.far_field = false`(켜면 먼 작은 인스턴스가 반사에서 상자가 되고, 뷰의 국소광 그림자에서 먼 작은 캐스터가 빠진다 — 광선 시간과 정적 TLAS 212 MB를 줄이는 값이다. `far_field_cull_angle_deg = 1`, `far_field_cull_radius_m = 300`은 언리얼 기본. 2026-10-03 추가, 실행 안 함)

## 5. 실행 방법

`powershell -File Tools\Verify\Run-Ue6Final.ps1 [-Only bt_lobby] [-Resolutions 1080p,1440p,4K] [-Layers gi,refl] [-SkipPictures] [-SkipTimings] [-Out DIR]` (GPU lock의 `HOLD`가 있으면 멈춘다. 스크립트는 `HOLD`를 지우지 않는다.)

- 장면: `C:\Users\USER\UnravelNext-refl\Cache\ReflJudge\scenes`의 bt_bath, bt_lobby, bt_lounge, te_lounge. 게임 경로(내부 해상도 → TSR), 자동 노출, `gi.deterministic=true`.
- 그림: **600프레임 정지**(월드 공간 캐시가 플레이 중처럼 찬 상태) → f600에서 90° 돌린 시야로 컷 → 이후 20°/s 회전. f599(컷 전), f600·f601·f603(컷 직후), f615·f660·f719(회전 중). `pfm_to_png.py`가 국소 노출 + 필름 곡선을 거쳐 PNG로 바꾼다(블룸·비네트·그레인은 없다).
  - 첫 실행은 정지 60프레임이었다. 콜드 스타트 직후라 카드 조명이 덜 찬 상태였고(같은 조건 두 실행의 f59에서 바닥 GI가 18배 달랐다), 그래서 600으로 바꿨다.
- 시간: 회전 600프레임, 정지 600프레임의 패스별 GPU 시간(`timing_*` 폴더; `Tools\Verify\pass_times.py`로 요약). 시간 실행에는 DRED를 켜지 않는다.
- 게이트가 실패로 끝나도(S 오류 비트 등) 나머지 실행은 이어지고 끝에 실패 목록을 낸다. 장치 제거가 로그에 보이면 즉시 멈춘다.
- 로비는 경로 추적 기준 영상이 있다(`UnravelNext-refl\Cache\Reference\lobby\host_ev4_480x270_4096_…`, 호스트 카메라, 4096 spp, EV 4). `Tools\Verify\ref_blocks.py`가 4×3 블록 평균 밝기 비를 낸다. 한계: 기준은 클리어코트·공기·햇빛 집광이 없고 로비 한 시점뿐이다(바닥은 코팅 대리석이라 수치를 그대로 믿기 어렵다).
- `Tools\Verify\Run-Ue6Still.ps1 -Name X -Set k=v,…`: 정지 카메라 한 번 + 기준 대비 블록 비(설정 하나를 바꿔 볼 때).
- `Tools\Verify\Run-Ue6Batch.ps1`의 `ab` 묶음(`-Variants ab`, `-Ab <묶음>`): 2026-10-03 이후 실행 없이 쓴 스위치를 하나씩 기본값과 견준다 — 변형당 캡처 한 프레임(비교 뒤 삭제)과 회전 시간 실행. 요약의 A/B 장에 그림 차이와 GPU 프레임 차가 나온다. 배치는 여유 공간 30 GB 미만이면 시작하지 않고, 요약 뒤에 원본 프레임(`*.pfm`)을 지운다(`-KeepRaw`). 아직 돌리지 않았다(`UE6_WORKPLAN_KO.md` 8.3 (g)).
  - 그 뒤에 더한 묶음(`UE6_WORKPLAN_KO.md` 8.5): lanes(광선 패스의 목록 디스패치), rays, dof, hdr, material, weather, steam, 그리고 tsr·clouds·fog의 새 행. 게이트에 `--lens`, `--rain`, `--display-peak`, 캡처 층 `chain`.
- 병합 검토(읽기; `UE6_WORKPLAN_KO.md` 12절): 일곱 브랜치의 21,257줄에서 실행하면 틀리는 것 18건을 찾아 고쳤다 — 가장 큰 둘은 기본 경로에 있었다(see-through 인스턴스가 GI 광선을 막던 것, 데칼 8개부터 틀리던 광선 씬의 데칼 수). 고치지 않고 적어 둔 것 10건(비의 장막에 위쪽 끝이 없는 것이 가장 크다). 코드 작성·빌드 통과, 실행 안 함.

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

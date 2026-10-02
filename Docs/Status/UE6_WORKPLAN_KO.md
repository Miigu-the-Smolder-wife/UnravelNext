# UE6 구조 렌더러 — 남은 작업과 조사 결과 (2026-10-02)

사용자 지시(2026-10-02): 안개를 잘 만들 것. 얇은 지오메트리 비용, 숲 규모, high 티어, 캐릭터 음영, 원거리 GI, 구름. 배치가 도는 동안에도 코드를 쓸 것. 최적화할 때 표현·함수·알고리즘·수치해석·시스템 구조·하드웨어 매핑·수학/물리 모델을 점검할 것.
측정 결과는 `UE6_PORT_STATUS_KO.md` 7절. 이 문서는 "무엇이 왜 남았고 어디를 고치는가"만 적는다.

## 0. 작업 방식 (2026-10-03 사용자 지시로 바뀜)

- **검증하지 않는다.** 테스트 실행 파일·스틸·게이트·배치·GpuLock을 돌리지 않는다(사용자가 실행을 말할 때까지). 확인은 빌드 통과뿐이고, 옳음은 코드와 원본 소스를 읽어 따진다. 새로 쓴 것은 문서에 "코드 작성·빌드 통과, 실행 안 함"으로 적는다.
- 기능 완성과 구조 최적화를 **분량으로** 한다: 영역별 에이전트가 각자의 워크트리·브랜치에서 쓰고 빌드하며, 주 세션은 겹치지 않는 코드를 쓰고 병합한다. 빌드는 `-Jobs 6 -LowPriority`(사용자가 같은 기계에서 게임 중).
- 디스크: 병합된 워크트리는 바로 지운다(`git worktree remove --force`; 커밋 안 된 소스는 그 브랜치나 `wip/<이름>`에 커밋한 뒤). 캡처 출력은 남기지 않는다. 옛 세션 워크트리의 미커밋 소스와 텍스트 기록은 `wip/old-*` 브랜치에 있다.
- 세션이 끊기면 에이전트도 멈춘다. 워크트리와 브랜치는 남으므로 아래 표로 이어서 한다(미커밋 변경은 워크트리에 있다).

| 브랜치 (워크트리 `UnravelNext-ue6-w-<이름>`) | 맡은 것 | 대기 중인 후속 |
|---|---|---|
| `w/hair` | 머리카락 2단계: 다른 물체에 그림자, 그림자 경계 계단, 근접 비용, 반사·GI 프록시 (`char-hair2`의 WIP 병합 위에서) | — |
| `w/char` | 눈(EyeBxDF 대응), 천 fuzz 혼합, 광선 hit·coverage 층·뷰모델의 피부, 호스트 ABI | 재질 입력(UV 변환, 디테일 맵, 시차 맵), 광원 완성(사각광 텍스처·barn door, 채널, 광원별 배율) |
| `w/atmo` | 안개 노출 전 저장(`fog-tests` WIP), `froxelRayAt` 정밀도, 불투명 앞 구름, 씬 파일의 구름·안개 블록, GI 광선·A14 뷰 안개, 구름 태양 경로의 긴 스텝 앨리어싱 | — |
| `w/opt` | V·VSM 계층 컬을 작업 큐 한 커널로, 빈 래스터 요청 생략, 그림자 뷰의 텍셀 미만 인스턴스 제외, 프록셀 리스트 후보 1회, 게이트 통계 | VSM 정적/동적 페이지 분리, 굵은 상주 페이지·페이지 팽창, 그림자 뷰 HZB, HZB로 거른 무효화; 그 뒤 V 소프트웨어 래스터·클러스터 압축·스트리밍 연결 |
| `w/tsr` | TSR resurrection·reprojection field·thin geometry·이력 해상도, 업스케일 뒤 모션 블러, 반투명 속도, 샤픈·렌즈 플레어, 장면 기준 컬러 그레이딩(결합 LUT) | — |
| `w/cache` | 표면 캐시 해상도 피드백, 클러스터 LOD 카드 캡처, 캡처의 재질 층, radiosity 뒷면 재추적, 카드 없는 hit의 간접광, 수집·반사 커널 재대조 | 표면 캐시 직접광의 광원 함수 |
| `w/coverage` | coverage 층: 가려진 프래그먼트 저장 안 함(깊이 버킷·타일 HiZ), 삼각형당 비용, 타일×깊이 구간 조명, 통계 | — |

주 세션이 2026-10-03에 직접 넣은 것(코드 작성·빌드 통과, 실행 안 함): 태양 접촉 그림자(`shadow.vsm.screen_ray_length`), 재질 AO 맵의 간접광·스펙큘러 가림 적용(`mWordOcclusion`), 입자용 광량 볼륨의 광원 함수, 구름 태양 경로 24스텝 + 추적 텍셀 전용 디스패치.

## 1. 안개

첫 구현(공기 볼륨의 매질)의 한계 [실측/그림]: 하늘·먼 땅이 회색으로 덮임, 국소광 많은 실내에서 깊이 슬라이스 띠(타일 24 px × 64 슬라이스가 65 km까지: 30 m 안에 슬라이스 약 22개).
지금 구조(원본의 VolumetricFog + ExponentialHeightFog에 해당; 코드는 `Passes/Atmosphere/Fog*.hlsl*`, `Passes/Shadow/FroxelSystem.cpp`):

| 부분 | 내용 | 스위치(`atmosphere.fog.*`) |
|---|---|---|
| 근거리 볼륨 | 셀 16 px × 96 슬라이스(0~80 m, 슬라이스 = log2(z·k + 1)·b). 셀마다 소광과 산란광: 태양 × 공기 투과 × 구름 그림자 × VSM 그림자 구간, 국소광(MegaLights 볼륨의 fluence·moment), 간접광(이전 프레임 반투명 볼륨). 지터 + 히스토리 0.9 | `cell_px`, `depth_slices`, `volumetric_distance_m`, `history_weight` |
| 원거리 | 80 m ~ 65 km를 16 슬라이스로, 닫힌 식 높이 안개. 태양 항은 공기 볼륨이 구한 슬라이스별 그림자 비율과 구름 그림자를 받는다(능선 그림자가 먼 안개에 드리운다) | `far_slices`, `far_distance_m`, `far_shadows` |
| 하늘 | 하늘 픽셀도 안개를 통과(원본의 안개 패스는 하늘도 덮는다: `bFogOnlyOnRenderedOpaque`는 장면 캡처 전용). 0이던 때 안개 낀 능선이 맑은 지평선 앞에 서 있었다 [그림: FogV2 ridge_sunset] | `sky_amount` (기본 1) |
| 밀도 변화 | 평균 0인 2옥타브 값 잡음(격자 주기 256, 높이 방향 2배 촘촘), 씬 바람을 따라 흐름. 원거리 닫힌 식은 평균 밀도 그대로 | `noise_amount`(0.3), `noise_scale_m`(20), `noise_drift_mps`, `noise_wind_scale` |
| 읽기 | 공기 조회(`atmosphereAerial`/`atmosphereAirView`) 안에서 한 번의 3D 조회 — 불투명·coverage·유리·물·입자가 각자 깊이에서 받는다 | — |
| 반사 광선 | 반사 광선(화면 추적·월드 추적·하늘 miss)이 광선 구간의 안개를 받는다: 투과는 닫힌 식, 광원은 반사면까지의 뷰 경로 평균(볼륨의 산란광 ÷ 불투명도) — 방 안 안개는 방의 것 | `on_rays` |
| 평면 반사 뷰 | 뷰마다 자체 볼륨(프레임당 4개까지, 히스토리·지터 없음, 거울 평면부터) — 호수가 안개 낀 건너편과 하늘을 비춘다 | `secondary_views` |
| 게임 입력 | `FrameContext::fog`(= `UnxFrameSetFog`, ABI 6 선택 export): 밀도·감쇠·높이·알베도·g·시작 거리·하늘 양·잡음. 없으면 품질 파일 값. 높이는 월드 높이(원점 이동을 따라간다) | 게이트 `--fog D` |
| 그림자 페이지 | 셀 중심 광선 구간의 페이지 요청(`s.vsm.markfog`) | `shadow_texels_per_cell` |

물리 모델 점검:

- 단일 산란 + 안개 자기 그림자 없음. 알베도 ≈ 1인 안개에서 "태양 경로의 자기 소광"과 "다중 산란으로 되돌아오는 빛"은 대부분 상쇄되므로(에너지는 흡수되지 않고 퍼질 뿐) 얇은 안개(광학 두께 < 1)에서는 맞는 근사다. 두꺼운 안개(낮은 태양 + 긴 경로)에서 실제는 직사광이 확산광으로 바뀌어 표면 그림자가 흐려지는데, 이 모델은 표면의 직사광을 줄이지 않는다 — 원본도 같다. 필요해지면: 표면의 태양광 × exp(−τ_안개·(1−g′))와 그 차이를 하늘광에 더하는 항.
- 위상함수: HG(g 0.2) 한 로브(원본 기본값). 실제 물방울(g ≈ 0.85)의 강한 전방 피크는 없다.
- 원거리 광원은 열마다 80 m 지점의 태양·간접광 하나다(먼 곳의 간접광 변화 없음).
- 반사 광선의 안개 광원은 반사면 근처 값이다: 긴 반사 광선 끝의 조명 변화는 반영되지 않는다.

남은 것: GI 광선(원본도 기본 끔 `SampleHeightFogOnGI`), 보조(A14) 뷰, 국소 안개 볼륨, 입자 매질과의 순서.

테스트: `unx_test_shadow_fogtests`(`Passes/Shadow/Tests/FogTests.cpp`, 프로브 `FogProbe.hlsl`). 닫힌 식·격자·잡음은 C++ 쌍둥이와 GPU를 같은 질의에서 비교하고, 프레임에서는 적분 볼륨을 읽어 균일 매질·높이 감쇠·시작 거리·태양 그림자(근거리 셀, 원거리 슬라이스)·국소 볼륨·원점 이동·평면 반사 뷰를 기준과 비교한다(허용 오차와 근거는 파일 머리말). 테스트가 찾아 고친 것 [실측]:

- `fogOpticalDepth`가 밀도를 64배로 묶는 높이를 가로지르는 구간에서 `fogExtinctionAt`의 적분과 달랐다(평균을 64로 잘랐다: 밀도 0.002·감쇠 0.02에서 안개 높이 0.3 m 위에서 아래로 500 m, 닫힌 식 64 대 적분 34.65). 구간을 묶인 부분과 지수 부분으로 나눠 적분한다(적분 대비 2e-9).
- 뷰가 셀의 정수 배가 아니면(1080행 = 67.5셀) 중심이 뷰 가장자리에 놓이는 마지막 행 셀이 히스토리를 받지 못했다(이전 볼륨 범위가 아니라 뷰 범위로 검사). 이전 볼륨의 범위로 검사한다.

남은 한계 [실측]: 볼륨의 복사 휘도는 노출 전 nit를 fp16에 담아 65504에서 잘린다(g 0.8·128 klx·밀도 0.2에서 태양 쪽 기준값 3.2e5 nit; 이 검사 하나는 실패로 남아 있다). fp16 기록은 0 쪽으로 버려져(이 하드웨어) 히스토리 0.9에서 셀 끝까지의 광학 두께가 평균 0.28 ~ 0.29 %, 복사 휘도가 0.21 ~ 0.25 % 낮다(히스토리 없이 0.03 %, 0.06 % 이내; 상한 1 %). `noise_amount` 0.5 초과는 인자가 0에서 잘려 평균 밀도가 닫힌 식보다 높다(1.0에서 +0.6 %).

## 2. 얇은 지오메트리 (조사 완료)

- 빌더가 그룹마다 단순화 오차를 `lod_max_relative_width_error(0.25) × 그룹의 가장 좁은 폭`으로 묶는다(`Tools/ClusterBuilder/src/ClusterBuilder.cpp:183-195`). 넘으면 그 그룹은 종단(:219). 나무 클러스터 3,820개 중 LOD가 있는 것은 3개, 풀은 0개(`Results/V/BandC/README_KO.md:18`).
- 먼 얇은 지오메트리를 맡기로 한 대역 C 브릭은 꺼져 있다(`brick_max_feature_width = 0`).
- preserve-area 없음. 테스트 `thin_geometry_is_not_thinned`(`Tests/ClusterBuilderTests.cpp:531-553`)가 "면적 2 % 이내 유지"를 요구.
- 쿡 캐시 키에 이 값이 들어 있어(`ClusterBuilder.cpp:624`) 바꾸면 모든 메시가 다시 빌드된다.
- coverage 래스터: 프래그먼트당 약 0.3 ns + 삼각형당 1.13 ns, 호숫가 4K에서 1,200만 프래그먼트(`COVERAGE_REDESIGN_KO.md:209`). 합성은 기록당 21~24 ns였던 것을 타일×깊이 구간 조명으로 줄이는 설계가 있다(`RENDERER_REDESIGN_V2_KO.md` 14.1c, 스위치 `shading.coverage_tile_lights`).
- 대역별 통계는 `unx::visibility::Stats`에 있으나 RendererGate가 찍지 않는다(`RendererGate.cpp:1386`).
- **할 일**: (a) 빌더: 얇은 그룹도 단순화하되 섬(연결 성분) 단위로 남은 삼각형을 키워 면적을 보존(Nanite의 Preserve Area). 단순화된 클러스터의 폭을 다시 계산. (b) 게이트에 대역별 통계 출력. (c) 그 뒤에도 남는 대역 B 비용은 설계 문서의 깊이 묶음 래스터·프리미티브 HiZ.
- 숲 씬의 그림자 상한 폭발(레벨당 4e8)도 나무에 LOD가 없어서다: (a)가 되면 굵은 레벨의 절단이 수천 → 수 개로 준다.

**(a) 빌더 구현 (2026-10-02, 브랜치 `lod-thin`; CPU 테스트만 돌렸고 런타임 연결은 아직 없다)**

- 설정 `visibility.lod_thin_preserve_area`(기본 true, 쿡 캐시 키에 포함). 폭 규칙에 막혀 종단이 되던 그룹에서, 다른 그룹과 정점을 나누지 않는 섬(잎·풀잎 하나)을 통째로 버린다. 그룹 안에서 공간 순서로 하나 걸러 하나씩 버려 한쪽만 비지 않게 한다. 폭 규칙은 그대로다: 얇은 형상을 깎는 collapse는 여전히 없다.
- 남은 섬은 무게중심을 기준으로 주축마다 같은 오프셋 d만큼 키운다(축 배율 1 + 2d / 그 축 길이, 상한 4배). 둥근 잎에는 균일 확대와 같고, 풀잎·바늘잎은 길어지지 않고 넓어진다. d는 그룹 면적이 단순화 전과 같아지도록 이분법으로 구한다. 잠긴 섬은 그대로 두고 자유로운 섬이 부족분을 모두 가진다. 2 % 안으로 못 맞추면 그 그룹은 전처럼 종단이다.
- 기록 오차 = max(단순화 오차, 버린 섬의 특징 폭) + 키우며 생긴 최대 변위. 잎은 그 폭이 LOD 임계(1 px)보다 좁게 보이는 거리에서만 사라진다(버린 섬과 남은 섬 사이의 거리는 오차에 넣지 않았다).
- 키운 섬은 메시에 없는 위치라서 빌더가 새 정점을 만든다(`clusterbuilder::LodVertices`: 위치 + 나머지 속성을 가져올 원본 정점). 단순화된 클러스터의 폭은 키운 형상으로 다시 잰다.
- **런타임 연결이 남았다.** 클러스터는 메시 정점 인덱스만 가지므로 새 정점이 씬 메시에 있어야 한다: `build(scene, settings, &stats, &lod)` → `lod.appendTo(scene)` → `GpuScene::upload(scene)` 순서. 지금의 호출부(`HostRenderer::commit`, 게이트들)는 `lod` 없이 부르고, 그 경우 설정은 효과가 없으며 출력은 전과 비트 단위로 같다(씬 9개의 해시 일치: 지형·튜브·풀·forest_thin·forest_card). `HostRenderer::commit`·RendererGate·VisibilityGate·WaterGate는 이미 `build` 뒤에 `upload`를 하므로 세 줄이면 된다. GiGate·StreamGate는 `upload`가 먼저라 순서를 바꿔야 하고 MGate는 씬이 const다. 호출부는 빌더 밖이라 고치지 않았다.
- 실측, 잎 4만 장 수관(삼각형 8만, 방향 균일: 방향 부류마다 DAG): 절단별 삼각형 80,000 → 39,854 → 19,790 → 9,768 → 5,404 → 4,568(1/17.5), 클러스터 3,766 → 288(1/13.1), 면적은 모든 절단에서 1.000, 오차 0.09 ~ 0.32 m, 폭 중앙값 5.2 → 26 cm.
- 실측, scenegen `tree_thin_0`(삼각형 80,448, 클러스터 3,817): 가장 거친 절단이 114 삼각형 · 클러스터 3개(전에는 80,256 · 3,814), 면적 1.000, 오차 1.15 m. 오차 8 cm에서 10,156 삼각형(1/7.9), 16 cm에서 2,618(1/31). 종단 그룹 267 → 3. 새 정점 238,476개(원본 240,315개: 정점 메모리 약 2배). 빌드는 한 그루에 약 0.8 ~ 2.7 s → 1.4 ~ 3.6 s(다른 작업과 CPU를 나눠 쓴 값).
- forest_thin의 잎·풀 전체(인스턴스 가중, 가장 거친 절단): 클러스터 4.07억 → 2,530만(1/16), 삼각형 82.3억 → 2.1억(1/39). **남은 2,530만 가운데 2,500만이 풀이다**: `grass_thin`은 풀잎 100장이 방향 부류 약 25개로 갈려 DAG마다 클러스터가 1개라 줄일 것이 없다. `sheet_orientation_min_width = 0.016`(풀잎을 방향에 상관없이 묶음)이면 풀 한 포기가 클러스터 1개가 되어 합계 130만 클러스터 · 2,640만 삼각형이다. 이 값은 원본 클러스터 구성을 바꾸므로 GPU A/B와 함께 정한다.
- 테스트: `unx_test_clusterbuilder thin_` (`thin_geometry_has_lod_and_keeps_its_area`, `thin_geometry_among_shared_borders`, `thin_setting_off_is_the_build_without_it`, `thin_setting_is_part_of_the_mesh_key`, `thin_lod_of_scene_meshes`); 기존 `thin_geometry_is_not_thinned`은 그대로 통과한다(정점을 받지 않는 빌드).

## 3. 숲 규모

- (2)의 LOD가 먼저. 그 뒤 남는 것: 그림자 레벨의 텍셀보다 작은 인스턴스를 V의 인스턴스 컬에서 제외(뷰 레코드에 최소 반지름), 요청 수 줄이기(빈 요청 1개가 약 0.2 ms).

## 4. 캐릭터 음영

| | 현황 | 남은 것 |
|---|---|---|
| 피부 | A: 이중 GGX 로브 + 얇은 부분 투과광(`ShadeOpaque` LAYERED=3, `MegaLightsShade`). B: 화면 공간 SSS(`SubsurfaceScatter.hlsli`: Burley 프로파일, d = ℓ / s(A), s(A) = 1.9 − A + 3.5(A − 0.8)², 접평면 표본 16개, 알베도는 산란 뒤). 기본 평균 자유 경로 = 피부 실측 1.30 / 0.95 / 0.67 mm | 광선 hit·coverage 층의 피부, 1인칭 뷰모델 반지름, 면광원·평면 반사 뷰는 컴파일만 됨, 호스트 ABI에 파라미터 없음 |
| 머리카락 | 가닥 기록 음영: 폭 평균 섬유 모델 + 이중 산란, 몸마다 64³ 밀도 볼륨(빛 쪽 섬유 수), 자체 MegaLights 인스턴스, 반투명 볼륨 SH의 간접광. 2단계(브랜치 `w/hair`, 4.1절): 다른 표면과 다른 몸의 머리카락에 드리우는 그림자, 그림자 경계의 계단을 없애는 행진 규칙과 지터, 세그먼트 단위 음영·기록 목록·정렬 크기, 반사 광선과 최종 수집 광선의 프록시. **빌드만 했다. GPU에서 실행한 적이 없다(테스트·그림·시간 모두 없음)** | 4.1절의 실행 확인 전부. 머리 그림자가 없는 곳: 안개·공기의 국소광, `mega_lights` 없는 coverage 프래그먼트의 국소광, 픽셀의 세 번째 그림자 광원 뒤의 국소광, 광선 hit·카드의 그림자 광선. 광선이 보는 머리카락에는 섬유 모델이 없다. 기록 수(가닥이 지키는 광학 두께 × 실루엣)는 그대로다 |
| 눈 | 없음 | 각막 굴절·홍채 깊이·림버스(원본 `EyeBxDF`) |
| 천 | Charlie sheen 층 있음(클리어코트와 배타) | fuzz 혼합 |
| 클리어코트 | 있음 | — |

### 4.1 머리카락 2단계 (2026-10-03, 브랜치 `w/hair`)

`char-hair2`의 중단 시점 작업을 합치고 이어서 썼다. **전 트랙 빌드만 통과했다. 테스트 실행 파일·still·게이트를 한 번도 돌리지 않았다**(사용자 지시: GPU를 쓰지 않는다). 아래는 코드에 있는 것이고, 그림과 수치로 확인된 것은 없다.

| 항목 | 코드에 있는 것 | 파일 · 스위치 |
|---|---|---|
| 1. 다른 물체에 드리우는 그림자 | 태양: S의 화면 가시성(VSM 조회 결과)의 태양 슬롯 × 표면에서 태양까지 밀도 볼륨의 투과율 exp(−섬유 수) (`s.shadow.hair`). 국소광: MegaLights 표본의 가중치 × 표본 광원까지의 투과율, 1/256 아래는 가려진 표본으로(`m.ml.hair`; 머리카락 자체의 인스턴스에는 적용하지 않는다). 다른 몸의 가닥: 가닥이 자기 몸의 섬유에 더해 빛까지의 경로에 있는 다른 몸의 섬유를 센다(수염 위의 머리, 옆 사람의 머리). 국소광까지의 행진은 광원 거리에서 끝난다. 2차(2026-10-03): coverage 층의 클러스터 프래그먼트(기록이 있는 픽셀마다 S의 태양 프로파일과 같은 4개 깊이의 투과율을 `m.coverage.hairsun`이 쓰고, 합성이 프래그먼트의 태양 가시성에 곱한다), 안개 셀과 공기 슬라이스의 태양 항, `mega_lights`가 꺼진 경로의 S 국소 슬롯 1~3(불투명 표면) | `Passes/Hair/HairShadow.hlsl`(MODE 0~2), `HairDensity.hlsli`(`hairTransmittance`, `hairFibreCountOthers`), `VsmSystem.cpp`, `MegaLights.cpp`, `CoverageHair.hlsl`, `CoverageShade.hlsli`, `FogScatter.hlsl`, `FroxelSlice.hlsli`, `FroxelSystem.cpp`. `shading.hair_shadows`, `shading.hair_shadow_steps` |
| 2. 그림자 경계의 계단 | 원인(코드): 가닥의 행진이 경로 전체의 스텝 길이로 텍스처 하나를 골랐다. 16스텝에서 경로가 24셀(머리 기준 29 cm)을 넘으면 4×4×4 평균(4.7 cm)만, 그것도 최대 6~7셀 간격으로 읽었고, 그룸을 지나는 경로는 거의 다 그랬다. 고른 텍스처가 바뀌는 곳에는 선이 생긴다. 지금: 긴 경로는 처음 steps/2 셀을 한 셀씩 읽고(가닥 바로 옆의 머리카락이 자기 그림자의 경계를 만든다) 나머지는 평균을 거친 셀 한 칸씩 읽는다(최대 steps 칸; 표본 수는 16 → 8 + 나머지 길이/4셀). 머리카락이 아닌 표면의 행진은 거친 셀 한 칸씩 가며 빈 칸은 건너뛰고 찬 칸은 셀 표본 4개로 읽는다(경로 전체가 셀 해상도). 모든 행진의 표본 위상을 픽셀·기록·세그먼트 점과 프레임마다 새로 뽑는다 — 남는 셀 무늬는 시간 필터가 지우는 잡음이 된다(원본의 voxel 순회와 같은 방식) | `HairDensity.hlsli`, `CoverageHair.hlsl`(`hairJitter`). `shading.hair_density_steps`, `shading.hair_march_jitter` |
| 3. 근접 비용 | 가닥 쪽 계산(태양 커널, 간접광)을 기록마다가 아니라 기록이 있는 세그먼트마다 3점에서 한다(`m.hair.visible`, `m.hair.strands`). 밴드 A 표면 앞의 기록만 목록에 넣어 64스레드 그룹으로 음영한다. 목록 항목이 기록의 타일을 함께 가진다(블록마다 한 번 아는 값; 전에는 기록마다 타일 목록을 이분 탐색했다: 타일 수의 log2회, 12~15회 조회). 무거운 픽셀의 정렬망을 run 크기에 맞춘다(17~32개 run: 16쌍 × 15단, 전에는 512쌍 × 55단 고정), run이 하나인 픽셀은 병합 없이 정렬된 쌍을 그대로 쓴다. 가까운 hair 기록·먼 hair 기록 찾기는 타일 블록 단위 dispatch. LOD가 남기는 가닥의 세그먼트만 프레임 버퍼에 쓴다: 가닥을 해시 순서로 두면 남는 가닥은 그 순서의 앞 K개다(원본이 곡선을 재배열해 LOD를 앞 N개로 두는 것과 같다). 전에는 버린 가닥도 반지름 0으로 자리를 차지해 세그먼트 버퍼·V의 mesh 그룹·밀도 splat·세그먼트 비트와 조명 버퍼가 그룸 전체 크기였다(`hair_ball`: 세그먼트 88만 개 중 그리는 것은 약 7.5만 개 [계산: 비율 0.085]) | `CoverageHair.hlsl` MODE 5, 7~11, `CoverageHeavySort.hlsl`(SIZE 0/1), `CoverageHeavyRound.hlsl`, `ShadingSystem.cpp`, `HairSystem.cpp`·`HairSimulate.hlsl` STEP 2. `shading.hair_segment_shading` |
| 4. 반사·GI | 그룸은 TLAS에 없다. 광선의 프록시는 밀도 볼륨이다: 광선의 첫 섬유(몸마다 섬유 수의 지수분포에서 뽑은 문턱, 광선마다 한 번)가 광선의 hit보다 앞이면 그것이 hit이다. 그룸 바깥을 향하는(평균 밀도 기울기의 반대) 무광 표면으로 음영한다: 머리 재질의 base colour, 태양 그림자 광선 1개 + 국소광 표본 1개와 그 그림자 광선, 각각 점에서 빛까지의 머리카락 투과율. 반사(`ReflectionLumenTrace`)와 최종 수집(`LgTrace`)의 월드 광선에 있다. 두 화면 추적은 화면 hit보다 앞에 섬유가 있는 광선을 월드 추적에 넘긴다(같은 픽셀·프레임의 같은 뽑기; 화면 깊이에는 머리카락이 없다). 원본은 같은 광선에서 hair voxel을 순회하고 hit을 검은색으로 둔다. 2차(2026-10-03): radiance cache 광선과 반투명 볼륨 광선도 머리카락을 만난다 — 단, 광선이 출발하는 점이 들어 있는 그룸은 뺀다(가닥은 셀의 빛을 자기 몸의 머리카락 투과율로 직접 감쇠하므로 두 번 세지 않게). hit의 간접광 = 직전 프레임 반투명 볼륨 SH의 조도(프록시 면 쪽) | `RayTracing/HitHair.hlsli`, `RayScene::recordHair`(조명 격자 헤더 22·23번 워드), `LgTrace.hlsl`, `LgScreenTrace.hlsl`, `ReflectionLumenTrace.hlsl`, `ReflectionScreenTrace.hlsl`, `LumenRadianceCacheTrace.hlsl`, `LumenTranslucencyVolumeTrace.hlsl`. `raytracing.hair` |

비용 점검에서 하지 않은 것과 이유 [계산]:

- 기록 수 상한. `hair_ball`의 373만 기록은 가닥의 투영 면적이다: LOD가 가닥 수를 줄이면 폭을 1/비율로 넓혀 광학 두께를 지키므로 기록 수 ≈ 광학 두께(그룸당 약 5.5) × 실루엣 픽셀 × (폭 + 1)/폭 이고, 남기는 가닥의 폭은 거리와 무관하게 지름 × 가닥 수 / (8 × 길이) 픽셀(이 씬 1.09)이다. 줄이려면 광학 두께를 깎아야 한다.
- 카메라 쪽 광학 두께로 뒤 기록을 버리는 방법: 두께 4에서 자르면 1.8 %가 뒤 배경으로 새고(밝은 하늘 앞에서 보인다), 6에서 자르면 머리 위(앞쪽 껍질 평균 3~4)에서는 버릴 것이 거의 없다. 새지 않게 하려면 합성이 남은 몫을 머리카락으로 채워야 하는데, 합성 커널(`CoverageComposite` 197,232 B / 한도 204,800 B)을 고쳐야 한다. 하지 않았다.
- 합성은 이미 마스크 합집합이 차면 멈춘다. 남은 비용은 기록당 정렬과 합성이다.

실행해서 확인할 것(순서대로): `unx_test_hair_hairtests`(밀도 볼륨 3절: 파라미터 배치가 바뀌었다), `unx_test_visibility_haircoveragetests`, `unx_test_host_hosthair`, `hair_ball` still(자기 그림자 경계, 바닥·두피의 그림자, 두 머리 사이 그림자), 거울이나 젖은 바닥이 있는 씬의 반사, 그 뒤 시간 측정(`m.hair*`, `m.coverage*`, `r.refl.lumen.trace`, `r.gi.lg.trace`).

## 5. 구름

- 있음: 구형 껍질 한 층의 볼륨 레이마치, 이중 HG, 2옥타브 다중 산란 근사, 1/4 해상도, 태양 방향 깊은 불투명도 맵.
- 이번에 더한 것: 2×2 블록의 한 텍셀씩 추적 + 재투영(`atmosphere.clouds.temporal`), 돔은 프레임당 16행 갱신. 구름 그림자가 안개 셀·원거리 안개·공기의 단일 산란에 드리운다. 구름 뒤의 태양 원반·달·별이 구름 투과율을 받는다.
- 없음: 불투명 지오메트리 앞의 구름(산 정상이 구름 속에 있는 경우), 씬 파일의 구름·안개 블록(지금은 호스트 API와 게이트 인자).

## 6. 원거리 GI

- radiance cache 프로브와 반투명 볼륨의 광선이 `lumen.radiance_cache_far_field_distance_m`(10 km)까지 간다. 카드 범위(300 m) 밖 hit은 직접광 + 하늘 조도 근사(`giFarSkyIrradiance`: 법선 방향 하늘 복사휘도 × π).
- 실내(로비) 비용 변화 없음 [패스 중앙값]. 퍼니스 방 낮 = 밤 유지. 실외 A/B는 배치 7.
- 원본과의 차이: 원본은 HLOD로 만든 원거리 TLAS와 원거리 카드를 쓴다. 여기는 TLAS 하나에 전체 인스턴스가 있어 거리만 늘렸다 — 인스턴스 수가 큰 씬(숲)에서의 순회 비용은 배치에서 본다.

## 7. 티어

| 티어 | 로비 4K (프레임 / 패스 합, ms) | 정의 |
|---|---|---|
| epic | 11.76 / 11.44 (안개 켬) | 파일 값 그대로 |
| high | 9.98 / 9.64 | 프로브 32 px, 반사 2×2당 1광선, MegaLights 표본 2, radiance cache 프로브 16², TSR flicker 끔 |
| performance | 7.44 / 7.25 | 내부 720줄(4K ×3), 반사 2×2당 1광선, TSR flicker 끔 |

## 8. 4K 목표와 최적화 점검

실측 기준: 로비 4K(내부 1080p) 11.86 ms, 패스 이름 261개 [Ue6Batch2]. "1080p" 실행은 내부 720p다(render_scale 0.667, 최소 높이 1080).
같은 씬의 두 해상도로 나누면 **해상도와 무관한 일 3.3 ms + 내부 100만 픽셀당 4.1 ms**(4K에서 8.5 ms)다.

| 묶음 (4K, ms) | 합 | 고정 | 픽셀 비례 |
|---|---|---|---|
| r.gi | 2.32 | 0.45 | 1.87 |
| m.ml (MegaLights) | 1.54 | 0.20 | 1.34 |
| r.refl | 1.27 | 0.05 | 1.21 |
| m.tsr + m.upscale | 1.64 | 0 | 1.6 |
| s.froxel | 0.91 | 0.09 | 0.81 |
| m.coverage (+direct) | 0.89 | 0.9 | 0 |
| m.lit + m.resolve | 0.93 | 0 | 0.93 |
| r.card | 0.53 | 0.50 | 0.04 |
| s.shadow + s.vsm | 0.86 | 0.56 | 0.30 |
| v.cull + v.coverage | 0.38 | 0.40 | 0 |

50 µs 미만 패스 211개의 합 1.77 ms. 픽셀당 비용이 큰 패스 하나가 있는 것이 아니라 0.1~0.3 ns/px짜리 패스 수십 개의 합이다 — 원본과 같은 구조의 비용이다. 그래서 4K 프레임을 ms 단위로 움직이는 설정은 내부 해상도 하나다: 티어 `performance`(내부 720줄, 4K ×3 업스케일)를 추가했다.

측정의 함정 [이번에 확인]: 패스 CSV에는 같은 이름의 패스가 한 프레임에 여러 번 나온다(MegaLights temporal은 확산·스펙큘러 등). 행 단위 중앙값을 더하면 틀린다 — 프레임 안에서 이름별로 합친 뒤 통계를 낸다(하니스의 JSON이 그렇게 한다). 빌드·테스트가 도는 중의 시간 실행은 프레임 중앙값이 약 1 ms 부풀었다(패스 중앙값의 합은 그대로: 11.79 → 11.88, 프레임 11.86 → 12.95) — 요약에 패스 중앙값의 합을 함께 낸다.

사용자 지시의 축별 점검과 결정:

| 축 | 본 것 | 결정 |
|---|---|---|
| 표현 | 프록셀 격자(조명 리스트·공기·국소광 볼륨)와 반투명 볼륨은 **방향과 깊이**의 함수를 담는데 셀이 픽셀 수로 정해져 있었다: 내부 1080p가 720p의 2.25배 셀을 같은 그림에 썼다(s.froxel 0.90 vs 0.45 ms, ltv.trace 0.38 vs 0.21 ms) | 기준 높이(720) 위에서는 셀이 각도를 유지(`atmosphere.froxels.tile_reference_height`, `lumen.translucency_volume_grid_reference_height`). 측정 대기 |
| 시스템 구조 | 작은 패스 211개 1.77 ms: VSM 약 57개, GI 28개, V 컬 36개, coverage 22개, 카드 11개. 한 묶음이 지배하지 않는다. V·VSM의 계층 컬은 레벨마다 준비+실행 2패스(간접 인자) | 패스 타임스탬프 자체가 큐를 세우는지부터 잰다(게이트 `--no-pass-timestamps`). 그 뒤: 계층 컬을 한 커널의 작업 큐로, 빈 래스터 요청 건너뛰기 |
| 알고리즘 | `s.froxel.lists`: 조명 × 타일 전수 검사를 count와 fill에서 두 번, fill은 프록셀마다 중요도 정렬(상위 lights_max) | 각도 격자 뒤의 수치를 보고: 타일 후보를 한 번만 만들기, 정렬 머리가 필요한 소비자(MegaLights 켠 상태)에 한정 |
| 수치해석 | 안개 조회의 1 − T는 fp16에서 불투명도 0.4 % 아래로 양자화된다(반사 광선의 광원 추정에서 그 아래는 적용 안 함). 높이 안개 광학 두께의 (e0 − e1)/a는 a → 0에서 양 끝 평균으로 전환(상대오차 1e-4) | 유지 |
| 수학/물리 모델 | 안개(1절). 원거리 GI: 카드 밖 hit에 하늘 조도 근사. 공기: 구름 그림자가 공기의 단일 산란에 없었다(구름 아래 지형 위에 맑은 날의 대기 원근) | 공기 슬라이스의 단일 산란에 구름 그림자(표면 픽셀이 읽는 슬라이스만; 하늘 보정 항은 그대로) |
| 하드웨어 매핑 | TSR reject: 16×16 타일에 테두리 6(28×28 = 3.06배 작업). 타일을 키우면 그룹 메모리 한도(32 KB)에 걸린다(지금 23 KB) | 보류: 채널 패킹으로 배열 수를 줄이면 32×32 타일(1.89배)이 가능 — 측정 후 |
| 함수 | MegaLights 볼륨의 1/d²는 프록셀 반지름으로 바이어스(스파이크 제거, 원본과 같은 식) | 완료 |

high 티어: 같은 커밋에서의 비교가 아직 없다(Batch2 기본 vs Batch4 high는 커밋이 다르다) — Batch5 결과로 다시 본다.

## 9. 국소광 구성요소 (2026-10-03, 브랜치 `w/hair`)

Unreal의 light component에 있고 여기에 없던 여섯 가지를 썼다. **코드 작성·빌드 통과, 실행 안 함**: 전 트랙 빌드만 통과했고 테스트 실행 파일(CPU 테스트 `unx_test_scene_lightcomponents` 포함)·still·게이트를 한 번도 돌리지 않았다. 아래는 코드에 있는 것이고 그림과 수치로 확인된 것은 없다.

레코드 [코드]:

- `scene::Light`의 새 필드. 기본값은 전과 같은 빛이다: `specularScale`·`diffuseScale`·`volumetricScattering`·`indirectIntensity`(1), `sourceTexture`(없음), `barnDoorAngle`(π/2 rad, 방출면 법선에서 잰 각)·`barnDoorLength`(0 = 없음), `lightingChannels`(1 = 채널 0), `maxDrawDistance`(0 = 항상)·`maxDistanceFadeRange`, `temperature`(0 = 안 씀, K), `falloffExponent`(0 = 역제곱).
- 씬 파일: 선택 블록 `LCMP`(광원 끝 바이어스 블록 뒤). 값을 하나라도 바꾼 광원만 인덱스 + 값 12개로 들어간다. 그런 광원이 없으면 블록이 없고 파일 바이트가 전과 같다.
- 인스턴스의 채널: `Instance::flags` 비트 4~6(`withLightingChannels`, `instanceLightingChannels`). 마스크 ^ 1로 저장하므로 비트가 0인 기존 인스턴스는 채널 0이다. GPU 인스턴스의 flags에 그대로 간다.
- `gpu::Light` 80 → 112 B: 스케일 4개(half, 값 − 1), 그리기 거리·페이드 구간, 감쇠 지수, barn door(half 2개), 소스 텍스처 SRV + 1. `typeFlags` 비트 9~11 = 채널 ^ 1. 새 워드가 모두 0이면 평범한 빛이다(FX 광원처럼 0으로 채운 레코드).
- R의 CPU 광원 레코드(`RtLight`): intensity = 세기 × indirect × diffuse, color = 색온도를 곱한 색, pad 비트 0 = hit이 창을 GPU 레코드로 다시 계산.
- 한 곳에서 계산한다(`Passes/Common/Scene.hlsli`): `lightWindow`(범위 창 또는 지수 창 × 뷰 페이드 × 채널 검사), `lightViewFade`, 스케일 접근자 4개, `lightBarnDoorRect`. 커널이 `UNX_LIGHT_COMPONENTS 0`을 정의하면 스케일·지수·페이드·barn door·소스 텍스처가 컴파일에서 빠진다(크기 한도에 걸린 커널용).

| 항목 | 코드에 있는 것 | 소비자 |
|---|---|---|
| 1. 스케일 | diffuse·specular: `shPunctualIlluminance`가 조도에 diffuse 스케일을 곱하고 specular/diffuse 비를 `g_shLightSpecular`에 둔다. 국소광의 specular 로브(`shSpecular`, `shSpecularSubsurface`)가 그 비를 곱한다. 태양의 로브는 `shSpecularLobe`(스케일 없음). 면광원은 `shAreaIntegral`이 적분마다 곱한다(회전 프레임 = diffuse, LTC = specular). volumetric: `froxelIntensity`. indirect: 빛을 저장하는 점(`mlPointLambert`)의 값과 R의 광원 레코드 | 불투명 음영(함수 안에서; `ShadeOpaque.hlsl` 본문은 한 줄만 바뀜), MegaLights 가중치와 음영(`mlLightUnshadowed`), coverage 프래그먼트(coat·sheen 로브 포함), 머리카락(조도의 diffuse 스케일), FAR 타일 항(diffuse), 공기 슬라이스·안개 셀·국소광 볼륨·물 매질·볼륨(volumetric), surface cache 직접광(diffuse × indirect), 광선 hit의 광원 표본(diffuse × indirect) |
| 2. rect 소스 텍스처 · barn door | 텍스처: `TextureSystem::lightSourceTextures` → `GpuScene::setLightSourceTextures`가 SRV를 광원 레코드에 넣는다. `shAreaColor`가 음영점의 방출면 위 수선의 발에서 읽는다. 레벨 = log2(평면까지 거리 / √면적) + log2(짧은 변 픽셀) − 2 (원본의 식). 방출면을 맞힌 광선은 hit 위치의 레벨 0. barn door: `lightBarnDoorRect`가 원본 `GetRect`의 식으로 점에서 보이는 사각형을 구하고 `shAreaIntegral`의 rect가 그 사각형을 적분한다 | 불투명·MegaLights·coverage·머리카락·surface cache(모두 `shAreaColor`, `shAreaIntegral` 경유), 반사 광선의 방출면 hit(`rtEmitterRadiance`) |
| 3. 채널 | 음영점의 인스턴스 채널을 `g_lightChannels`에 두면 `lightWindow`가 채널을 공유하지 않는 광원에 0을 준다. MegaLights 표본 뽑기(`MegaLightsSample.hlsl`)가 픽셀의 vis id → 인스턴스 flags로 정한다: 가중치가 0인 광원은 뽑히지 않으므로 음영 쪽에는 검사가 없다 | 불투명 표면의 MegaLights 경로. 안개·공기·광선 hit은 검사하지 않는다(원본도 같다) |
| 4. 최대 그리기 거리 | `lightViewFade` = saturate((거리 한계 − 카메라~광원 거리) / 페이드 구간), 구간 0이면 자름. `lightWindow`에 곱한다. 그림자 슬롯 우선순위(`VsmSystem.cpp`)에도 곱한다: 사라진 광원은 슬롯을 맨 뒤에 받는다 | `lightWindow`를 쓰는 모든 곳: 불투명, MegaLights, coverage, 머리카락, FAR 타일 항, 공기·안개·볼륨, surface cache, 광선 hit(pad 비트), 방출면 hit |
| 5. 색온도 | `colorTemperatureTint`(Krystek의 플랑크 궤적 근사 → Rec.709, 휘도 1), `lightColor` = 색 × 틴트를 원래 색의 휘도로 맞춘 값. GPU 레코드와 R의 레코드에는 곱한 색만 있다 | CPU(`GpuScene.cpp`, `RayScene.cpp`) |
| 6. 감쇠 지수 | 점·스폿에서 `falloffExponent` > 0이면 창 = (1 − (d/범위)²)^지수 × d². 호출하는 쪽의 1/d²와 상쇄되어 세기 × (1 − (d/범위)²)^지수가 된다(세기 = 광원 위치의 조도). 0이면 전과 같은 역제곱 × 범위 창 | 4와 같다. 광선 hit은 표본 가중치에 (자기 창 / 범위 창)을 곱한다(`rtLocalLightFinish`) |

한계 [코드]:

- `Native/Host` ABI에 새 필드가 없다(다른 작업의 몫으로 남겼다). 지금은 씬 파일과 `scene::Light`·`Instance::flags`로만 넣을 수 있다.
- `ShadeOpaque.hlsl` 본문에 남은 것: 국소광의 coat·sheen 로브에 specular 스케일(`shLightSpecular()`을 곱하면 된다), 프록셀 리스트 경로(`mega_lights` 끔)의 채널 검사(광원 루프 앞에서 `g_lightChannels`에 인스턴스 채널을 넣으면 된다).
- 채널 검사가 없는 곳: coverage 프래그먼트와 머리카락(기록에 인스턴스가 없다), surface cache 카드(카드의 인스턴스 채널을 넣지 않았다), FAR 타일 항.
- barn door가 없는 곳: 그림자 광선의 표본점(가려진 부분으로도 광선이 간다), FAR 타일 항, 공기·안개의 세기, 방출면 프록시.
- 소스 텍스처: diffuse와 specular가 조회 한 번을 같이 쓴다(원본은 각각). mip은 텍스처 시스템의 것이고 원본의 가우시안 프리필터가 아니다. 입자·공기·FAR 타일 항은 텍스처 없는 색이다. 텍스처를 처음 올린 프레임에 광원 버퍼를 다시 만든다(FX 광원이 그 프레임에 빠질 수 있다).
- `mega_lights` 켠 상태의 lit 입자는 안개와 같은 국소광 볼륨을 읽으므로 volumetric 스케일을 따른다(원본의 반투명 볼륨은 따르지 않는다). 프록셀 리스트 경로의 입자는 diffuse 스케일이다.
- `FxLayerSetup.STEP0.ML0.GIV0`은 구성요소 없이 컴파일한다(한도 204,800 B에서 256 B 아래). 머리카락은 specular 스케일을 따로 받지 않는다. 광선 hit의 specular 로브도 같다(레코드에 스케일이 하나다).
- 큰 커널(B, 한도 204,800): `ReflectionTraceInline.SKY0.JOB2.CORNERS1` 203,648, `CoverageComposite.PART1.*.AREA1` 199,460, `GiTrace.SKY0.SPLIT1` 193,968.

실행해서 확인할 것(순서대로): `unx_test_scene_lightcomponents`(CPU), 광원 레코드 크기가 바뀌었으므로 `unx_test_shading_shadingtests`·`unx_test_fx_fxlighttests`·`unx_test_raytracing_fxlighthits`, 그 뒤 항목마다 still(스케일 0과 2, 텍스처를 붙인 rect, barn door 각도 2개, 채널이 다른 두 물체, 거리 페이드 구간 안팎, 지수 2와 8).

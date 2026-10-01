# 조정 인계 (2026-09-27 저녁, 조정 세션 문맥이 가득 참)

새 조정 세션은 이 문서, GAME_PRODUCTION_HANDOFF_KO.md, FEATURE_STATUS_KO.md 맨 위 절들만 읽고 시작한다.

## 지금 상태

- **주간 사용량.** 약 95 %다(초기화 9/30 19:00). 클라우드 세션은 조직 설정 때문에 연결할 수 없다. 추가 사용은 꺼져 있다.
- **렌더러 저장소.** GitHub 비공개 원격을 연결했다: `github.com/Miigu-the-Smolder-wife/UnravelNext` (main 푸시됨).
- **게임 프로젝트 두 개.** `C:\Users\USER\UnravelGames\BathhouseTycoon`, `TrainExorcist`. 엔진 갱신은 `Tools/GameProject/Update-GameEngine.ps1 -Name <게임>`으로 한다(해당 Unity를 닫은 뒤). 최신 배포는 Unravel 759f813f(렌더러 a7bc481)다.
  - BathhouseTycoon은 갱신을 받았다.
  - TrainExorcist는 사용자의 Unity가 열려 있어 보류 중이다.
- **엔진 흐름.** 렌더 A·B·C, 엔진 1·2 모두 멈춤이고 트리는 깨끗하다. 사용자가 렌더러 코드를 직접 고치겠다고 했다.

## 사용자 판정: 실패

최고 품질·최고 성능 지시에 대해 사용자가 판정했다. 실내 화질이 게임에 쓸 수준이 아니고, 성능은 목표의 3.5~4.6배다.

- **성능.** 기차 게임 Player 실측(e6fa5e8): 1080p 90 fps, 1440p 61 fps, 4K 36 fps. 목표는 4K 6.06 ms다.

## 확인된 문제 (코드 위치, 사용자에게 전달함)

1. **실내 GI 노이즈와 모자이크.**
   - `Config/quality/gi.toml`: rays_per_frame 500000, screen_probe_spacing_px 8(8×8 블록), 캐시 칸 약 16 px, history 32/256(큰 값이 오래 남음).
   - 화면 공간 디노이저가 없다. ShadeOpaque는 `view.giIrradiance`를 읽게 연결됐다(35c7d96). 나머지 커널은 아직이다.
   - 미검증 시도는 `Results/R/GiInterior/wip/*.patch`에 있다.
2. **욕탕 벽 곰팡이 얼룩.** 반사와 GI를 꺼도 남는다. 직접광(면광원 14개 LTC와 국소 그림자) 또는 게임이 생성한 타일 텍스처 탓이다. 확인 방법은 `shading.experiment_disable=32`와 텍스처 자체 확인이다.
3. **반사 흰 네모 블록.** 면광원이 캐시 텍셀을 거쳐 K 경로에 찍힌다(emitters=false면 사라짐). `GiCache.hlsli` 484·497행의 `giCacheRadiance`가 coneHalfAngle을 쓰지 않는다.
4. **욕조 물 반짝이.** 평면 반사 보조 뷰에 시간 누적이 없어 면광원 표본 잡음이 실린다.
5. **성능.**
   - FixedUpdate 안에서 GPU 완료를 동기로 기다린다(해상도 비례 11~24 ms, EnableVfxGpu 또는 World tick 의심).
   - 4K 상위 패스: m.lit.shade 5.37, r.refl.shade 4.34, s.vsm.raster 1.70 ms.
6. **Player 결함.**
   - `UnravelNextScene.cs` 221·227행: 정적 배칭한 메시가 안 그려진다.
   - 색 보정 LUT를 편집기 경로에서 읽는다.
7. **그 밖.** GI 캐시가 벽을 넘어 새는 약 3 %, 해상도 조절 재현 미확인(파이프라인은 camera.pixelWidth를 따름), 게임 욕조의 물결 원천 미연결.

## 조정 교훈 (기억에도 있음)

- 숫자가 아니라 사용자가 볼 화면(실내, 움직임, 1080p·1440p)을 눈으로 보고 판정한다. 개선 수치를 먼저 말하지 않는다.
- 세션 문맥이 30만 토큰을 넘으면 비우고 재개 문서로 새로 시작한다.

## 클라우드 세션 규칙 (사용자 지시 2026-09-27 밤)

- **목표.** 압도적 최적화, 렌더 문제 해결, 이미 있는 기능의 완성이다. 새 기능은 만들지 않는다.
- **품질.** 품질·표본 수를 낮추거나 회피 구현을 하지 않는다. 판정은 사용자가 볼 화면(실내, 움직임, 1080p·1440p·4K)을 눈으로 보고 한다.
- **검증.** 클라우드에는 GPU·D3D12·Windows가 없다. 코드를 고치고 HLSL은 가능하면 리눅스 dxc로 컴파일만 확인한다.
  - 작업은 브랜치 `cloud/render-fixes`에 올린다.
  - 커밋마다 로컬 확인 항목(장면, 해상도, 기대 결과)을 이 파일 끝 "로컬 확인 대기"에 적는다.
  - 로컬 세션이 빌드·측정·캡처로 확인한다.
- **Unreal 참고.** Unreal Engine 소스(EpicGames/UnrealEngine)는 알고리즘과 구조를 이해하는 참고용으로만 쓴다. Lumen, Nanite, VSM, 디노이저, TSR 등이 대상이다. Unreal EULA 때문에 코드는 한 줄도 복사하지 않는다. 참고한 개념과 파일 경로만 기록하고, 구현은 우리 설계와 코드로 새로 쓴다.

## 클라우드 세션 1 결과 (2026-09-27 밤, 브랜치 cloud/render-fixes)

GPU가 없어 **모두 미검증**이다. HLSL은 리눅스 dxc 1.8.2505(빌드와 같은 플래그)로 481 커널 전부 컴파일되고 최대 DXIL은 195.8 KiB다(ReflectionTraceInline JOB2, 기준 194.8). 바꾼 C++는 clang과 DirectX-Headers(WSL 어댑터)로 문법 검사만 했다. 커밋마다 확인할 것은 아래 "로컬 확인 대기"에 있다.

- **화질**
  - 반사 흰 네모: GI 텍셀의 면광원 '방출 채널'을 없앴다. M은 거울 광선 픽셀이 아닌 곳에서 LTC로 계산하고, G 광선은 면광원을 보지 않는다. `giCacheRadiance`에 원뿔 사전 필터를 넣었다.
  - GI 모자이크·얼룩: 프로브 근접 차폐를 시간 누적하고, 캐시 조도에 공간 필터를 걸었다. 평면 반사 뷰도 같은 조회와 필터를 쓴다.
  - 욕탕 벽 얼룩 후보: 국소 그림자 탭마다 쌍선형 PCF를 쓴다.
- **성능**
  - FixedUpdate 동기 대기: FX 틱 펜스를 틱 패스 끝으로 옮겼다. 예상 4K 28 → ~22 ms.
  - M: 셰이딩 gather를 생략하고, B2 판정은 첫 안정 면광원을 만날 때 읽는다.
  - GI 조회: 지도 평가를 Load4로 읽고, r.gi.screen에서 쿼드 공유를 한다.
  - 반사 hit: 불필요한 적재를 뺐다. 반사 누적에 정지 인스턴스 지름길을 넣었다.
- **하지 않은 큰 항목 (다음 세션 권고)**
  - VSM 페이지 캐시: S의 "one path"는 요청된 페이지를 모두 매 프레임 그린다. 그래서 정지 카메라에서도 4K s.vsm.raster가 1.7 ms로 일정하다. 정적 페이지 재사용은 S가 의도적으로 뺀 경로라 원격에서 되살리지 않았다. 이득은 raster + pagemax 약 2 ms [예상]이다.
  - 반사 비용은 광선 수 구조가 정한다. 4K에서 반사가 약 7 ms이고, M 경로는 픽셀당 광선 1개다. 해상도 비례 구조 결정이 필요하다.
  - GI 캐시가 굵은 칸을 통해 벽 너머로 새는 몫이 3 %다. 같은 법선 축의 안팎 표면이 한 칸을 공유하기 때문이며, 가시성 판정이 필요하다.
  - 렌더 그래프 계획이 600프레임 중 575번 재컴파일된다. 버퍼 크기가 매 프레임 바뀌는 것으로 의심되며, CPU 몫이다.
  - C# 결함(정적 배칭, LUT 경로)은 이 저장소 밖이다.
- **Unreal**: 이 환경에는 Unreal 소스가 없어 열람하지 못했다. Lumen 화면 프로브의 시간·공간 필터와 VSM 캐시 개념만 참고했고(파일 경로는 커밋 메시지에 적음), 코드는 복사하지 않았다.

## 클라우드 세션 2: 4K 6.06 ms와의 거리 (2026-09-27 밤)

기차 4K 실측(e6fa5e8, GPU 21.7 ms)을 영역별로 묶어 설계 예산(ARCHITECTURE 2.13 실내 열, COVERAGE 12.4)과 대조했다.

| 영역 | 실측 4K | 설계 예산(실내) | 비고 |
|---|---:|---:|---|
| 반사(shade·accumulate·trace·resolve 등) | ≈ 7.1 | 0.82 | 광선 수 구조가 정함: M = 거칠기 < 0.14면 픽셀당 1광선, G = 흐림 < 3 px면 픽셀당 4광선 |
| M 셰이딩·해석·가장자리·후처리 | ≈ 7.3 | ≈ 1.5 | m.lit.shade 5.37에는 당시 캐시 조회 ≈ 2.4가 들어 있었다(지금은 r.gi.screen) |
| S(VSM raster·pagemax·froxel·가시성) | ≈ 4.5 | ≈ 1.2 | VSM은 설계대로 매 프레임 전부 다시 그림("one path"). 기차가 움직이면 캐시도 무효 |
| GI(갱신·프로브·screen) | ≈ 0.9 + r.gi.screen 2.4 | ≈ 0.6 | screen 조회는 설계 D 예상 0.15~0.2 |

- **세션 2에서 바로잡은 것.** 세션 1의 흰 네모 수정이 설계의 "필수 레버"(실내 면광원 스펙큘러 −3.47 ms)를 되돌렸다. 이를 면광원 전용 텍셀로 복원했다(1c85158; 설계 R 계약 그대로: RGB9E5 256 B/항목, +51 MB).
- **품질·표본 규칙 안에서는 6.06 ms에 닿지 않는다.** 남은 차이를 없애려면 아래 넷 중 하나 이상이 필요하다. 모두 [예상]이고 사용자 결정 사항이다.
  1. 4K 반사 광선 정책. M을 거칠기 < 0.05로 좁히고 나머지는 G로 보낸다. G 최소 간격은 해상도에 비례시킨다(4K에서 2 px). 시간 누적으로 보완. −2~3 ms.
  2. 기차에 붙은 국소광의 그림자 페이지를 기차 좌표계에 캐시한다. 광원과 캐스터가 함께 움직이면 깊이가 불변이다. −1~1.5 ms.
  3. r.gi.screen을 프레임 간 분할한다. 픽셀의 1/4씩 갱신하고 나머지는 정확 재투영으로 재사용한다. 조명 변화 지연은 최대 3프레임. −1.5 ms.
  4. 내부 해상도 + 시간 업스케일(TSR 류). 1440p에서 4K로 올리면 픽셀 비례 작업이 −55 %다. 새 기능이며 품질 정의가 바뀐다.
- **사용자 결정 2026-09-28.** 4번 채택(545bd74, 내부 1440p → 출력 4K 유지). 1~3번은 승인되지 않음: 표본 수·지연을 바꾸지 않는 구조 최적화만 한다. 세션 3 우선순위: (1) 렌더 그래프 재컴파일(600프레임 중 575회, CPU) (2) 반사 7.1 ms(같은 광선 수에서 trace·hit 셰이딩·적재·일관성) (3) M 셰이딩 7.3 ms (4) r.gi.screen 2.4 ms(설계 0.15~0.2) (5) VSM 정적 페이지 재사용(해가 무제한 속도로 움직여 전부 다시 그리는 최악도 예산 안) (6) GI 캐시가 벽을 넘어 새는 3 %. 모든 수치는 내부/출력 해상도를 함께 적는다(예: 1440p→4K).

## 클라우드 세션 3 결과 (2026-09-28, 브랜치 cloud/render-fixes, 모두 미검증)

기준 실측(e6fa5e8 기차 Player, GpuLock): 1080p 12.8 → 1440p 12.77 ms, 4K 21.7 ms. 업스케일 채택으로 4K 출력의 내부 해상도는 1440p이므로 **4K 출력의 기준선은 1440p 열(12.77 ms)** 이다. 1440p 상위 패스: r.refl.shade 3.77, m.lit.shade 2.25, s.vsm.raster 1.12, m.resolve 0.51, r.gi.trace 0.41, r.refl.accumulate 0.34.

| 항목 | 커밋 | 무엇 | 기대 [예상] |
|---|---|---|---|
| (1) 그래프 재컴파일 | 6d147fe, 62abcf6 | 버퍼 용량 버킷, 계획 8개 캐시, FX 틱 구조 고정, 반사 카메라·회전 맵 크기 단계화, 재컴파일 원인 로그 | plansCompiled 575/600 → 워밍업 뒤 ≈ 0 (CPU) |
| (2) 반사 | 0d1a394, d2065d1, 39fd454 | hit의 VSM 반영 필터를 조밀한 별도 패스로, GI 캐시 모서리 조회 일괄 발행, VSM 거주 판정 일괄 적재 | r.refl.shade(1440p 3.77) 감소 — 양은 측정해야 함 |
| (4) r.gi.screen | d2065d1 | 모서리 8개의 탐사·갱신·앵커·지도 적재를 묶어 발행(레벨당 종속 왕복 ≈ 30 → ≈ 4) | 지연 한정 커널이면 수 배 [예상], 점유율 손해면 N = 1로 비교 |
| (5) VSM | f433ae2, dda956d | 태양 페이지 캐시(바뀐 캐스터만 무효, 새 페이지만 지우고 그림), 전역 변화 시 one path | 정지·느린 장면 s.vsm.raster·pagemax 대부분 제거, 기차는 창밖 원거리 페이지 몫 |
| (6) GI 누설 | be1b7bf | 앵커가 조회점을 못 보면(텍셀 평균 hit 거리) 그 모서리 제외 | 벽 밑 햇빛 3 % 누설 제거 |
| (3) M 셰이딩 | 없음 | 셰이딩 커널은 이미 타일 LDS·선행 발행 구조. 계측(항별 귀속) 없이 바꿀 근거를 찾지 못함 | — |

- **측정 순서 제안(조정 세션):** 기차 내부 1440p → 출력 4K에서 A/B/A/B. 각 항목은 설정으로 끌 수 있다: `shadow.vsm.cache=false`, `gi.anchor_visibility=false`. 반사 penumbra 분리와 GI 일괄 발행은 코드 변경(커널별 `GI_CORNER_BATCH` 정의를 지우면 순차 형태).
- **M을 위해 필요한 계측:** m.lit.shade를 항별로(`shading.experiment_disable` 비트: 면광원·국소광·K 경로·태양·공기) 1440p에서 재면, 구조 변경(예: 면광원 LTC를 타일 단위 광원 목록으로 거르기) 여부를 판단할 수 있다.

## 로컬 확인 대기

(클라우드 세션이 커밋마다 한 줄씩 추가한다)

- **[반사 흰 네모 · B2 면광원 경로]** 목욕탕 1080p·1440p·4K, 바닥·벽 반사와 욕조 물의 평면 반사, 카메라 정지 + 이동. 기대: 반사 속 흰 사각 블록 소멸, 광택 타일에 비친 조명 하이라이트가 둥근 GGX 모양(LTC), 평면 반사 속 조명 반짝임 감소. 원인: GI 텍셀(8×8, ~20°, 갱신당 광선 1개)이 면광원 복사휘도를 '방출 채널'로 저장 → K 경로·평면 뷰·반사/GI hit가 텍셀 모양 사각형으로 읽음, hit에서는 NEE 스펙큘러와 이중 계산. 수정: GiTrace 텍셀에 면광원 0, M은 R 결과가 M(거울) 광선일 때만(a = 2) 안정 면광원 LTC 스펙큘러 생략, G 광선은 면광원 미포함(reflRayMask), 반사 누적 키에 경로 비트, 커버리지 조각은 마스크 없음(항상 LTC), `giCacheRadiance`에 원뿔 사전 필터(K 경로와 같은 mip 규칙). 시험: ReflectionAnalytic 6(거울 속 면광원, M 픽셀 a = 2), ShadingTests, GiAnalytic, PlanarMirror, Translucent, WaterShading. 비용 확인: r.refl.shade(G 광선이 발광체를 안 봄), 평면 뷰 셰이딩(원뿔 필터 텍셀 16개). 알려진 변화: 거친 유리(TranslucentComposite)·물의 캐시 대체 경로는 면광원 하이라이트를 더는 받지 않는다(두 경로는 원래 점광원 스펙큘러도 없음; 매끈한 유리·물결은 광선 경로라 그대로).
- **[GI 디노이즈 ① 근접 차폐 시간 누적 + ② 캐시 조도 공간 필터]** 목욕탕·기차 실내 1080p·1440p·4K, 정지 5초 + 천천히 이동 + 빠른 회전. 기대: 모서리·접촉부의 8×8 px 모자이크 소멸(정지), 이동 중 모자이크가 표면 위를 미끄러지는 현상 소멸, 벽·바닥의 칸 크기 얼룩 감소(노멀맵 음영은 유지). ① GiProbeGather: 16점이 프레임마다 황금비 회전, 프로브 표면점의 정확한 이전 위치(visId → 삼각형 → 이전 틱 정점, GiScreenHistory.hlsli)로 이전 프레임 프로브를 쌍선형(평면·법선 검사) 조회, 최대 16프레임 평균(`gi.screen_occlusion_history_frames`), 이력은 이번 추정 ±3σ로 클램프(움직이는 차폐물). 차폐는 기하만 의존 → 조명 지연 없음. 모든 소비자(M, 커버리지, 폴리지 뒷면)가 같은 값을 받음. ② r.gi.screen.filter(GiScreenFilter.hlsl): 칸 모서리의 0.75배(`gi.screen_filter_cells`) 반경, 12탭 황금각 나선, 평면(깊이의 2 %)·법선^32·데이터 가중; 자기 값이 없는 픽셀은 이웃 값으로 채움(a = 1). 확인할 것: 움직이는 캐릭터 옆 벽의 차폐 잔상(16프레임 이내로 사라져야 함), 카메라 컷 직후 한 프레임, 비용 r.gi.gather(+이력)·r.gi.screen.filter(4K 0.3~0.6 ms 예상), GiGate --lookup-stats의 screen 비교(필터 전 원본과 비교하도록 연결), GiAnalytic ProbeTileCompare, ShadingTests.
- **[평면 반사 뷰도 디노이즈 조도]** 욕조 물·거울 바닥의 평면 반사(목욕탕) 1080p·4K. 기대: 반사 속 벽·바닥의 GI가 메인 뷰와 같은 모양(칸 얼룩 필터·부분 칸 보정), 반사 경계에서 밝기 단차 감소. 평면 뷰(renderView)에 `tracks::giScreenIrradiance`를 머티리얼 해석 뒤에 넣어 r.gi.screen.planar + r.gi.screen.filter.planar를 돌리고, ShadeOpaque PLANAR은 P[9].w가 있으면 그 값을, 없으면 기존 직접 조회. 비용 확인: `*.planar` 두 패스(거울 타일 밖은 깊이 0이라 즉시 반환). CoverageShade 조각은 band A가 아닌 다른 표면이라 이 텍스처를 읽을 수 없음(시간 누적된 프로브 차폐는 공유) — 그대로 둠. edge 합성은 셰이딩 커널이 남긴 복사휘도를 쓰므로 이미 디노이즈 값 사용.
- **[M 셰이딩의 프로브 gather 생략 · 성능]** 기차 4K·목욕탕 4K에서 m.lit.shade.b0 시간(GpuLock, A/B/A/B)과 화면 동일성. r.gi.screen.filter가 메인 뷰 출력에 프로브 근접 차폐까지 곱해(E·occ, 같은 footprint 함수·같은 입력) M은 텍스처에 값이 있는 픽셀에서 프로브 gather를 K 경로 radiance·폴리지 뒷면에만 쓴다(SH 4개 평가·차폐 생략, 비K·비폴리지 픽셀은 gather 없음). 기대: 화면은 이전 커밋과 같음(차폐를 곱하는 위치만 이동, fp16 반올림 차이 수준), m.lit.shade 감소, r.gi.screen.filter는 footprint 8 로드만큼 증가. 확인: ShadingTests(특히 GI 관련 항목), GiAnalytic ProbeTileCompare(기본 인자라 동일해야 함), `gi.screen_filter_cells=0`에서도 차폐가 적용되는지.
- **[욕조 물 반짝이]** 평면 반사 뷰의 '면광원 표본 잡음'의 원천은 평면 뷰 ShadeOpaque가 안정 면광원 스펙큘러를 LTC 대신 GI 텍셀 방출 채널(갱신당 텍셀 광선 1개의 이동 평균)에서 읽은 것이었다. 첫 커밋(b0e7f40)으로 평면 뷰는 모든 면광원을 LTC(결정적)로 셰이딩한다 → 시간 누적 없이 원천 제거. 확인: 목욕탕 욕조 정지 카메라 10초, 반사 속 조명 하이라이트가 프레임 간 고정인지. 남으면 그때 평면 뷰 누적을 설계(재투영은 거울 카메라 기준).
- **[욕탕 벽 얼룩 · 국소 그림자 필터]** 먼저 가르기: 목욕탕 벽 정지 캡처를 `shading.experiment_disable=2048`(국소 그림자 슬롯 전부 1), `=32`(국소광 전부 끔), 원본 세 장으로 비교. 2048에서 얼룩이 사라지면 국소 그림자, 32에서만 사라지면 LTC 직접광, 셋 다 남으면 텍스처. 코드 원인(VsmLocalSample.hlsli): 반영부 필터 16탭이 고정 해바라기 패턴으로 선택 mip의 텍셀 하나를 점 샘플·이진 비교 → 가시도가 텍셀 격자 위 계단 16개의 합 → 벽에 고정된 텍셀 크기 얼룩(갓 안 전구 = 넓은 반영 = 거친 mip = 큰 얼룩), 경광원 경로도 ±0.5 텍셀 점 4개(1/4 계단). 수정: 탭마다 2×2 쌍선형 PCF(`vsmLocalTapOcclusion`, 페이지 내부는 GatherRed 1회, 페이지 경계는 텍셀별 페이지 조회), 경광원은 수신점 쌍선형 1회, 상주 페이지가 없는 탭은 평균에서 제외(밝음으로 세면 페이지 모양 밝은 패치). 기대: 2048 비교에서 차이가 나던 얼룩이 사라지거나 부드러운 반영으로 바뀜. 확인: ShadowTests·LocalShadow·VsmTests, 비용 s.shadow.visibility(탭당 텍셀 1 → Gather 1, ALU 증가), 폴백 타일(ShadeOpaque FALLBACK1). 남은 후보(에이전트 조사, 미수정): 차단자 탐색 5탭의 개수 변화로 반영 반경 점프(`shadow.vsm.search_taps=32`로 판별), 국소광 페이지 전파 없음(VsmPropagate가 국소 슬롯 건너뜀), 갓(얇은 천·유리)의 투과 없음 → 벽 직접광 0.86배의 후보.
- **[FixedUpdate GPU 동기 대기 · 성능 최대 항목]** 기차 Player 1080p·1440p·4K(EditorFps 게이트, GpuLock). 기대: `unityFramePeriodMs`가 `gpuFrameMs`에 붙음(4K 28.1 → ~21.7 ms, 1080p 11.1 → ~8.6 ms), `fixedMsPerFrame` 급감, 화면 동일. 원인: VFX 틱 readback(다음 고정 스텝의 prepare가 호출)이 기다리는 펜스를 대기 시점의 '큐 마지막 신호'로 정해(ParticleSystem resolveFence) 틱을 기록한 프레임 전체의 GPU 완료를 기다림 → CPU와 GPU 직렬. 수정: RenderGraph `PassBuilder::fenceAfter`(그 패스 뒤에서 명령 목록을 끊고 자체 신호, 제출 시 콜백으로 큐·펜스 전달; 계획 키에 포함), FX readback 패스가 이를 써서 슬롯 펜스 = 틱 패스 끝. 대가: 큐당 ExecuteCommandLists 1회 추가(~15 µs). 확인: ParticleTests·MeshParticleTests·FxLightTests·Host 스트림 시험(HostParticleLight --stream), 그래프 계획 재사용(stats.planReused 유지), 틱 이벤트 결과 동일.
- **[r.refl.accumulate 정적 인스턴스 지름길 · 성능]** 기차·목욕탕·city 4K에서 r.refl.accumulate(기차 4K 0.813 ms) A/B. 기대: 움직이지도 변형되지도 않는 인스턴스(`deformInstanceStill`: morph·스킨·바람 없음, 이전 변환 = 현재 변환)의 픽셀은 삼각형·정점 3개 적재·변형 없이 자기 점을 이전 위치로 씀 → 대부분의 픽셀에서 적재 감소. 화면 동일(재구성 점이 깊이 복원 점으로 바뀌어 ulp 수준 차이만). GI 프로브 이력(GiScreenHistory)도 같은 지름길. 확인: 움직이는 기차 안(카메라가 기차와 함께 이동: 기차 인스턴스는 변환이 바뀌므로 정확 경로), 반사 누적 상태 진단(`reflection.experiment_disable=1024`)이 이전과 같은 분포인지.
- **[GI 조도 지도 평가 Load4 · 성능]** city·기차 4K에서 r.gi.screen(단독 2.44 ms), r.refl.shade, r.gi.trace A/B와 비트 동일성(GiGate `--lookup-stats` screen 비교 0 차이, GiAnalytic ProbeTileCompare 0 차이). giIrrMapAt의 Catmull-Rom 4×4가 텍셀마다 4 B 적재 16회 → 행마다 Load4 4회(x0 = clamp(i0.x − 1, 0, 5)에서 클램프된 열이 항상 x0..x0+3 안: 전수 확인). 같은 텍셀·가중치·순서라 결과 비트 동일. DXIL 정적 적재 GiScreenIrradiance 45 → 33.
- **[B2 판정 읽기 지연 · 비용 주의]** 첫 커밋의 '픽셀 R 결과가 M 광선인가' 읽기를 광원 루프 앞에서 첫 안정 면광원을 만날 때로 옮김(M_STATUS 2026-09-25: 반사 읽기 선행 발행이 점유율을 떨어뜨려 1.26 → 1.50 ms). 비용 주의: 안정 면광원의 스펙큘러 LTC가 이제 비거울 픽셀마다 평가됨(이전엔 캐시 텍셀에서 공짜로 읽던 몫) → 면광원이 많은 목욕탕·기차에서 m.lit.shade 증가 가능. 기차 4K m.lit.shade를 첫 커밋 전후로 재고, 증가분을 화질 이득(흰 네모·반짝이 제거)과 함께 판정.
- **[반사 hit 캐시 조회의 불필요 적재 제거 · 성능]** 기차·city 4K r.refl.shade A/B, 화면 동일(비트 동일이어야 함). giKeepRead가 기여 항목마다 '어린 항목' 판정용 적재 2개(에폭·이력)를 했는데 그 합(g_giReadYoung)은 GiTrace의 되튐 대체 조회에서만 쓰임 → g_giTrackYoung 플래그로 그 조회 동안만 계산. 반사 hit당 최대 16 적재 감소.
- **[r.gi.screen 쿼드 공유 · 성능]** city·기차·목욕탕 4K r.gi.screen A/B, GiGate `--lookup-stats`(screenFlagDiffers 0, screenOver 0: 필터 전 원본 대 픽셀별 조회). 쿼드 4레인이 같은 레벨·셀 원점·법선 축이면 모서리 8개의 해시 조회(모서리당 종속 적재 3개)를 레인당 2개씩 하고 QuadReadLaneAt으로 교환(노멀맵 표면에도 적용), 법선 비트까지 같으면(노멀맵 없는 면) 모서리 지도 평가도 레인당 2개. 각 레인은 자기 삼선형 가중치로 giScreenLevel과 같은 순서로 합산 → 같은 값(컴파일러 축약에 따른 부동소수 반올림 차이만 가능). 확인: 쿼드 연산이 컴퓨트에서 정상(SM 6.6), 화면 경계 픽셀.
- **[세션 2 · B2 레버 복원 + 면광원 전용 텍셀]** (b0e7f40·dae1d73의 M·R 쪽을 대체) 설계서 ARCHITECTURE 2.13 "12.4 구조 2 — 필수 레버": 실내에서 정적 면광원 스펙큘러를 반사 경로로 넘겨 −3.47 ms. b0e7f40은 흰 네모를 고치려 비거울 픽셀마다 LTC 스펙큘러를 되살려 이 레버를 되돌렸다. 이번 구조: GI 캐시에 **면광원 전용 텍셀 배열**(항목당 64 × RGB9E5 = 256 B, 20만 항목 51 MB, 버퍼 끝 `giEmitterOffset`)을 두고, GiTrace가 네 번째 표본 블록으로 광원 복사휘도를 넘기면 GiIntegrate가 같은 이동 평균으로 섞는다. K 경로 지도(GiProbeMaps) = 텍셀 + 면광원 텍셀 → 메인 뷰는 원래대로 안정 면광원 LTC 스펙큘러를 생략하고 G/M 광선은 광원을 본다. 광선 hit(GI·반사)는 면광원 없는 텍셀 + NEE(사각형·이중 계산 없음), 평면 뷰는 LTC(캐시 텍셀만, 원뿔 필터), 유리·물 대체 경로는 원뿔 필터된 텍셀 + 면광원 텍셀. 기대: 기차·목욕탕 4K m.lit.shade가 b0e7f40 이전 수준으로(면광원 스펙큘러 LTC 몫 제거), 흰 네모는 평면 반사·반사 속 광택면에서 없음. 확인: ReflectionAnalytic 6, GiAnalytic 발광판, 캐시 메모리 +51 MB(로그의 GI cache MB), 목욕탕 거친 면의 조명 하이라이트(K)가 둥글게 번진 모양인지(사각형이면 K 밉 선택을 올려야 함).
- **[아지랑이·매질 패스 생략 · 성능]** 기차 4K(아지랑이 없음): m.distortion(0.277 ms)과 volume.haze.*, volume.media.*가 사라지는지, M이 OUTPUT0 커널(최종 4 B 직접 쓰기)로 바뀌는지(distortionActive가 false라 linear 경로가 빠짐; 후처리·모션 블러가 꺼져 있을 때). FX 프로그램 표에 NV_DISTORTION(5)/NV_VOLUME(3) 출력이 없으면 VolumeTrack이 해당 패스를 기록하지 않는다(ParticleSystem::hasProgramOutput). 확인: 아지랑이·연기 입자가 있는 장면(FX 게이트)에서 그대로 나오는지, VolumeTests·PostTests.

- **[내부 해상도 + 시간 업스케일 · 성능 최대 레버, 사용자 결정 2026-09-28 (9/27은 출력 해상도 선택만)]** `output.render_height_max = 1440`(0이면 꺼짐), `output.upscale_history_frames = 24`. 출력 높이가 1440을 넘으면 메인 뷰가 2560 × 1440(4K 픽셀의 0.44)에서 Halton(2, 3) 부화소 지터로 렌더되고(FrameRenderer::setupUpscale, 주기 ceil(8 × 출력/내부 픽셀) = 18), M의 셰이딩 체인(아지랑이·모션 블러·피사계 심도 포함) 뒤에 두 패스가 4K를 복원한다: m.upscale.motion(내부 해상도, 표본의 정확한 이전 표면점 → 이전 프레임 비지터 UV, GiScreenHistory 재사용), m.upscale(출력 해상도, 3 × 3 표본 가우스 창·YCoCg 상자 클립·가장 가까운 깊이의 모션·Catmull-Rom 이력·노출비). 결과(RGBA16F, 영속 핑퐁)가 후처리 체인 입력 + 다음 프레임 이력. 캡처(outputLinearHdr)와 `debug.view` 버퍼 시각화는 원래 해상도 유지. 모든 이력(R 반사·GI 프로브)은 지터된 이전 viewProj로 재투영(이전 프레임의 지터된 표본에 정확히 착지); 모션 블러는 비지터 행렬과 지터 변화량 보정 속도를 씀. 디버그 오버레이는 출력 크기 뷰로 그리고 깊이 판정은 내부 깊이를 비율로 읽음. **확인:** (1) 기차 4K GPU 프레임(기대 [예상] 픽셀 비례 작업 −55 %, 업스케일 자체 ≈ 0.4~0.6 ms), 1440p 이하에서 변화 없음. (2) 화질: 정지 5초 후 4K 네이티브 대비 선명도(가는 선·글자·타일 줄눈), 천천히 이동·빠른 회전 시 잔상(ghost)·떨림, 기차 창밖 빠른 배경, 파티클·유리·물(모션이 불투명 표면 기준이라 반투명은 클립에 의존). (3) 게이트: VisibilityGate·GiGate·MGate가 4K에서 main의 내부 해상도 산출물을 쓰는지(필요하면 게이트에서 `--set output.render_height_max=0`), HostScene, 모션 블러(MotionBlur 시험은 1440 이하라 무관). (4) 컷·해상도 변경 직후 한 프레임(리셋 경로). 품질이 부족하면 `render_height_max`를 1800으로 올려 비교.
- **[세션 3 · 렌더 그래프 재컴파일 ① 버퍼 용량 버킷 + 원인 로그]** 기차 Player 600프레임(내부 1440p → 출력 4K, 그리고 1080p 네이티브)에서 `RenderGraphStats::planReused` 비율과 `cpuCompileMs` 합. 계획 키가 임시 버퍼의 요청 크기를 그대로 해시해 광선 수·레코드 수·세그먼트 수로 크기를 정하는 버퍼(GI ray samples, 머리카락 세그먼트, 커버리지 레코드, 데칼, 물 광선 작업 등)가 한 바이트만 달라져도 재컴파일됐다. 수정(RenderGraph.cpp): 버퍼마다 용량 = 이전 계획의 용량(같은 id·이름, 요청 ≤ 용량 ≤ 2 × 요청이면 유지), 아니면 버킷(512 KiB까지 64 KiB 단위 — 배치 버퍼는 어차피 64 KiB 단위로 차지, 그 위는 옥타브당 8단계, 커질 때 +25 % 여유). 키·D3D12 Width·뷰 원소 수가 용량을 씀(커널 중 버퍼 GetDimensions 사용 없음 확인; `desc(buffer).size`는 요청 크기 그대로). 재컴파일 때 로그 `render graph: plan rebuilt (n): passes a -> b, resources c -> d; first differing pass i 'name' ...; resource j 'name' (x -> y bytes)`(처음 16회, 이후 100회마다). **확인:** 로그로 남은 원인을 모아 조정 세션에 전달, 메모리(transientBytesAliased) 증가가 수 % 이내인지, 버퍼 끝 넘어 쓰던 커널이 있다면 이제 조용해질 수 있음(GBV로 확인).
- **[세션 3 · 반사 hit의 태양 반영 필터를 따로 패스로 · 성능]** 기차·목욕탕, 내부 1440p → 출력 4K 및 1080p 네이티브. r.refl.shade(기차 1440p 3.77 ms, 4K 4.34 ms, e6fa5e8) + 새 r.refl.penumbra 합이 줄어드는지, 반사 화면 동일(fp16 저장 반올림 수준). 원인: hit 셰이딩이 S의 VSM 추정기(shadowSunVisibilityAt)를 hit마다 한 번에 돌려, 반영부 hit 하나만 있어도 파동 전체가 차단자 탐색 5탭 + 반영 필터 16탭(탭마다 페이지 조회 종속 적재)을 기다렸다. 수정: `shadowSunClassifyAt`(같은 거주 규칙·같은 레벨·vsmSunClassify, 투과율 포함)이 lit/umbra는 바로 값을 주고, mixed는 태양 항을 따로 두고(그림자 광선 경로와 같은 선형 분리) 점·기하 법선·레벨·reach를 큐에 넣는다. r.refl.penumbra(ReflectionPenumbra.hlsl)가 `shadowSunPenumbraDeferred`(= vsmSunPenumbra, 같은 입력)로 조밀한 파동에서 필터링해 값에 더한다. 큐는 기존 태양 큐(그림자 광선 영역)를 위에서부터 쓰고 법선·reach는 hit 레코드 자리(셰이딩 뒤 안 읽힘)에 넣어 메모리 추가 없음(광선 버퍼 머리 16 → 32 B). 셰이딩 커널 DXIL 137.5 → 121.8 KB(반영 코드 빠짐, rtHitRadiance 호출 3 → 2곳: 분리 값 계산 공유). **확인:** ReflectionAnalytic·반사 게이트(값 동일), `reflection.experiment_disable=2/4` 귀속(태양 가시도 몫이 이전과 같은지), 1440p·4K에서 r.refl.shade 감소량 대 r.refl.penumbra 비용, GBV(큐 인덱스·레코드 덮어쓰기).
- **[세션 3 · 렌더 그래프 재컴파일 ② 계획 캐시 + 구조 안정화]** 기차 Player 600프레임(내부 1440p → 출력 4K, 1080p 네이티브)에서 `plansCompiled`(e6fa5e8: 575/600, 1440p 600/600)와 로그 `render graph: plan rebuilt`. 원인 조사(모든 addPass 경로): 1순위 FX 틱 — 프레임마다 0/1/2 틱 묶음(묶음당 8~15 패스), 업로드 패스의 use 목록이 그 틱에 있는 섹션에 따라 바뀜, 그룹 0인 디스패치는 패스 자체가 빠짐. 2순위 평면 반사·물 반사 카메라의 사각형 크기(카메라가 움직이면 매 프레임 텍스처 크기 변화). 3순위 모션 블러 회전 맵 크기. 수정: (a) RenderGraph가 최근 계획 8개를 보관(현재 + 7, 최신 순; 키가 맞으면 그대로 재사용, 용량 버킷도 후보 계획 기준), 새 컴파일은 직전 계획과 같은 배치 리소스를 공유(뷰는 각자), 힙이 커지면 전부 해제. (b) FX 업로드는 모든 입력을 CopyDst로 선언, 그룹 0 디스패치도 패스는 기록(Dispatch만 생략). (c) 평면·물 반사 사각형 크기를 64 px 단위로(마스크가 거울 픽셀만 그리게 하므로 여분은 마스크 텍셀 비용). (d) 회전 맵 256 텍셀 단위 할당(커널은 상수의 실제 크기 사용). **확인:** plansCompiled가 워밍업 뒤 거의 0인지, cpuRecordMs/cpuCompileMs, GPU 시간 변화(빈 FX 패스의 장벽 몇 µs), 서술자 힙 사용량(계획 8개분 뷰), 평면 반사·물 반사 화면 동일, FX 게이트·ParticleTests·HostParticleLight --stream, GBV(계획 간 교대 시 별칭 장벽).
- **[세션 3 · GI 캐시 모서리 조회 일괄 발행 · 성능]** 기차·city·목욕탕, 내부 1440p → 출력 4K 및 1080p 네이티브: r.gi.screen, r.refl.shade, r.gi.trace A/B(GpuLock, A/B/A/B) + 비트 동일성(GiGate `--lookup-stats` screen 비교 0 차이, GiAnalytic ProbeTileCompare, ReflectionAnalytic). 원인(설계 12.3 비용식 T = 종속 왕복 × 지연 / 점유율): 삼선형 조회가 모서리 8개마다 해시 탐사 루프 → 갱신 수 → 앵커 → 지도(Load4 4개)를 차례로 기다려 레벨당 종속 왕복 ≈ 30회(hit 조회는 모서리당 5회 × 8)였다. 수정(GiCache.hlsli, 커널별 선택 `GI_CORNER_BATCH`): `giFindCorners`가 8 모서리의 홈 슬롯을 한 번에 적재(충돌 시에만 같은 탐사 순서로 이어감) → 갱신 수 한 번에 → (화면) 앵커 한 번에, 지도는 N개씩 무조건 적재(데이터 없는 모서리는 항목 0을 읽고 더하지 않음). 같은 모서리·값·합산 순서라 결과 동일. 적용: r.gi.screen(N = 4), 반사 hit 셰이딩·GI 추적(N = 2); M 셰이딩·커버리지 커널은 순차 형태 그대로(레지스터·DXIL 한도, 크기 변화 0). DXIL: GiScreenIrradiance 23.1 → 33.3, GiTrace 128.0 → 145.0, ReflectionShadeRays 121.8 → 138.5 KB. **확인:** 세 패스의 시간(레지스터 증가로 점유율이 떨어져 손해면 N = 1로 비교: 탐사·갱신 일괄만 남음), 결과 비트 동일.
- **[세션 3 · hit의 VSM 거주 판정 일괄 적재 · 성능]** r.refl.shade(와 굴절·유리·물 경로) A/B, 결과 비트 동일. shadowSunVisibilityAt·shadowSunClassifyAt의 "L + 1의 3 × 3 페이지 모두 거주" 판정이 단락 루프라 표 읽기 9회가 차례로 기다렸다 → `shadowPagesResident3x3`가 9개를 한 번에 읽고 AND(같은 답). `shadow.vsm.use_stats`(측정 전용)는 9개 모두를 읽음으로 센다. DXIL +2.9 KB(ReflectionTraceInline JOB2 198.2 KB: 한도 200 KB에 가까움 — 다음 변경 때 주의).
- **[세션 3 · VSM 태양 페이지 캐시 · 성능, 설계 선택]** `shadow.vsm.cache = true`(false = 이전 one path 그대로). S가 설계 개정 1 14.2④로 캐시를 뺐던 것을 사용자 결정으로 되살리되, 최악(해가 무제한 속도로 움직여 전부 다시 그림)은 one path와 같은 경로·비용이 되게 했다. 구조(VsmCache.hlsl, VsmScan.hlsl, VsmClearPages.ms.hlsl, VsmSystem.cpp): (1) 인스턴스마다 캐스터 상태(변환·변형 revision, 그림자·숨김 플래그)를 지난 프레임과 비교해 바뀐·나타난·사라진 캐스터와 매 프레임 움직이는 캐스터(바람·모프·지형 패치)의 지금·이전 경계 구를 목록에 올리고, 스킨은 V의 이번 프레임 posed 경계(`FrameResources::skinBounds`, V가 새로 게시; 무한 경계면 전체 다시 그림), (2) 그 구 아래의 상주 태양 페이지를 stale로, (3) 요청된 태양 페이지 중 지난 프레임에 같은 절대 페이지(tag)로 상주하고 stale이 아닌 것은 물리 페이지·내용·블록 계층·메타를 그대로 두고(kept), (4) 나머지만 결정적 순서의 빈 물리 페이지 목록에서 받아(아무것도 안 남기면 0, 1, 2, … = one path와 같은 배정) 메시 셰이더 사각형으로 그 페이지만 지우고(깊이 ALWAYS, 0) 래스터(컬 마스크 = DIRTY만)·pagemax. 국소광 페이지는 매 프레임 그림(기차·문과 함께 움직임). 전체 다시 그리기 조건: 해 방향, 캐스터 높이 범위(범위는 이번 프레임 캐스터가 안에 있는 동안 유지, 벗어나면 양쪽 25 % 넓혀 다시 그림), 장면 revision, 원점 이동, 복원, 아틀라스 재생성, 바람 속도·방향 변화, 비연속 프레임, 목록 용량 초과. 통계: `VsmStats::cachedPages`(kept), requested = kept + 새, dirty = 그린 페이지. **확인:** (1) VsmTests(캐시 켬: 정지 프레임 dirty 0·전부 kept, 카메라·캐스터 이동은 일부만 그림, 이동·느린 이동·해 변화·바람 뒤 기준 대비 비교 통과 = 남긴 페이지가 새로 그린 것과 같음; `--set shadow.vsm.cache=false`로 one path 단언도), ShadowGate(정지 카메라 dirty 0 전제가 다시 성립), LocalShadowTests·FroxelTests. (2) 기차 내부 1440p → 출력 4K·목욕탕 1080p: s.vsm.raster.raster(기차 1440p 1.12, 4K 1.70 ms)·s.vsm.pagemax·clear 감소량, 움직이는 기차에서 kept 비율(`cachedPages`/requested), 해가 움직이는 장면에서 one path 대비 추가 비용(캐시 패스 5개 ≈ 수십 µs [예상]). (3) 화면: 움직이는 캐릭터·기차·문·바람 식생 그림자에 잔상(stale 그림자)이 없는지, 캐릭터가 숨거나 나타날 때. GBV.
- **[세션 3 · GI 캐시가 벽을 넘어 새는 몫 · 화질]** `gi.anchor_visibility = true`(false = 이전과 같음; GiHeader.flags bit 1). 목욕탕·기차 실내, 해 켬, 1080p·내부 1440p → 출력 4K: 벽 아래·바닥 가장자리의 햇빛 번짐(R_STATUS의 햇빛 3 %)이 줄었는지, GiInterior 하네스의 기준 대비 편향(목욕탕 −1.8 %, 기차 −10.6 %였음)·타일 P95. 원인: 방 바닥과 벽 밖의 땅처럼 같은 법선 부류의 두 면이 굵은 칸 하나를 공유하면, 칸의 앵커(밖)가 본 빛이 안쪽 조회에 섞였다. 수정(GiCache.hlsli `giAnchorSeesPoint`): 항목 텍셀마다 이미 저장된 광선 평균 hit 거리(GiIntegrate, 텍셀 half 3; GiInvalidate가 쓰던 값)로 앵커→조회점 방향의 텍셀을 읽어, 거리 > 1.5 × 평균 hit 거리 + 칸의 1/4이거나 앵커 면 아래 45° 넘게 있으면 그 모서리는 "데이터 없음"(화면 조회의 짝 규칙·레벨 채움이 없는 칸처럼 메움). 적용: 화면 조회(r.gi.screen 쿼드 경로·순차 경로, 레인마다 자기 점으로 판정), hit·프로브 조회(giAccumulateLevel 두 형태). 새 저장 없음, 모서리당 적재 2개 추가(앵커 위치, 텍셀 1). DXIL: M 커널 +2~3 KB, ReflectionTraceInline JOB2는 거주 판정 루프 형태로 196.8 KB. **확인:** 실내 벽 아래 밝은 띠가 사라지고 방 안 GI가 어두워지지 않는지(정상 면에서 거절이 과하면 GiAnalytic 백색로·열린 하늘 시험이 떨어짐 → 1.5 배율·1/4 칸 여유를 조정), r.gi.screen 비용 증가량, 문·창 가장자리의 경계선.
- **[세션 3 · VsmTests MSVC C4310 빌드 실패 수정]** VsmTests.cpp 298행: 상수 `~(...)`를 uint8_t로 캐스트하면서 잘리던 식을 마스크 뒤 캐스트로 바꾸고 복합 대입을 명시적 대입으로 바꿈(`t[i + 3] = (uint8_t)(t[i + 3] & (0xFFu & ~(...)))`). 이 브랜치에서 바뀐 C++ 줄 전부를 clang으로 MSVC /W4 대응 경고(-Wall -Wextra -Wshadow-all -Wshorten-64-to-32 -Wimplicit-int-conversion -Wfloat-conversion -Wimplicit-float-conversion -Wsign-compare -Wunused -Wunreachable-code)로 다시 검사: 경고 0. C4310(상수 캐스트 잘림)은 clang에 대응 경고가 없어 추가한 줄의 좁히는 캐스트를 전부 눈으로 확인(이 한 곳뿐). 테스트·게이트 101개 파일은 구문 검사 통과(남은 오류는 리눅스 shim에 없는 Windows API뿐: LoadLibraryW, _wfopen_s, GetCurrentProcessId 등, 이번에 안 바꾼 파일). **확인:** MSVC /W4 /WX 로컬 빌드 전체(특히 C4127·C4245처럼 clang이 못 잡는 경고).
- **[세션 3 · 로컬 실패 2건 수정 (e08cc88 검증, RTX 4080)]**
  - **VsmTests "wind change" 635/1524.** 그 줄의 `shadow::stats()`는 읽어 온 가장 새 프레임, 즉 돌풍 *앞* 프레임(바람이 일정한 프레임)의 카운터였다. 캐시가 켜진 그 프레임은 흔들리는 캐스터 페이지만 그린다(635). 바람 변형 캐스터는 f433ae2부터 매 프레임 "바뀐 캐스터"다(VsmCache MODE0 `animated`: INSTANCE_WIND, 경계 구 = 메시 구 + windOffsetBound, 지금·이전 둘 다). 돌풍(장면 바람 속도·방향 변화)은 프레임 전체를 캐시 불가로 만든다(`sameWind`). 수정: 시험이 `statsAfter()`로 돌풍 프레임 자신의 카운터를 읽는다(5번 바람 구간도 마지막 바람 프레임의 카운터). 기대: 돌풍 프레임 dirty = requested.
  - **ReflectionAnalytic furnace (M 최악 4.22 %, G 최악 12.44 %).** 원인 후보를 코드로 좁혔다. 모서리 일괄 발행(d2065d1)과 VSM 거주 일괄(39fd454)은 같은 모서리·값·순서라 결과가 같다. 원뿔 사전 필터는 이 시험 경로(M/G hit 조회는 cone 0)에 없다. 반사 penumbra 분리(0d1a394)는 해가 꺼진 furnace에서 타지 않는다. 남는 것은 앵커 가시성(be1b7bf)이다. 균일한 furnace(모든 항목 L = 2)에서 일부 모서리 거절은 값을 바꾸지 못한다. 바꾸는 경로는 둘이다. (a) 어린 항목(텍셀당 광선 1~2개의 hit 거리)이 이웃을 무작위로 거절해 가중치가 아직 수렴 중인(Jacobi 단계, 어두운) 어린 칸으로 옮겨감. (b) 거절된 항목이 giKeepRead(유지·갱신 요청)에서 빠져 반사 hit만 보는 칸이 덜 갱신됨. 수정(GiCache.hlsli): 수렴한 항목(갱신 16회 이상, `GI_VIS_MIN_UPDATES`)만 "못 본다"고 판정; 데이터가 있는 모서리는 거절돼도 모두 giKeepRead; 한 레벨에서 전부 거절되면 그 레벨은 거절 전 모서리로(가시성은 모서리 사이 재가중만, 레벨을 비우지 않음); 화면 조회도 같은 규칙. 가시성 끔일 때 순차 형태는 브랜치 전 코드와 같은 연산이다.
  - **확인 순서:** ① ReflectionAnalytic 기본값. ② 여전히 실패하면 `--set gi.anchor_visibility=false`로 다시 돌린다. 통과하면 원인이 앵커 가시성이라는 확정이다(기준 거리 배율 1.5·최소 갱신 수 조정 또는 기본 끔). ③ 끄고도 실패하면 ReflectionShadeRays.hlsl·GiTrace.hlsl의 `#define GI_CORNER_BATCH`를 지워(순차 형태) 일괄 발행을 가른다. ④ 그다음 0d1a394(반사 셰이딩 꼬리 정리)를 가른다. VsmTests·GiAnalytic·ShadingTests 재확인. DXIL 최대 197.5 KB.
- **[세션 3 · 로컬 실패 3건 (395c777 검증, RTX 4080) + 악화 원인]**
  - **ReflectionAnalytic furnace (G 최악 12.44 → 15.63 %).** 395c777의 규칙이 오히려 나빠졌다는 결과가 원인 경로를 가리킨다: 그 커밋은 "어린 항목(갱신 16회 미만)은 항상 보임, 수렴한 항목만 판정"으로 바꿨는데, 이러면 수렴한 모서리가 하나라도 (잘못) 거절될 때 그 레벨의 가중치가 전부 아직 수렴 중인 어린 칸으로 넘어간다. 차가운 캐시(시험은 128프레임, 반사만 닿는 칸이 ~100프레임에 수렴)에서는 어린 칸이 낮게 읽히므로 M·G 화소가 낮게 나온다. 수정(GiCache.hlsli `giVisJudged`): **한 레벨에서 데이터가 있는 모서리가 모두 수렴했을 때만** 가시성을 판정하고, 어린 모서리가 하나라도 있으면 그 레벨은 판정 없이(브랜치 전과 같은 삼선형 합) 읽는다. 벽 너머 누출(방 바닥과 바깥 땅처럼 오래 산 칸 사이)은 그대로 막힌다. 순차·일괄·화면 조회 세 형태 모두 같은 규칙. **확인:** ① ReflectionAnalytic 기본값. ② 실패하면 `--set gi.anchor_visibility=false` 한 번(이번에는 꼭): 통과하면 원인 확정(기본 끔으로 바꾸고 누출 대책을 다시 설계), 실패하면 앵커 가시성이 아닌 것이 확정이니 `ReflectionShadeRays.hlsl`·`GiTrace.hlsl`의 `#define GI_CORNER_BATCH` 제거로 일괄 발행을 가른다. 로그의 "M outlier / G outlier" 줄(표면 좌표·NoV)을 같이 보내 주면 위치로 원인을 더 좁힐 수 있다.
  - **HostMotion "static: 180° shutter" · ParticleTests tick 137.** 코드로 확정 원인을 찾지 못했다(미해결). 확인한 것: oneBox 정지 장면(960×540)은 업스케일이 꺼지고(540 ≤ 1440), m_movedNow·바람이 없어 셔터 0.5 실행도 모든 프레임에서 모션 블러가 비활성이다. 즉 두 실행은 같은 GPU 작업이고, 실패는 "같은 입력 두 번이 다름"(경합·미초기화 읽기·타이밍 의존 결정)이다. 배제한 것: VSM 아틀라스 증가(4096페이지, 상자 하나로는 고갈 없음), 반사 광선 용량(2^20, 인라인 경로 미사용), 통계 워드 충돌(캐시 kept = word 6, free = word 4), 새 임시 버퍼(모두 읽기 전 기록), giIrradiance 선언(M 두 패스 모두 선언), 평면·물 없음. FX: 틱 펜스 콜백·슬롯 재사용·업로드 영역·import 캐시·영 그룹 패스의 장벽을 따라가 봤으나 잘못을 찾지 못했다. **로컬에서 한 번에 가르는 스위치를 넣었다:** `UNX_GRAPH_PLAN_CACHE=0`(렌더 그래프가 직전 계획 하나만 유지 = 계획 캐시 전 동작), `UNX_FX_FRAME_FENCE=1`(FX 되읽기가 틱 펜스 대신 프레임 펜스 = 6522f82 전 동작). **확인 순서(HostMotion, 수 초짜리):** ① 기본값 재실행(이번 furnace 수정 포함) ② `UNX_GRAPH_PLAN_CACHE=0` ③ Config/quality/shadow.toml `cache = false` ④ gi.toml `anchor_visibility = false`. 처음 통과하는 단계가 원인. **ParticleTests:** ① `UNX_FX_FRAME_FENCE=1` ② `UNX_GRAPH_PLAN_CACHE=0`. 이미 기록 기능이 있으니 실패 실행을 `--record DIR`로 남기면 `--replay`로 GPU 없이 여기서 원인 틱을 분석할 수 있다.
  - **r.gi.screen 0.77 → 1.49 ms (+ filter 0.39), r.refl.shade 1.76 → 2.11 ms, r.gi.trace 0.47 → 0.66 ms (기차, 내부 1440p · 출력 1440p).** 세 패스 모두 앵커 가시성(be1b7bf/395c777)의 모서리마다 추가 적재(갱신 수, 앵커 위치, 텍셀 거리, 법선 재적재)를 지는 조회 커널이다. 이번 수정: 이미 읽은 갱신 수를 넘겨 재적재 제거, 어린 모서리가 있는 레벨은 가시성 적재를 통째로 생략, 앵커 위치·법선을 Load4 한 번으로(가시성·방사휘도·조도 지도가 같은 값 공유; 조도는 `giIrrMapAt`로 같은 연산). 결과 값은 가시성 규칙 변경 외에 같다. **귀속 측정:** 같은 장면에서 `gi.anchor_visibility=false`로 세 패스를 다시 재면 가시성 몫이 나온다. 남는 몫은 일괄 발행(d2065d1)·쿼드 공유(def5809)의 레지스터/점유율 영향 후보(`GI_CORNER_BATCH` 제거로 비교). r.gi.denoise.it 0.71 → 0은 main의 M 쪽 잡음 제거가 r.gi.screen.filter(0.39)로 바뀐 것이라 합계는 1.48 → 1.88 ms.
  - 리눅스 dxc로 491 커널 전부 컴파일, 최대 DXIL 197.1 KB(ReflectionTraceInline SKY0 JOB2). 바꾼 C++(RenderGraph.cpp, ParticleSystem.cpp)는 clang 구문·경고 검사 0건. 모두 GPU 미검증.
- **[세션 3 · 20379fb 로컬 결과 반영 (욕탕 얼룩 · 조회 비용 · main에서도 실패하는 2건)]**
  - **욕탕 1080p 바닥의 밝은 푸른 얼룩 3곳 (main에 없음) — 원인과 수정.** 앵커 가시성으로 거절된 모서리를 화면 조회(r.gi.screen, `giCacheIrradianceScreen`)가 "없는 칸"으로 셌다. 없는 칸의 가중치는 법선 축 짝 모서리로만 옮겨지는데(바닥은 y 축), 벽 쪽에서 거절된 모서리는 짝도 대개 거절되므로 그 레벨의 가중치가 1보다 작아지고 나머지가 **다음(더 거친) 레벨**로 넘어갔다. 거친 칸은 벽을 넘어 바깥까지 걸치고, 어린 모서리가 섞이면 가시성 판정도 건너뛰므로 바깥 하늘빛(푸른색)이 들어왔다 — 막으려던 누출을 거친 레벨에서 다시 끌어온 것. 수정(`giScreenVisRenorm`): 거절된 모서리의 가중치는 같은 레벨의 보이는 모서리들 사이에 다시 나눈다(레벨 가중치는 거절 전과 같음 → 다음 레벨로 가는 몫은 main과 같다). 반사·GI hit 조회는 원래 레벨 안에서 정규화하므로 해당 없음. 원뿔 사전 필터는 확산 조회에 없고, 화면 필터·프로브 근접 차폐 누적은 셀 모양·푸른 색조를 만들 수 없다. **확인:** 욕탕 1080p 바닥(같은 캡처). 얼룩이 남으면 `gi.anchor_visibility = false`로 한 번 찍어 앵커 가시성이 원인인지 가른다. 벽 모서리가 main보다 어두운 것은 이번 수정 후 다시 봐야 한다(이 경로의 레벨 가중치 변화가 원인일 수 있음; 남으면 `gi.screen_occlusion_history_frames = 1`로 프로브 차폐 시간 누적을 가른다).
  - **r.gi.screen 1.44 / r.refl.shade 2.06 / r.gi.trace 0.66 ms (main 0.77 / 1.76 / 0.47, 기차 내부 1440p · 출력 1440p).** 세 커널의 공통 변경은 모서리 일괄 발행(`GI_CORNER_BATCH`, d2065d1)이고 r.gi.screen은 쿼드 공유(def5809)까지 있었다. 둘 다 [예상]만으로 넣었고 측정은 반대였으므로 제거했다: GiCache.hlsli의 일괄 코드(`giFindCorners`, `giScreenCells`, 일괄 `giAccumulateLevel`·`giScreenLevel`)와 세 커널의 정의를 지우고 main의 순차 형태로 되돌림, GiScreenIrradiance.hlsl은 main 파일 그대로 + 주석. 남은 추가분은 앵커 가시성(수렴한 레벨에서만: 모서리당 앵커 위치·텍셀 거리 적재)이고, 조도 지도의 행 단위 Load4(0b3ecfd, 모서리당 16 → 4 적재)와 앵커 위치·법선 Load4 공유로 상쇄되는 쪽이다 [예상]. **확인:** 같은 A/B/B/A로 세 패스. main 이하가 아니면 `gi.anchor_visibility = false`로 한 번 더 재서 가시성 몫을 보고, 그 몫이 크면 가시성을 화면 조회에서 텍셀 거리 대신 더 싼 판정으로 바꾸거나 기본 끔을 판단한다.
  - **HostMotion "static: 180° shutter" (main에서도 실패) — 원인과 수정.** 두 실행은 같은 GPU 작업(블러 비활성)이므로 비결정성이다. `gi.deterministic`은 "같은 입력 → 비트 동일 캐시(선택은 키 우선순위, 시드는 키)"를 약속하고 GiTrace는 광선 시드를 항목 키(`giDetPriority`)에서 뽑지만, **GiIntegrate의 확률적 반올림(디더) 시드는 항목 인덱스**였다. 인덱스는 항목을 만드는 스레드들이 빈 목록을 원자적으로 꺼낸 순서라 실행마다 다르다 → 캐시가 실행마다 반올림이 달라지고 그 캐시를 읽는 모든 화소가 달라진다. 수정: 결정론 모드에서 디더 시드도 키에서(GiIntegrate.hlsl, 비결정론 모드는 그대로). **확인:** hostmotion. 남으면 다음 후보는 결정론 모드에서 인덱스 순서가 들어가는 다른 곳(반사 hit가 만든 항목의 앵커 — GiDetFold가 담당)이다.
  - **ReflectionAnalytic furnace (main에서도 실패, G 최악 12.59 %).** 이번에는 고치지 못했다. 시험의 합격 조건은 평균 ±1 %와 "3 % + zσ 초과 화소 0개"이고 최악 값 자체는 조건이 아니라서, 어느 화소가 어떤 경로로 벗어나는지(시험 로그의 `M outlier` / `G outlier` 줄: 화소, 값, 기대, sigma, 허용치, 표면 좌표, 거칠기)가 있어야 원인을 좁힐 수 있다. 그 줄들과 `--frames 256` 한 번의 결과(수렴 부족인지 편향인지 가름)를 보내 주면 다음 세션에서 바로 본다. 참고: GiTrace 주석에 이 시험이 원래 통계적으로 흔들린다는 기록이 있다(균형 휴리스틱 도입 때 11회 중 6회 실패 → 2회).
  - **ParticleTests tick 137 (FRAME_FENCE=1, PLAN_CACHE=0 둘 다 같은 실패).** FX 시뮬레이션 커널(FxBegin·Emitters·Spawn·Integrate·Collide·ChildSetup·Grid·Surfaces·Ribbon 등)의 DXIL은 main과 **바이트 단위로 같다**(리눅스 dxc로 두 트리를 컴파일해 비교; 다른 것은 렌더 쪽 FxLayerSetup뿐). 두 스위치를 끈 상태에서 남는 호스트 차이는 업로드 패스가 모든 입력을 CopyDst로 선언하는 것과 그룹 0인 패스를 (디스패치 없이) 기록하는 것뿐인데, 둘 다 장벽만 더할 뿐 데이터를 바꾸지 않는다. 그래서 main에서도 같은 실패일 가능성이 높다. **확인:** main에서 `--ticks 160` 한 번. main도 실패하면 브랜치 이전 문제로 분리하고, `--record DIR` 결과를 커밋해 주면 기록된 패킷·되읽기로 원인 틱을 분석한다.
  - Config/quality/shadow.toml `cache` 주석과 VsmCache.hlsl 머리 주석의 "user decision 2026-09-28"을 "design choice"로 바꿈(사용자 결정은 업스케일 채택과 최적화 시작뿐).
- **[세션 3 · 1db06e4 로컬 결과 반영 (욕탕 얼룩·벽 점 · r.refl.shade · 국소광 페이지 캐시)]**
  - **욕탕 바닥 얼룩(욕조 앞·옆, 오른쪽 뒤)과 벽의 밝은 점 2개 → 앵커 가시성 기본 끔(`gi.anchor_visibility = false`).** 이미지로 보면 욕탕은 천장 면광원으로 밝히는 닫힌 방이라 얼룩은 하늘빛이 아니라 바닥의 밝은 조도 조각이고, 욕조 받침·벽처럼 *실제로* 가리는 물체 가까이에 셀 크기로 있다. 앵커 가시성은 거기서 모서리를 빼고 남은 칸에 가중치를 몰아주는데, 그 판정이 셀·레벨 단위로 켜지고 꺼지므로 셀 모양 불연속이 생기는 것이 이 방법의 성질이다(1db06e4의 레벨 내 재분배로 거친 레벨 누출은 줄었지만 이 몫은 남음). 벽의 점은 램프의 거울 위치(반사)에 있고, 반사 hit의 캐시 조회도 같은 가시성 재가중을 탄다. 다른 후보는 코드로 배제: 프로브 근접 차폐 누적(정지 카메라에서 자기 프로브로 정확히 재투영, 번짐 없음), 화면 필터(같은 평면 안의 평균이라 밝은 조각을 만들 수 없음), 원뿔 사전 필터(유리·물·평면 반사 조회에만 쓰임), 면광원 텍셀 분리(방출체 텍셀 + 텍셀 = main의 텍셀, 저장·읽기 배율 일치; M은 main처럼 안정 면광원 스펙큘러를 반사 경로에 맡김). **벽 너머 3 % 햇빛 누출 항목은 다시 열림**(가시성 방식으로는 얼룩 없이 못 막음; 다른 설계가 필요). **확인:** 욕탕 1080p 같은 캡처에서 얼룩·점이 main과 같아야 한다. 남으면 다음 후보는 `gi.screen_occlusion_history_frames = 1`(프로브 차폐 시간 누적 끔)과 `gi.screen_filter_cells = 0`(필터 끔)으로 한 장씩.
  - **r.refl.shade 2.05 ms (main 1.76, 기차 내부 1440p · 출력 1440p) — 원인과 수정.** 반그림자 지연(0d1a394) 뒤로 반그림자 hit마다 `rtHitRadiance`(hit 셰이딩 전체)를 두 번(가시성 0과 1) 불러 차로 태양 항을 얻었다. 분기라서 웨이브에 그런 hit가 하나만 있어도 웨이브 전체가 두 번째 셰이딩을 돈다(main은 화면 밖 hit만 그랬음). 수정: `rtHitRadianceParts`가 한 번의 평가에서 "가시성 적용 값"과 "가시성 1의 태양 항"을 함께 돌려준다(HitShading.hlsli; `rtHitRadiance`는 이를 부르고 연산 순서가 같아 **결과가 비트 단위로 같다**). 반그림자·그림자 광선 hit도 셰이딩 1회. 태양 항은 이제 정확한 값(두 fp 평가의 차가 아님). **확인:** 기차 1440p r.refl.shade(+ r.refl.penumbra 합)가 main 이하인지, 반사 속 그림자 경계 화면 동일.
  - **추가 요청(1440p·1080p 대폭 최적화, 품질 유지) 1단계 — 국소광 페이지 캐시.** 욕탕 내부 1440p(출력 4K 측정 기준) 상위가 s.vsm.localraster0/1 합 2.45 ms로, 국소광 그림자 페이지를 정지 장면에서도 매 프레임 전부 다시 그리고 있었다(태양 캐시 MODE 3은 태양 슬롯만). 태양과 같은 방식으로 국소광 페이지도 유지한다: 요청됨 + 지난 프레임 거주 + 태그 = 광원 세대(위치·범위·반경·종류·슬롯의 광원이 바뀌면 증가) + **광원 범위(farM) 안에 바뀐 캐스터 구가 없음**(새 MODE 6: 광원별 비트, 바뀐 캐스터 목록 용량으로 루프 상한). 유지된 페이지는 내용 그대로, 국소광 컬 마스크는 이번 프레임 새 페이지(DIRTY)만 그린다(VsmLocalCullMask, 태양 VsmCullMask와 같은 규칙). 전역 변화(해·범위·장면 개정·원점 이동·복원·아틀라스)나 캐시 끔(`shadow.vsm.cache = false`)이면 이전처럼 전부 그린다. 내용은 다시 그린 것과 같다(같은 광원·같은 캐스터) → 품질 변화 없음. 기대 [예상]: 정지 욕탕에서 localraster ≈ 0, s.vsm.clear 감소; 움직이는 캐스터(사람·문)가 있으면 그 광원만 다시 그림. **확인:** 욕탕 1440p·1080p A/B(localraster0/1, s.vsm.clear, s.vsm.cache.keep/locallights 비용), VsmTests 전체(국소광 시험의 "그린 페이지 수" 단정이 있으면 캐시 끔 기준으로 바뀌었는지), 화면 동일(국소광 그림자), 광원 이동·캐스터 이동 장면에서 그림자가 따라오는지.
  - 리눅스 dxc 492 커널 전부 컴파일(MODE 6 추가), 최대 DXIL 193.1 KB(ShadeOpaque LAYERED2; 반사 인라인 커널은 셰이딩 1회로 줄어 그 아래). C++ 구문·경고 검사 0건. GPU 미검증.
  - 다음 단계 후보(1440p·1080p): s.vsm.localmark 1.18 · s.shadow.overflow 1.05 · s.froxel.integrate 1.10 · s.shadow.visibility 0.88(욕탕 내부 1440p). 계측 없이 [예상]만으로 넣은 구조 변경이 두 번 느려졌으므로, 다음은 이 패스들의 항목별 비용(패스 내부 단계 시간)을 먼저 받고 고른다.
  - furnace(main에서도 실패, G 최악 13.18 %): 이번에도 원인 화소를 모른다. 로그의 `G outlier` / `M outlier` 줄(화소·값·기대·sigma·허용치·표면·거칠기)을 README에 붙여 주면 기대값 식(벽 hit의 Le + ρL + S(NoV)L, 캐시 L = 2, Lambert GI)과 G 경로(제어 변량 = K 지도)의 어느 쪽이 어긋나는지 바로 가를 수 있다.

## 세션 4 · 구조 최적화 (887b5d8 이후, 품질·표본·부하 그대로)

기준 실측(1db06e4, RTX 4080, renderergate 정지 600프레임, A/B/B/A 중앙값): 기차 내부 1440p·출력 1440p 9.93 ms, 기차 내부 1440p·출력 4K 10.42 ms, 욕탕 내부 1440p·출력 1440p 15.53 ms, 욕탕 내부 1440p·출력 4K 16.06 ms. 887b5d8(국소광 페이지 캐시, hit 셰이딩 1회)은 아직 미측정. 아래 커밋은 모두 **결과가 같은 구조 변경**(같은 요청·같은 탭·같은 값)이며 GPU 미검증.

- **[국소광 요청·탭 조회의 중복 제거 + 태양 페이지 테이블 워드 재사용]**
  - s.vsm.localmark(욕탕 내부 1440p 1.18 ms): 화소·광원·mip마다 중심 + 반 페이지 이웃 4개를 전부 큐브 투영해 요청했는데, 각 축의 두 이웃 중 하나는 항상 중심과 같은 페이지다(반 페이지 이동은 가까운 경계 반대쪽으로는 페이지를 넘지 못함). 그 쪽은 투영·저장을 건너뛴다(경계에서 1/1024 페이지 이내면 원래대로 둘 다) → mip당 투영 5 → 3, 같은 요청 집합. [예상] localmark −30~40 %.
  - 국소광 가시성(s.shadow.visibility·s.shadow.overflow·froxel이 공유, VsmLocalSample.hlsli): 탭마다 방향을 투영한 뒤 수신면 깊이(`vsmLocalPlaneDepth`)를 위해 같은 방향을 **다시 투영**했다 → 탭 자신의 투영 면으로 계산(`vsmLocalPlaneDepthAt`, 같은 값). 필터 탭(16)과 경광원 탭은 마지막으로 읽은 페이지의 테이블 워드를 재사용(대부분의 탭이 앞 탭과 같은 페이지: 의존 적재 제거). 차단자 탐색 탭도 재투영 제거. [예상] 국소광 가시성 비용 −20~30 %(욕탕 visibility 0.88·overflow 1.05 ms의 해당 몫).
  - 태양(VsmSample.hlsli `vsmEntry`): 스레드가 마지막 슬롯의 테이블 워드를 기억(정적 변수). 반그림자 탐색·필터 탭과 영역 분류가 대부분 같은 페이지를 연달아 읽는다. 사용 통계 측정(`shadow.vsm.use_stats`)일 때는 매번 읽음. [예상] s.shadow.penumbra(기차 내부 1440p 1.02 ms) −10~20 %, 반사 hit 태양 분류도 조금.
  - 확인: LocalShadowTests·VsmTests·ShadingTests 통과(값이 같아야 함), 욕탕·기차 화면 동일(국소광 그림자, 태양 반그림자), 욕탕 1440p s.vsm.localmark·s.shadow.visibility·s.shadow.overflow, 기차 1440p s.shadow.penumbra A/B.
- **[국소광 가시성: 광원 도달 거리 밖 화소는 추정 생략]** 프록셀 목록은 광원 구가 프록셀과 만나는지로 만들어져 화소마다 광원 범위 밖인 것도 섞인다. 그 화소에서 광원의 셰이딩 창 w(d)는 0이므로(shPunctualIlluminance·shAreaWindow: d ≥ range에서 0) 가시성은 아무것도 곱하지 않는다 → farM(= range + 발광체 반경) 밖이면 차단자 탐색·필터 없이 1(VsmLocalMark가 페이지를 요청하지 않는 화소와 같은 기준). 화면 동일. [예상] 욕탕 s.shadow.overflow·s.shadow.visibility의 국소광 몫에서 범위 밖 항목만큼(광원이 많은 목욕탕에서 큼). 확인: LocalShadowTests(시험은 범위 95 % 밖을 건너뜀), 욕탕 화면 동일, 욕탕 내부 1440p s.shadow.overflow·visibility A/B.
- **[반사 국소광 표본: BRDF가 0인 표본은 그림자 광선 없음]** r.refl.localshadow(기차 내부·출력 1440p 0.36 ms)는 반사 hit마다 국소광 표본 하나에 그림자 광선을 쏜다. hit의 셰이딩 법선이 표본 방향을 등지면(N·L ≤ 0) `rtLocalLightBrdfCos`는 잎(Foliage, 투과) 재질이 아닌 한 정확히 0이라 그 가시성은 곱해지지 않는다 → 광선을 쏘지 않음(두 패스가 같은 `rtSurface`의 `s.normal`을 씀). 인라인 경로(REFL_LOCAL_TRACE)도 f = 0이면 광선 없음. 값 동일. [예상] 표본의 대략 절반이 등진 쪽인 벽·바닥 hit에서 r.refl.localshadow −30~50 %. 확인: ReflectionAnalytic 5(국소광 hit), 기차·욕탕 반사 화면 동일, 기차 1440p r.refl.localshadow A/B. (반사 인라인 커널 DXIL 197 → 172 KB: 887b5d8의 hit 셰이딩 1회 효과.)
- **[GI 텍셀 이중선형: 행당 적재 1회]** `giTexelRadiance`(반사 hit·GI hit의 캐시 방사휘도, 원뿔 필터 탭, 유리·물)가 텍셀 4개를 Load2 네 번으로 읽었다. 한 행의 두 텍셀은 붙은 워드(8 B씩)라 행당 Load4 한 번(지도 가장자리에서 클램프로 같은 텍셀이면 Load2 한 번) → 같은 텍셀·가중치·합산 순서(비트 동일), 적재 명령 절반. 조도 지도의 행 Load4(0b3ecfd)와 같은 방식. [예상] r.refl.shade·r.gi.trace의 캐시 조회 몫 −5~10 %. 확인: GiAnalytic·ReflectionAnalytic 통과(값 동일), 기차 1440p r.refl.shade·r.gi.trace A/B. 리눅스 dxc 492 커널, 최대 DXIL 192.8 KB.
- **[GiIntegrate: SH 계수와 조도 지도를 다른 웨이브에서 동시에]** r.gi.integrate(기차 내부·출력 1440p 0.31 ms)는 갱신 항목당 128스레드 그룹에서 SH 9계수(레인 0~8, 표본 128개 직렬 루프)를 먼저, 그다음 조도 지도 81텍셀(레인 0~80)을 계산했다 → 첫 웨이브가 두 루프를 연달아 돌고 마지막 웨이브(레인 96~127)는 놀았다. SH를 레인 96~104로 옮겨 두 루프가 다른 웨이브에서 병렬로 돈다. 계수마다 같은 루프·같은 합산 순서라 비트 동일. [예상] r.gi.integrate −25~40 %(32레인 웨이브: 첫 웨이브의 직렬 SH 몫). 확인: GiAnalytic·hostmotion(결정론) 통과, 기차 1440p r.gi.integrate A/B.
- **[GI 화면 필터: 평면 밖 탭은 G버퍼를 읽지 않음]** r.gi.screen.filter(기차 내부·출력 1440p 0.39 ms)는 탭 12개마다 깊이·G버퍼를 모두 읽어 위치·법선을 만들었다. 가중치 = exp × 평면² × 법선 일치라 평면 가중치가 0인 탭(화소의 접평면에서 먼 탭: 벽과 바닥 경계, 물체 가장자리)은 0을 더할 뿐이다 → 깊이로 위치만 먼저 복원하고 평면이 0이면 G버퍼 적재·디코드 없이 넘김. 쓰이는 탭은 `giScreenInputs`와 같은 식이라 비트 동일. [예상] 경계가 많은 화면에서 필터 −10~25 %. 확인: 기차·욕탕 화면 동일, r.gi.screen.filter A/B.
- **[그림자: 해바라기 탭 좌표 표, 국소광 요청의 파동 내 중복 저장 제거]** (1) 해·국소광 추정기의 차단물 탐색(5탭)·반영 필터(16탭)는 탭마다 sqrt·cos·sin을 계산했다(국소광은 화소 × 빛마다 21회). 설정된 탭 수 5·16은 배정밀도로 계산해 float로 반올림한 표(`kVsmDisk5/16`, VsmCommon.hlsli)에서 읽고, 다른 탭 수만 계산한다. 탭 위치·개수·가중치는 같고 좌표는 GPU sin/cos보다 정확하다(차이 1e-7 수준). s.shadow.penumbra·visibility·overflow, 반사 국소광 그림자, 안개 공통. (2) VsmLocalMark의 요청 저장을 해의 VsmMark처럼 파동의 활성 레인 중 같은 주소는 한 번만 저장(같은 워드·같은 값이라 결과 동일). [예상] 국소광 추정기 ALU −5~10 %, localmark 저장 트래픽 대폭 감소(8×8 파동이 대부분 같은 페이지). 확인: vsmtests·localshadowtests 통과, 욕탕·기차 그림자 화면 동일, s.vsm.localmark·s.shadow.overflow·penumbra A/B(내부·출력 해상도 함께).
- **[반사 명중 셰이딩: 움직임 계산이 표면의 레코드를 다시 읽지 않음, 그림자 광선 큐를 파동당 원자 1회]** r.refl.shade(기차 내부·출력 1440p 2.05 ms)에서 reflHitMotion은 rtSurface가 이미 읽은 인스턴스·메시·서브메시·기하·삼각형 인덱스를 다시 읽었다(컴파일러가 합치지 못함). `rtSurfaceParts`가 읽은 레코드를 넘겨 재사용 → 명중당 버퍼 적재 313 → 304(DXIL 계수). 그림자 광선 큐 적재는 레인마다 원자 연산이었다 → 반영 큐처럼 파동당 원자 1회 + prefix. 값·표본 동일(큐 안의 순서만 바뀜: 결과는 slot으로 되돌아감). [예상] r.refl.shade −2~4 %. 확인: reflectionanalytic·hostmotion 통과, 기차·욕탕 반사 화면 동일.
- **[판단 · 1440p·1080p 총 3~4 ms 목표 (사용자 요청)]** 지금까지(세션 4)의 변경은 모두 결과가 같은 구조 최적화이고, 합쳐 [예상] 기차 내부 1440p·출력 1440p에서 약 0.8~1.5 ms, 욕탕에서 국소광 캐시 포함 약 2~4 ms 줄이는 몫이다(887b5d8부터 미측정). 1db06e4 기준 9.93 / 15.53 ms에서 **3~4 ms는 결과가 같은 변경만으로는 닿지 않는다**:
  - 프레임은 GPU 처리량 한정이다(renderergate는 GPU 타임스탬프 `gpuFrameMs`). 프레임 안 async compute는 이 GPU에서 처리량 이득이 없다는 실측이 설계서 4.3에 있고 일부러 꺼져 있다(큐 간 fence 62~79 µs). 비어 있는 GPU 시간을 겹쳐서 줄이는 길은 없다.
  - 남은 큰 몫(기차 내부 1440p): 반사 계열 합 ≈ 3 ms(shade·accumulate·trace·localshadow·combine·resolve·jobs·classify), GI 화면 조회 + 필터 ≈ 1.2~1.5 ms, M 셰이딩·resolve ≈ 1 ms, 태양 반그림자·가시성 ≈ 1.3 ms, GI 추적·적분 ≈ 0.8 ms, 나머지 작은 패스 100여 개 ≈ 2 ms. 이 대부분은 화소 수 또는 광선·표본 수에 비례하는 일이고, 표본·광선·해상도를 그대로 두면 구조 변경으로 줄일 수 있는 폭은 패스별 수십 % 수준이다.
  - 3~4 ms에 닿으려면 **사용자 결정이 필요한 레버**가 있어야 한다 [예상, 모두 품질 규칙을 바꾸는 것]: ① 1440p·1080p 출력에도 내부 해상도 + 시간 업스케일(4K에 채택한 545bd74 경로를 `output.render_height_max`로 넓힘: 예) 1440p 출력을 내부 960p에서 → 화소 비례 몫 약 0.44배, 기차 1440p ≈ 6~6.5 ms) ② 반사 광선 정책(4K에서 승인되지 않았던 레버 1: 거친 면 G 표본 간격·M 광선 수) ③ r.gi.screen을 프레임 나눠 갱신(레버 3) ④ GI 갱신 광선 예산. ①만으로는 약 6 ms이고, 3~4 ms는 ①+②+③ 수준이 필요하다. 결정해 주면 그 경로로 진행한다.
  - 그 전에 887b5d8 이후 커밋들(국소광 캐시, 반사 셰이딩 1회, 이번 세션 4 변경)을 같은 A/B/B/A로 재 주면 남은 차이를 패스별로 다시 정리한다.

## 세션 5 · 사용자 결정 레버 1~3 (2026-09-28, "보이는 것이 같으면 된다 — 게임플레이 중 다양한 행동에서도")

- **[레버 1 · 1440p·1080p 출력에도 내부 해상도 + 시간 업스케일]** `output.render_scale = 0.6667`, `render_scale_min_height = 1080`, `render_height_max = 1440`(상한). 내부 해상도: 4K 출력 → 2560×1440(기존과 같음), 1440p 출력 → 1707×960, 1080p 출력 → 1280×720(모두 화소 0.44배). 1080 미만 출력(시험 장면 960×540 등)과 캡처(`outputLinearHdr`)·디버그 뷰는 그대로 원래 해상도. 업스케일 경로·지터·히스토리는 4K에서 쓰던 것 그대로(Halton 주기 18). **텍스처 선명도:** M의 텍스처 발자국(SampleGrad 기울기, 법선 기울기 모멘트의 rho)을 출력 화소 기준으로 줄인다(`FrameConstants::upscaleRatio`, 새 필드, 구조체 576 → 592 B; `mFootprintScale`). 시간 업스케일러가 쓰는 mip bias log2(내부/출력) = −0.585와 같다. 이것이 없으면 내부 960p의 거친 mip이 업스케일 뒤에 흐리게 보인다(기존 4K 경로도 이번부터 같은 보정을 받음). 다른 뷰(평면 반사·보조 뷰)와 시험의 `FrameConstants c{}`는 0 = 원래대로. [예상] 기차 내부 960p·출력 1440p ≈ 9.9 → 5~5.5 ms(화소 비례 몫 0.44배 + 업스케일 약 0.25 ms), 내부 720p·출력 1080p ≈ 3.5~4 ms; 욕탕은 그 비율로 약 8 ms / 6 ms. 확인: (1) 기차·욕탕 출력 1440p·1080p 화면을 원래 해상도(`render_scale = 1`)와 나란히: 텍스처 선명도·가장자리·반사·그림자 차이가 보이는지(정지, 걷기, 회전, 달리기) (2) 4K 출력 화면이 전보다 선명해졌는지(흐려지거나 반짝임 없는지) (3) hostmotion·vsmtests·localshadowtests·reflectionanalytic 통과(960×540은 원래 해상도 그대로) (4) renderergate 1440p·4K A/B.
- **[레버 3 · r.gi.screen 프레임 분할]** `gi.screen_update_frames = 4`. 메인 뷰의 8×8 타일이 2×2 Bayer 순서로 4프레임에 한 번씩 캐시를 조회한다(그룹 단위라 재사용하는 웨이브는 조회를 통째로 건너뜀). 나머지 화소는 **자기 표면점**의 지난 프레임 값을 쓴다: vis id → 삼각형의 이전 틱 정점으로 정확한 움직임(GiProbeGather의 히스토리와 같은 `giPreviousSurface`) → 이전 뷰 투영 → 주변 4화소 쌍선형. 4화소 **모두** 같은 표면이어야 한다: 저장 깊이를 역투영한 점이 2 화소폭/시선 코사인 안, 평면에서 1 화소폭+거리/2 안, 법선이 같은 캐시 법선 클래스이고 8° 안, 데이터 플래그 같음, 값 나이 ≤ 2. 점 자체가 지난 프레임 이후 ¼ 화소폭 넘게 움직였으면(움직이는 물체) 새로 조회. 실패하면(가림 해제, 가장자리, 이동 물체, 첫 프레임) 그 화소는 조회. 값은 노출 비율로 다시 맞춘다. 결과: 값은 최대 3프레임 전의 같은 표면점 조회값(조명 변화 지연 ≤ 3프레임, 프로브·캐시 갱신은 분할 안 함). 히스토리 재시작: 새 크기, 장면 revision, 조명 epoch, 불연속(복원·컷), 원점 이동, 조회 통계 게이트 켜짐, vis 버퍼 없는 프레임(시험의 대체 가시성). 메모리: 화소당 값 8 B + 키 8 B, 핑퐁 2벌 = 32 B (내부 960p 52 MB, 내부 1440p 118 MB). [예상] r.gi.screen 약 −60~70 %(정지·걷기; 빠른 회전·이동 물체가 많으면 덜): 기차 내부 960p·출력 1440p 약 0.5 → 0.2 ms. 확인: (1) 기차·욕탕에서 걷기·회전·달리기·문 열기·빛 켜고 끄기 중 간접광에 8×8 타일 무늬나 번쩍임·끌림이 없는지(`gi.screen_update_frames = 1`과 나란히) (2) GiGate(lookup stats는 분할 꺼짐)·GiAnalytic·hostmotion 통과 (3) r.gi.screen A/B.
- **[레버 2 · 반사 광선 정책 — 적용하지 않음(레버 1 이후 이득 없음, 판단 근거)]** 정의된 레버 2는 "M을 거칠기 < 0.05로 좁히고 0.05~0.14는 G로, G 최소 간격을 해상도에 비례(4K 원래 해상도에서 2 px)"였다. 레버 1 이후 내부 높이는 최대 1440(4K 출력)·960(1440p)·720(1080p)이라 비례 최소 간격은 2 × 1440/2160 = 1.33 이하 → 정수 간격 1 px 그대로다. 그러면 0.05~0.14 화소는 M 광선 1개 대신 G 표본(광선 4개/화소)이 되어 **오히려 비싸진다**. 남는 변형(G 표본당 광선 4 → 1~2, 시간 누적으로 보완)은 수렴 상태 잡음 표준편차가 1.4~2배가 되고 움직일 때(히스토리 거부) 잡음이 보인다 → "보이는 것이 같다"에 어긋나 넣지 않았다. 반사 비용은 레버 1로 화소 비례 몫이 0.44배가 된다(기차 반사 합 ≈ 3 ms → ≈ 1.3 ms [예상]).
- **[레버 1 확인 항목 추가 · 그림자 해상도]** 태양·국소광 VSM의 레벨/밉은 **내부** 화소 발자국으로 고른다(바꾸지 않음). 원래 1440p보다 그림자 텍셀이 출력 화소 약 1.5개 크기다(4K 출력에서 이미 쓰던 것과 같은 비율). 반그림자가 넓은 그림자는 차이가 없고, 접촉부의 딱딱한 그림자 가장자리만 약간 부드러울 수 있다. 보이면 태양은 `shadow.vsm.lod_bias = -0.585`(원래 해상도와 같은 레벨)로 맞출 수 있다 — 대신 페이지 수가 늘어 s.vsm.raster 비용이 는다. 국소광은 같은 보정을 코드로 넣을 수 있다(욕탕 국소광 VSM 비용 증가). 확인: 기차 창틀·욕탕 수도꼭지 아래 같은 접촉 그림자를 `render_scale = 1`과 나란히.
- **[세션 5 합계 · 3~4 ms 목표 대비, 모두 예상]** 1db06e4 실측 기준(기차 9.93, 욕탕 15.53 ms, 둘 다 내부·출력 1440p)에서 세션 4 구조 변경 + 레버 1 + 레버 3: **기차 출력 1440p(내부 960p) ≈ 4.5~5 ms, 출력 1080p(내부 720p) ≈ 3~3.5 ms; 욕탕 출력 1440p ≈ 7~8 ms, 출력 1080p ≈ 5~6 ms.** 기차 1080p는 목표 안, 기차 1440p는 근처, 욕탕은 국소광 VSM(페이지 래스터는 화소 수에 비례하지 않음)·froxel이 남는다. 다음 측정(renderergate 출력 1440p·1080p·4K, 기차·욕탕, 패스별)으로 남은 몫을 다시 나눈다.

## 세션 6 · 더 줄이기 (분석 에이전트 4개의 보고에서 결과가 같은 것부터)

- **[반사 정확 다듬기 4가지]** (1) 반사·GI 광선 명중의 캐시 항목 touch·요청: 이번 프레임에 이미 스탬프된 항목은 건너뜀(giKeepRead와 같은 규칙: 스탬프를 찍은 쪽이 touch·요청을 이미 함) → 명중마다 원자 교환 1회·저장 1회 감소, 같은 셀을 맞히는 이웃 광선의 경합 제거(r.refl.shade, r.gi.trace). (2) r.refl.localshadow: 광원 표본은 명중점(origin + dir t)만 필요 → 표면 재구성(rtSurface: 인스턴스·메시·삼각형·정점 적재) 전에 표본을 뽑고, 그림자를 드리우지 않는 표본이면 재구성 없이 끝. (3) 해 VSM 분류가 본그림자(UMBRA)면 얇은 캐스터 투과율 조회를 하지 않음(가시도 0이라 읽히지 않음). (4) r.refl.accumulate: 전부 K인 타일(유효 텍셀 0)은 이번 프레임 모드·값이 없으므로 히스토리 없음으로 바로 끝 — 초기화 안 된 모드·값을 읽어 가짜 히스토리를 쓸 수 있던 잠재 오류도 막음. [예상] r.refl.shade −3~8 %, r.refl.localshadow −40~70 %(그림자 없는 표본 비율에 따라), accumulate 소폭. 확인: reflectionanalytic·hostmotion, 기차·욕탕 반사 화면 동일.
- **[하늘 시야 LUT: 카메라 높이가 조금 바뀔 때마다 다시 만들지 않음]** s.atmosphere.skyview(재구성 1회 0.22~0.27 ms [실측, 기존 로그], 해상도 무관)는 카메라 고도를 float로 정확 비교해 걷기·머리 흔들림·계단·원점에서 멀어짐(곡률 항)마다 **매 프레임** 다시 만들어졌다. 고도 변화가 max(0.25 m, 1e-4 × 고도)를 넘을 때만 다시 만든다(해 방향은 그대로 정확 비교). 대기 척도 높이 1.2~8 km라 0.25 m는 복사휘도 2e-4 미만, 지평선 이동 0.05 px(960 px 기준) — 10-bit 한 단계의 수십분의 1. [예상] 움직이는 프레임마다 −0.2 ms 이상(1080p 3 ms 프레임의 약 7 %). 확인: 계단·점프하며 하늘·지평선 비교, 움직일 때 s.atmosphere.skyview 빈도(stats.skyViewBuilds).
- **[그림자 래스터의 컬링 고정비 제거]** 래스터 서비스 실행(태양 VSM, 국소광 요청마다 뷰 252개)의 인스턴스 컬링이 GPU가 쓰는 인스턴스(메시 파티클)의 **용량**(호스트 예약 65536)으로 그리드를 잡아 요청당 (65536/64) × 252 ≈ 258 K 그룹(1650만 스레드)을 띄웠다 — 거의 전부 할 일 없음. (1) CPU 인스턴스는 직접 디스패치, GPU 인스턴스는 CullReset이 라이브 개수로 쓴 인자(VA_GPU_INSTANCES, 비어 있던 인자 워드 15~17)로 간접 디스패치(CullInstances SOURCE=2). (2) 타일 마스크가 모두 0인 뷰(페이지 캐시로 정지 프레임의 국소광 뷰 대부분)는 인스턴스·청크 단계에서 바로 버림 — 그런 뷰의 클러스터는 어차피 타일 시험에서 모두 떨어지므로(CullClusters) 그려지는 것은 같다. [예상] 국소광 요청당 −0.05~0.2 ms, 태양 실행 −0.02 ms; 욕탕(국소광 요청 여러 개)에서 합 −0.3 ms 이상 가능. 확인: localshadowtests·vsmtests, 욕탕·기차 그림자 동일, s.vsm.localraster*.instances.p1 시간.
- **[국소광 overflow 타일: 그림자를 줄 수 있는 빛이 있을 때만 목록에]** 세 번째를 넘는 그림자 광원이 froxel 목록에 있기만 하면 타일이 overflow 목록에 올라 ShadowOverflow가 전부 평가했다(froxel 목록은 froxel과 빛 구의 보수적 교차라 실제로 닿지 않는 빛이 많다). 이제 그중 하나라도 이 화소를 가릴 수 있을 때(슬롯 있음, 도달 거리 farM 안, 근평면 밖 = vsmLocalVisibility의 조기 반환 조건 그대로, `shadowLocalCanShadow`)만 올린다. 안 올린 타일은 head 0 → M이 1을 쓰는데, 평가했어도 1(저장 255)이었다 → 비트 동일. LocalShadowTests는 "head 0인데 세 번째 넘는 빛이 있음 = 누락"으로 셌으므로, 그 빛들이 모두 도달 거리 밖이면 정상으로 보도록 고쳤다. [예상] 욕탕 s.shadow.overflow −20~40 %(1.05 ms 중), M의 overflow 레코드 적재 감소. 확인: localshadowtests(overflow 항목), 욕탕 화면 동일.
- **[GiIntegrate: 직렬 합을 한 웨이브만, SH 기저를 표본당 한 번]** r.gi.integrate(내부 1440p 0.31 ms, 해상도와 무관한 고정비)는 그룹의 128스레드(웨이브 4개)가 모두 같은 직렬 루프(바운스 64 + 64, 전체 조도 128회)를 돌았고, SH 레인(9개)은 표본 128개마다 SH 기저 9개를 다시 계산해 동적 인덱스(로컬 메모리 배열)로 하나만 썼다 — SH 레인이 그룹의 가장 긴 경로. (1) 직렬 합은 첫 웨이브만 돌려 groupshared로 나눔(같은 루프·같은 순서라 비트 동일, 장벽 1개 추가). (2) 표본마다 기저 × 가중치 9개를 적재 단계에서 한 번 계산해 groupshared에 두고 SH 루프는 곱·합만(같은 곱, DXIL alloca 0). [예상] r.gi.integrate 0.31 → 약 0.15~0.2 ms(모든 출력 해상도 공통). 확인: GiAnalytic(결정론 모드 비트 동일), hostmotion, GI 화면 동일.
- **[M 국소광 루프: 0을 더하는 빛은 그림자 자료를 읽지 않음; 커버리지 셰이딩의 슬롯 찾기 O(N²) → O(N)]** (1) ShadeOpaque는 빛마다 가시도(슬롯 또는 세 번째 넘는 빛의 overflow 레코드 = 의존 적재 2번, FALLBACK 타일에서는 VSM 추정기 전체)를 먼저 읽었다. 이제 그림자 서수만 세고, 창(window)·스폿·조명 면(NoV > 0 && cosL > 0, 또는 Foliage 뒷면; 코트·쉰 로브도 NoL ≤ 0에서 0)으로 기여가 정확히 0인 빛은 가시도·빛 함수·BRDF를 건너뜀. 가시도 읽기는 한 곳(DXIL 한도: 최대 변형 193.8 KB). (2) CoverageShade는 빛마다 froxel 목록을 두 번(z_near·z_far) 처음부터 걸어 슬롯을 찾았다 → 조각마다 한 번씩 처음 세 그림자 광원을 모아 두고 비교(`shadowFirstCasters`, 목록 항목은 유일하므로 같은 서수). [예상] 욕탕 4K 출력 m.lit.shade −0.1~0.2 ms, 1440p 출력 −0.05~0.1 ms; 커버리지 패스(머리카락·가장자리)의 국소광 몫 대부분. 확인: 욕탕·기차 화면 동일, ShadingTests.
- **[GI 캐시 조회의 로컬 배열 제거, 결정론 앵커 표 지우기는 결정론 모드에서만]** (1) `giScreenPick`의 uint4 동적 성분 인덱스가 로컬 배열(alloca)이 되어 r.gi.screen 커널 8개, ShadeOpaque 16개가 있었다(레벨마다 16번 저장 + 모서리마다 인덱스 적재) → select로 바꿔 0개(비트 동일). (2) GiTableClear가 매 프레임 결정론 앵커 표(테이블 슬롯 × 8 B = 4 MB)를 지웠는데 이 표는 `gi.deterministic`에서만 쓰인다 → 그 모드에서만 지움. [예상] r.gi.screen −0~10 %, r.gi.clear −4 MB 쓰기(약 0.01 ms). 확인: GiAnalytic --determinism, GiGate.
- **[VsmLocalMark: 중심 방향 투영을 밉 3개에 한 번, 반 페이지 여유를 광원 위치의 반올림에 맞춤]** 중심 방향은 밉마다 같아서 투영도 같다 → 한 번만(밉당 텍셀 좌표만 다름: 2의 거듭제곱 배). 이웃 요청 생략의 여유 1/1024 페이지는 원점에서 먼 광원(≈250 m 이상, 밉 6)에서 투영 반올림보다 작을 수 있었다 → 여유 = 1/1024 + 2^mip × 4 ulp(|위치|)로 늘려 생략이 항상 정확하게(먼 곳에서 요청이 조금 늘 수 있음). [예상] s.vsm.localmark −10~20 %. 확인: localshadowtests, 원점에서 먼 곳의 국소광 그림자 페이지 경계.
- **[해 반그림자: 16탭 필터를 받는 화소만 따로 촘촘한 파동으로]** s.shadow.penumbra(기차 내부 1440p 1.02 ms)는 목록의 화소마다 차단물 탐색(5탭) → 원반 분류 → 필터(16탭)를 한 스레드에서 했다. 탐색에서 끝나는 화소(탐색 결과 밝음, 원반 밝음·본그림자 = 경로 3·5·6)는 같은 파동의 필터 화소를 기다렸다. STAGE=0이 탐색·분류를 하고 끝난 화소를 바로 쓰며, 필터가 필요한 화소만 (화소, 반지름, 레벨, 도달) 16 B 레코드로 두 번째 목록에 넣는다. STAGE=1(s.shadow.penumbra.filter, 간접)이 필터만 가득 찬 파동으로 돈다. 함수는 vsmSunPenumbra를 둘로 나눈 것(vsmSunPenumbraSearch + vsmSunPenumbraFilter, 기존 호출자는 둘을 이어 부름), 수용체는 같은 shadowReceiver로 다시 만든다 → 값 동일. 추가 메모리: 화소당 16 B 임시(내부 960p 26 MB, 에일리어싱). [예상] 경로 3·5·6 비율이 30~50 %면 s.shadow.penumbra −0.15~0.3 ms(내부 1440p 기준, 960p는 약 절반). 확인: vsmtests(경로 통계 32 + 4p는 STAGE=0이 셈), 기차·욕탕 그림자 동일, s.shadow.penumbra + .filter 합.
- **[froxel 적분: 선분이 빛의 범위에 들어가지 않는 국소광은 평가·보행 없이 건너뜀]** froxel 목록은 froxel과 빛 구의 교차라서 타일 중심 광선의 선분이 빛의 범위 밖을 지나는 빛도 들어 있다. 선분 직선과 빛의 거리 h(airLocalMap, 이미 쓰던 값)가 range 이상이면 가우스 노드 거리는 모두 h/cos ≥ range라 창이 0 → 기여가 정확히 0. 그런 빛은 그림자 없는 빛의 8노드 평가, 그림자 있는 빛의 VSM 보행(항목)에서 빠진다. 남은 항목은 레인을 더 받아(K = 64/항목 수) 조각 합의 순서만 달라진다(부동소수 반올림 수준, 실험 비트 128과 같은 성질). VsmLocalMarkAir의 요청은 그대로. [예상] s.froxel.integrate −10~25 %(욕탕 4K 출력 1.10 ms 중). 확인: FroxelTests, 욕탕 안개·빛줄기 동일.
- **[세션 6 · 결정이 필요한 레버 (분석 에이전트 보고, 모두 [예상])]** 결과가 같은 변경은 위에 넣었다. 아래는 화면이나 반응이 달라질 수 있어 넣지 않았다.
  1. GI 광선 예산을 프레임당 → 초당(예: 60 fps에서 지금 양 = 3000만/초): 300 fps에서 r.gi.trace+integrate −0.4~0.5 ms. 수렴 상태 화질 같음, 불 켜고 끌 때 간접광이 따라오는 시간은 60 fps 때와 같아짐(프레임 수로는 김).
  2. 반사 명중점의 GI 캐시를 모서리 8개 대신 가중치 확률로 1개 읽기: r.refl.shade −25~40 %. 간접광 잡음 증가(시간 누적이 흡수, 움직일 때 보일 수 있음).
  3. 반사 명중점의 국소광 그림자를 광선 대신 VSM으로: r.refl.localshadow 대부분. 반사 속 국소광 그림자가 필터된 VSM 그림자가 됨.
  4. 기차에 붙은 국소광 그림자를 기차 좌표로 캐시(직선 이동 중에만 정확): 기차 이동 중 국소광 페이지 재래스터 대부분 제거.
  5. 국소광·해 반그림자·국소광 공기를 2~4프레임에 나눠 재사용(r.gi.screen과 같은 방식): 그림자 변화가 최대 3프레임 늦음.
  6. 모션 블러 끄기/카메라만, 가장자리 AA 끄기, render_scale 0.5: 각 −0.05~1.5 ms, 모두 보이는 차이.
- **[세션 6 · 남은 정확한 후보(작음, 다음 차례)]** 업스케일에 m.post.final 합치기(블룸 꺼짐일 때, −0.05 ms), 업스케일 움직임 팽창을 내부 텍셀에 저장 + 공유 타일(−0.03~0.06), 움직임 벡터 한 번 계산(−0.03~0.05), 반사 광선 방향·원점 저장(광선 버퍼 1 GB 한도 조정 필요, −0.05~0.1 at 960p), GI 방출체 표본 블록 생략(방출체 없는 장면 −0.04~0.08), 호스트 패스별 타임스탬프 기본 끔(−0.03, 인게임 패스 표가 꺼짐).

## 세션 7 · 로컬 검증 368e103 대응 (2026-09-28)

- **[레버 3(r.gi.screen 프레임 분할) 기본 끔 — 원인]** 4K 출력(내부 1440p) r.gi.screen 0.89 → 0.98 ms [실측]. 재사용 시험이 이웃 화소의 G버퍼 법선(노멀맵이 적용된 셰이딩 법선)을 8° 안·같은 캐시 법선 클래스로 요구하는데, 텍스처 표면에서는 이웃 법선이 대부분 어긋나 재사용 시도 비용 + 조회 비용을 모두 낸다. 또 조회 값은 그 정확한 법선에 의존하므로 이웃 값은 원리적으로 이 화소의 값이 아니다(노멀맵 디테일이 번짐). → `gi.screen_update_frames = 1`, 커널 변형 SPLIT=0(원래 조회 커널, 13.5 KB)과 SPLIT=1(분할, 설정으로만)로 나눔. 확인: r.gi.screen이 887b5d8 수준(내부 1440p 0.89 ms 이하)으로 돌아왔는지.
- **[renderergate --capture-output FILE.pfm]** 업스케일 뒤 출력 해상도의 선형 영상(복사휘도 × 노출, 후처리 인코딩 전 = --capture와 같은 단위)을 PFM으로. 프레임은 플레이와 같이 내부 해상도(output.render_scale)로 그려지고, M이 `ViewResources::upscaled`(업스케일 출력, RGBA16F)를 내보내 게이트가 복사한다. 같은 장면·해상도로 `--capture`(원래 해상도 강제) PFM과 비교하면 "보이는 것이 같다"를 판정할 수 있다. 업스케일하지 않는 프레임(render_scale 1 등)에서는 실패 메시지.
- **[1080p 타이밍]** Harness가 1080p(1920x1080)를 받는다, output.toml resolutions에 1920x1080 추가, renderergate `--resolution 1080p` 및 `all`(4K, 1440p, 1080p).
- **[reflectionanalytic furnace — 원인 미확정]** 887b5d8 이후 G 경로를 지나는 변경을 모두 887b5d8과 대조했다: giTexelRadiance Load4(같은 텍셀·가중치·합산 순서), rtSurfaceParts(같은 레코드), 적중 스탬프 가드(스탬프는 giRequestHit만 쓰고 그 앞에 항상 giTouch → 같은 요청 집합), 그림자 광선 큐(순서만), UMBRA 투과율(읽히지 않음), 누적 K 타일(시험 경로는 vis id가 없어 누적 없음), GiIntegrate(같은 식·같은 합산 순서), giScreenPick(같은 값), 결정론 앵커 표(결정론 모드 전용), 업스케일 발자국(시험의 FrameConstants c{}는 upscaleRatio 0 → 1). 퍼니스에는 해·국소광이 없어 그림자 변경도 무관. 비정확한 변경을 찾지 못했다. 시험은 gi.deterministic = false라 GI 캐시가 실행마다 다르고, 판정은 평균 ±1 %와 이상치(3 % + z σ/√4)다. 필요한 것: (1) 실패 로그의 "G outlier at pixel ..." 줄과 G mean 값 (2) 887b5d8과 HEAD를 각 2번 실행해 변동 폭 (3) `--set gi.deterministic=true`로 두 빌드 비교 — 수치가 같으면 GI 잡음, 다르면 G 경로 커밋(041a8f3, 3380015, 8f74804, e6c729e, 7f9f35b, 86b7e77) 이분 탐색.
- **[r.refl.shade 1.69 → 1.97(4K 출력) — 가장 유력한 원인]** 명중당 코드는 887b5d8보다 가볍다(DXIL: 버퍼 적재 315 → 307, 원자 39 → 39, 텍스처 122 → 119). 늘어난 것은 명중 수일 가능성이 크다: 363f038부터 M의 텍스처·법선 기울기 모멘트가 출력 화소 발자국을 써서(원래 해상도와 같은 모습) 거칠기 넓힘이 줄고 → K(광선 없음)였던 화소 일부가 M/G가 되어 광선이 는다. 확인: reflection.stats_log_frames = 60으로 M·G 작업 수를 887b5d8과 비교. 사실이면 이것은 "원래 해상도와 같은 반사"의 비용이다(되돌리면 반사가 원래 1440p보다 흐려짐).
- **[목표 3~4 ms 대비 남은 몫 (368e103 실측 기준, 부분)]** 기차 출력 1440p(내부 1707×960) 5.40 ms, 욕탕 출력 1440p 6.31 ms. 받은 패스(기차 출력 1440p): r.refl.shade 1.04, r.gi.trace 0.47, r.gi.integrate 0.30, r.gi.screen 0.45 + filter 0.17 = 2.43, 나머지 약 2.97(패스별 표 미수령).
  - 해상도와 무관한 고정비: r.gi.trace + integrate 0.77(프레임당 광선 50만 개), 하늘 LUT(이제 움직일 때만), VSM 래스터(페이지 수 = 장면·광원), 국소광 요청의 컬링 고정비(e48a81f로 감소 예상). 1080p 출력(내부 1280×720 = 960p 화소의 0.56배)에서도 그대로 남는다.
  - 화소 비례 몫: 반사(shade·trace·accumulate·combine·resolve), M(resolve·shade), 해 가시도·반그림자, GI 화면 조회·필터, 업스케일(출력 해상도).
  - [예상] 1080p 출력: 기차 ≈ 1.2(고정) + 4.2 × 0.56 ≈ 3.6 ms, 욕탕 ≈ 4.3 ms → 기차는 목표 안, 욕탕은 근처. 1440p 출력: 기차 5.40에서 레버 3 끔(−0.05)만으로는 목표 밖.
  - 1440p에서 목표에 닿으려면(결정 필요, 큰 순): (a) 반사 명중점 GI 캐시 확률 1모서리 읽기 r.refl.shade −25~40 %(−0.3~0.4) (b) GI 광선 예산 초당 고정(300 fps에서 −0.5) (c) 반사 텍스처·거칠기 발자국을 내부 화소로 되돌림(363f038의 M 부분 유지, 반사 광선 수만 이전 수준: r.refl.shade 약 −0.15 추정, 반사가 원래 1440p보다 약간 흐림 — --capture-output으로 판단) (d) 모션 블러 카메라 전용·가장자리 AA 끄기.
  - 다음 측정에 필요한 것: renderergate `--resolution all`로 세 출력 해상도의 전체 패스 표(Harness JSON의 passMs), 그리고 `--capture-output`/`--capture` PFM 쌍.

## 세션 8 · 로컬 검증 974c6bb 대응 (2026-09-29)

- **[업스케일 재구성: 원인과 수정]** 원인 두 가지(코드 분석 + CPU 모델 `Tools/ImageQuality/UpscaleModel.py`로 확인):
  1. 프레임마다의 재구성 커널이 **내부 화소** 단위 가우스(exp(-2.29 d²), 2/3 배율에서 σ ≈ 0.7 출력 화소)였다. 히스토리는 그 값을 쌓으므로 수렴한 영상도 그만큼 흐리다. → 거리를 **출력 화소**로 재고 K = 60(σ ≈ 0.09 출력 화소)으로 좁힘. 64위치 Halton 주기(전: 18)로 모든 출력 화소 중심 근처에 표본이 온다 → 정지 영상이 원래 해상도의 점 표본으로 수렴.
  2. 히스토리를 매 프레임 현재 내부 3×3 표본의 평균 ± 1σ 상자로 자름 → 한 프레임의 3×3이 놓친 가는 선·몰딩(내부 화소보다 가는 디테일)이 매번 깎임. → **저주파만 보정**: 히스토리를 내부 화소 하나 폭으로 평균낸 값이 상자 밖이면 그 차이만큼 히스토리를 옮긴다(조명 변화·가림 해제는 따라가고 고주파 디테일은 유지). 보정량이 상자 크기만큼 크면(다른 표면) 이전처럼 전체를 자름.
  3. 히스토리 길이를 커널 가중치의 "프레임" 단위로: 정지 64, 움직임(¼ 출력 화소/프레임 이상) 8 — 움직일 때마다 히스토리를 다시 표본화(Catmull-Rom)하므로 긴 히스토리는 움직이는 디테일을 흐린다. 새로 드러난 표면(히스토리 4프레임 미만)은 넓은 커널로 채움(구멍·계단 방지).
  - 텍스처 LOD 바이어스 log2(내부/출력) = −0.585는 363f038부터 적용됨(M의 SampleGrad 기울기 × upscaleRatio).
  - **CPU 모델 결과 [예상, 합성 장면, 출력 240², 내부 160²]:** 정지(120프레임) 0.25~0.5 cycles/px 에너지 비 0.10 → **0.92**, SSIM 0.784 → **0.996**. 1.4 px/프레임 이동: 0.03 → 0.56, SSIM 0.768 → 0.92. 이동 중에는 같은 모델에서 **내부 = 출력(업스케일 없이 시간 누적만)도 0.32**라 → 움직임의 손실은 해상도가 아니라 히스토리 재표본화 때문이고, 2/3 배율로 "움직일 때 원래와 같다"는 이 구조로는 도달하지 못한다 [판단]. Lanczos-3 재표본화는 느린 이동에서 0.91/0.965까지 오르지만 빠르면 차이 없음(36탭 비용).
  - **판정 도구** `Tools/ImageQuality/UpscaleCompare.py NATIVE.pfm UPSCALED.pfm [--crop x,y,w,h]`: 0.25~0.5 cycles/px 에너지 비, SSIM, 경계 대비비, 평균 차; 후보 기준 0.95~1.05·≥0.99(재설계안 1.6). numpy 필요.
  - 확인: renderergate `--capture` 대 `--capture-output`(정지 600프레임, 이동 경로)을 도구로 — 정지는 기준 안(에너지 비 ≥ 0.9, SSIM ≥ 0.99)이어야 하고, 이동은 수치와 눈으로. 움직임에서 조각 위 밝은 네모 점이 남는지.
- **[reflectionanalytic furnace — 원인 여전히 미확정, 새 사실 하나]** 887b5d8 이후 바뀐 파일 47개를 시험 경로(GI 캐시·반사 G·RayScene·Frame 상수·GI 화면 필터 포함)까지 다시 대조했고, 결과를 바꾸는 변경은 없다(세션 7 목록 + GiScreenFilter 평면 우선 탭: 같은 위치 식, 0 가중치 탭만 생략). 새 사실: 재설계안 0.1에 따르면 887b5d8 측정 자료는 **dirty 작업 트리**(diff 636ef77259e87a89)에서 나왔다 — "887b5d8에서 통과"가 깨끗한 커밋의 결과인지 확인이 필요하다. 필요한 것(한 번이면 됨): 깨끗한 887b5d8과 HEAD에서 `unx_test_reflection_reflectionanalytic`을 각 2회, 그리고 `--set gi.deterministic=true`로 각 1회 — 로그의 furnace 한 줄 전체와 "G outlier at pixel" 줄. 결정론 모드 수치가 두 빌드에서 다르면 G 경로 커밋 6개를 이분 탐색한다.
- **[재설계안(RENDERER_REDESIGN_20260929) 평가]**
  - 동의: 표본·광선·탭 수를 줄이지 않는 원칙, 측정 먼저(P0/MB-0~4)·기각 조건 명시, 업스케일은 품질 게이트 통과 전 기본 아님, 중앙값 합산 금지, 기차 좌표는 주소 안정화에만.
  - 문제: (1) 할당표(1440p 원래 해상도 2.80 ms)는 실측 9.5 ms에서 약 3.4배를 구조 변경만으로 요구한다. 반사 3.43 → 0.60, 그림자 필터 1.37 → 0.24, 셰이딩 1.10 → 0.32처럼 5배 안팎의 절감을 "자료 순서로 묶기"에서 기대하는데, 문서 스스로 coherent 벤치율 도달이 할당을 보장하지 않는다고 적는다. 정렬·binning으로 흔히 얻는 것은 1.3~2배다 [판단]. (2) P3(GI stencil)는 우리가 측정한 선례(모서리 일괄 적재: r.gi.screen 0.77 → 1.44 ms 악화, 20379fb)와 같은 방향이라 MB-2의 기각 조건이 먼저 판정돼야 한다. (3) 업스케일 게이트(움직임 포함 에너지 비 0.95~1.05·SSIM ≥ 0.99)는 CPU 모델로 보면 2/3 배율의 움직임에서 구조적으로 통과하지 못한다(시간 누적의 재표본화 손실, 내부=출력이어도 0.32). 문서대로면 업스케일은 기본에서 빠지고 1440p는 원래 해상도(약 9.5 ms)로 돌아간다 — **사용자 결정 필요**.
  - 구현한 것: **P0 계측** — Harness `extraJson`, renderergate 결과 JSON에 `workload` 객체(출력·내부 크기, render_scale, VSM dirty 페이지·래스터 삼각형·요청 평균/최대·캐시 페이지·국소광 수·경로별 화소, overflow 광원 최대, froxel 광원 항목 수, 반사 작업 수(M/G)·주 광선 수 Q_r = M + 4G·평면 뷰, GI 살아 있는 항목·요청·선택 갱신·주 광선 Q_g = 갱신 × 64·반사 hit 조회). 프레임별 패스 구간은 기존 CSV에 있다. 1080p 타이밍(세션 7)과 --capture-output도 P0의 일부다.
  - 구현하지 않은 것과 이유: P1~P6은 각각 MB 측정으로 채택·기각을 판정하도록 문서가 정해 두었고, GPU가 없는 여기서는 그 판정을 할 수 없다. 측정 없이 큰 경로를 갈아엎으면 문서의 A/B·기각 절차를 어긴다. P1(페이지 요청 소유)의 정확한 부분(파동 중복 제거, 중심 투영 1회, 여유 보정)은 이미 들어가 있다.
  - 다음 순서 제안: (1) 로컬에서 renderergate `--resolution all`, 원래 해상도(`--set output.render_scale=1`)와 업스케일 둘 다, 기차·욕탕 → workload JSON = MB-0 (2) 그 수치로 1.1~1.5 비용식 대입, 가장 큰 격차(반사 Q_r·그림자 J) 순으로 P4 → P2 착수 (3) 업스케일 기본 여부 결정.

## 세션 9 · 2~3 ms로: 사용자 결정 2026-09-29 "업스케일 그대로, 제안대로 구현"

기준 [실측, main의 Results/Local/Timing20260929, 974c6bb, 평균 ms]: 기차 출력 1440p(내부 960p) 5.49 = 반사 2.02 · GI 1.58 · 그림자/VSM/froxel 0.86 · M 0.76 · 가시성 0.16 · AS 0.11. 욕탕 6.43 = 그림자/VSM/froxel 2.62 · GI 1.69 · M 1.10 · 반사 0.84. GI 중 trace 0.50/0.68 + integrate 0.31은 해상도와 무관(프레임당 50만 광선). 분석 에이전트 3개(GI 고정비, 욕탕 국소광 그림자, M·GI 화면)의 보고에서 결과가 같은 것만 구현한다.

- **[비동기 컴퓨트: GI 블록 ∥ S의 froxel·화면 그림자]** 프레임이 그래픽 큐 하나에서 직렬(Harness `async_compute: false`, 호스트도 끔)이었다. RenderGraph에 `setAsyncPasses(names)`(이름 또는 `prefix*`; Compute로 선언된 패스만): 지정 패스는 컴퓨트 큐, 나머지는 그래픽 큐. 데이터 순서는 선언된 사용이 정하고 큐 사이는 기존 펜스 로직(RAW/WAW/WAR, 테스트된 경로) → **영상은 한 큐 프레임과 같다**. 컴퓨트 큐 목록의 동기 범위 ALL_SHADING/NON_PIXEL_SHADING은 COMPUTE_SHADING | RAYTRACING으로 명시. `output.async_compute_passes` = GI의 주 뷰 패스 전부(begin~select, trace, integrate, gather, maps, screen, screen.filter; 평면 뷰의 `.planar`는 제외). FrameRenderer: `shadowVisibility(main)`을 GI 바로 뒤, 반사 앞으로 옮김(입력은 resolve·VSM 페이지뿐, 소비자는 M 셰이딩뿐) → 그래픽 큐가 froxel(0.15/0.50) + 화면 그림자(0.48/1.41)를 도는 동안 GI(~1.6 ms)가 컴퓨트 큐에서 돈다. 큐 사이 펜스 2개(각 62~80 µs, RenderGraph.h 실측). [예상] 기차 −0.2~0.4, 욕탕 −0.5~1.0 ms. **확인:** renderergate 두 장면 1080p·1440p A/B(`--set output.async_compute_passes=[]`가 이전 프레임), 화면 동일(--capture 해시 비교 또는 UpscaleCompare로 SSIM 1.0), GBV·디버그 레이어 경고 없음(특히 컴퓨트 큐 장벽), 그래프 fail 없음, JSON `graph.cross_queue_syncs`·`queues`의 compute 항목. Unity 호스트(외부 그래픽 큐)에서도 한 번 실행.
- **[GiIntegrate: 루프를 파동에 나눔 + 0인 발광 표본 건너뜀]** (1) 발광체 표본 64개가 모두 0이면(발광 삼각형 없음·모두 가림) 세 누적 루프가 텍셀 표본 64개에서 멈춘다: 건너뛰는 항은 모두 +0이고 +0에서 시작한 합은 −0이 되지 않으므로 비트 동일. (2) 직렬 합(바운스 128회, E·E² n회)을 첫 파동에서 모두 한 뒤 맵·SH 루프를 하던 것을, 바운스 합은 SH 파동(레인 96), E 합은 맵 마지막 방향 파동(레인 64)에 나누고 맵·SH 합은 가중치 전에 계산(가중치가 필요한 것은 lerp·저장뿐) → 장벽 3 → 2, 임계 경로 약 1/3~1/2 감소. (3) 엔트리의 이전 값(텍셀, 발광 텍셀, 맵, 창, 극) 적재를 장벽 앞으로. 모든 합의 순서·식은 같다. [예상] r.gi.integrate 0.31 → 0.15~0.2. **확인:** GiAnalytic·gianalytic furnace·GiGate(값 동일), hostmotion 정지 셔터 비트 동일(gi.deterministic), r.gi.integrate 시간.
- **[국소광 탭: 아틀라스 크기 조회를 스레드당 1회]** vsmLocalTapOcclusion의 gather마다 GetDimensions(DXIL에서 textureGather 앞마다 getDimensions)가 있었다 → 스레드 정적 변수에 한 번(모든 호출자가 같은 VSM 아틀라스). 같은 정수·같은 나눗셈. [예상] 욕탕 s.shadow.visibility·overflow·M 국소광 −3~8 %. **확인:** LocalShadowTests·VsmTests·ShadingTests 동일, 욕탕 s.shadow.* 시간.
- **[반사 traversal·국소광 그림자 광선이 GI를 기다리지 않음]** r.refl.trace(0.18)·r.refl.localshadow(0.18)는 GI 캐시·화면 프로브를 쓰지 않는데(ReflectionTraceGen의 giHeader는 쓰이지 않는 읽기) 공유 선언(declareShared)에 캐시 UAV가 있어 GI 블록 뒤로 묶였다 → 두 패스는 캐시·프로브를 선언·바인딩하지 않음(UNX_NONE), localshadow를 inline 패스(GI 캐시를 읽음, 광선 버퍼는 안 씀) 앞으로 선언. 그래픽 큐의 GI 창 안 작업: 기차 froxel 0.17 + 화면 그림자 0.48 + 반사 classify·jobs·trace·localshadow 0.47 ≈ 1.1 ms 대 GI 1.5 ms. 값은 같다(같은 입력, 같은 순서의 광선 버퍼 쓰기). **확인:** reflectionanalytic·hostmotion, 반사 화면 동일, 비동기 A/B에서 r.refl.trace·localshadow가 GI와 겹치는지(패스 시작 시각, Harness CSV).
- **[GI 화면 조회: 셀의 갱신 수와 앵커 법선을 함께 적재]** giScreenCellUpdates가 갱신 수를 읽은 뒤 앵커 법선을 조건부로 읽었다(모서리마다 왕복 2회) → 둘 다 엔트리만 필요하므로 같이 발행(갱신 0이면 버림). 같은 값. [예상] r.gi.screen −5~10 %. **확인:** GiGate --lookup-stats 동일, r.gi.screen 시간.
- **[GI 화면 필터: 탭 값과 깊이를 한 번에]** 탭마다 값 적재 → (데이터 있으면) 깊이 적재였던 것을 같이 발행. 같은 값. **확인:** r.gi.screen.filter 시간.
- **[froxel 광원 목록: indexBase를 루프 밖에서 한 번]** froxelEntry가 광원마다 FroxelGrid::indexBase(버퍼 36 B)를 다시 읽어 광원당 의존 적재 사슬(base → 항목 → 광원 레코드)의 한 고리였다 → `froxelIndexBase` + `froxelLightAt`(ShadeOpaque, CoverageShade, ShadowVisibility, ShadowOverflow, VsmLocalMark, ShadowFragments). 같은 항목. [예상] 욕탕 m.lit.shade·s.shadow.*·localmark 각 −2~5 %. **확인:** ShadingTests·LocalShadowTests 동일, DXIL 최대 191.2 KB(FxLayerSetup, 변경 없음), ShadeOpaque LAYERED2 190.8 KB.
- **[국소광 페이지 요청: 반 페이지 이웃이 다음 페이지로 확실히 넘어가면 투영 없이]** vsmHalfStepStays의 반대 경우: 축 방향 반 페이지가 여유(vsmMarkMargin) 이상 다음 페이지로 넘어가고, 그 페이지가 면 안(면 가장자리에서 반 페이지 이상)이며, 다른 축이 여유 안쪽이면 재투영 결과는 (page ± 1)이다(같은 반올림 한계) → 바로 요청. 여유 안이나 면 가장자리는 이전처럼 투영. 같은 요청 집합. [예상] 욕탕 s.vsm.localmark −15~30 %. **확인:** LocalShadowTests·VsmTests(요청 집합 비교가 있으면 그것), 욕탕 국소광 그림자 화면 동일(페이지 누락이면 거친 mip 대체로 흐려짐), s.vsm.localmark 시간.
- **[GI 화면 조회·필터가 반사 hit와 동시에 (임계 경로에서 빠짐)]** 프레임의 임계 경로는 resolve → GI → 반사 → M이다. 주 뷰의 r.gi.screen·filter(내부 960p 0.61 ms)는 M만 쓰는데, 캐시를 읽고(SRV) 반사 hit가 캐시에 쓰므로(UAV: hit 스탬프·나이·요청 목록·통계·새 엔트리) WAR로 반사가 그 둘을 기다렸다. 반사 hit가 쓰는 것은 조회가 읽지 않거나 없는 것으로 읽는 것뿐이다: 새 엔트리는 갱신 0(giScreenCellUpdates가 없는 칸으로 취급 — 생기기 전과 같음), 삽입은 빈 슬롯만 채우므로 다른 키의 탐사 사슬을 끊지 않는다. → RenderGraph `PassBuilder::useConcurrentRead(buffer)`: 앞의 쓰기는 기다리고(같은 큐 장벽, 큐 사이 펜스), 뒤의 쓰기는 이 읽기를 기다리지 않음. 주 뷰의 screen·filter만 사용(평면 뷰는 그대로). 새 엔트리의 기록(갱신 0)이 인덱스 공개보다 먼저 보이도록 giFindOrCreate에 DeviceMemoryBarrier를 항상(이전엔 결정론 모드만; 같은 디스패치 안의 동시 독자에도 필요했던 순서). 값은 같다. [예상] 기차 임계 경로 −0.4~0.6 ms(GI 창에서 그래픽 큐가 froxel·그림자·반사 앞부분 1.1 ms를 도는 동안 GI 본체 0.8 ms). A/B 선택지: `output.async_compute_passes`에 `"s.shadow.*"`를 더하면 주 뷰 화면 그림자도 컴퓨트 큐(GI 뒤)로 — 기차에는 이득 [예상 −0.3], 욕탕은 손해 [예상](그림자 1.4 ms가 컴퓨트 큐 꼬리에 붙음). **확인:** GiGate --lookup-stats·GiAnalytic 동일, 반사·간접광 화면 동일(특히 새 칸이 많이 생기는 빠른 이동: 반사 hit가 만든 칸이 조회에 이전 값으로 섞이면 얼룩), 디버그 레이어 경고(동시 접근)·GBV, 기차·욕탕 시간.
- **[froxel 적분도 컴퓨트 큐로]** `output.async_compute_passes`에 주 뷰의 s.froxel.tiledepth·integrate 추가(GI 앞, 같은 컴퓨트 큐). 욕탕은 그래픽 큐의 froxel 0.50 + 화면 그림자 1.41이 임계였다 → 컴퓨트: froxel 0.5 → GI 본체 → 화면 조회(≈2.2), 그래픽: 그림자 1.41 → 반사 앞부분 → (GI 완료) 반사 나머지(≈2.2)로 균형. [예상] 욕탕 −0.4~0.5, 기차 −0.1 ms. s.froxel.stats(그래픽 복사)는 광원 목록 머리만 읽어 적분을 기다리지 않고, CPU 통계 펜스(그래픽 큐 마지막 펜스)는 M이 컴퓨트 큐의 뒤 위치를 기다리므로 여전히 유효. **확인:** 대기·god ray·국소광 공기 화면 동일, FroxelTests, 욕탕·기차 시간, froxel 통계 로그가 계속 나오는지.
- **[프레임의 광선 추적 구조(BLAS refit·TLAS 빌드)도 컴퓨트 큐로]** r.as.deform·refit·tlas.static·sync·dynamic(기차 0.11, 욕탕 0.03 ms)은 프레임 맨 앞 임계 경로에 있었는데 처음 읽는 것은 GI 광선이다 → 컴퓨트 큐에서 V의 컬링·래스터, S의 페이지와 겹침. 그래픽으로 선언된 r.as.exact.patch·runtime.records 등은 그대로(컴퓨트가 펜스로 기다림). **확인:** 반사·GI 화면 동일, 스키닝·움직이는 물체가 있는 장면(hostmotion)에서 반사 속 물체 위치, 시간.

## 세션 10 · 로컬 검증 84e789f 대응 (2026-09-29)

- **[비동기 컴퓨트 기본 끔]** 실측 네 경우 모두 직렬보다 0.05~0.2 ms 느림(기차 1440p 5.41/5.44 대 5.29/5.36, 욕탕 1440p 6.29/6.30 대 6.10/6.09, 1080p도 같은 방향; `+ s.shadow.*`도 기차 1080p에서 직렬과 비슷할 뿐). 원인 판단: 큐 사이 펜스(각 62~80 µs)와 지연 한정 커널들이 SM을 나눠 써서 겹침 이득보다 큼. `output.async_compute_passes = []`. 기구(RenderGraph::setAsyncPasses, useConcurrentRead)와 결과가 같은 변경(선언 순서, 반사 trace가 GI 자원을 안 묶음)은 유지 — 직렬 실측이 974c6bb보다 기차 −0.2, 욕탕 −0.33 ms.
- **[업스케일 격자·빗살 무늬, 몰딩 계단 — 원인과 수정]** 84e789f crop을 FFT로 보면 업스케일에만 x 방향 0.47~0.5 cycles/px(2화소 주기 세로줄)와 y 3화소 주기 성분이 있고, 원래 해상도는 그 자리가 매끄럽다. CPU 모델을 실제에 가깝게 고침(고대비 나뭇결 ~0.65 cycles/px, 몰딩 선; `Tools/ImageQuality/UpscaleModel.py`): 재구성 커널·지터·부호·화면 고정 성분으로는 재현되지 않고(SSIM 0.999), **히스토리의 저주파 보정**(4f2c4eb: 히스토리 1내부화소 평균을 이번 프레임 3×3 표본의 평균±1σ 상자로 매 프레임 당김)을 넣으면 재현된다(정지 SSIM 0.958, 고주파 에너지 1.16 = 없는 무늬가 생김). 이번 프레임의 3×3은 내부 화소보다 가는 디테일을 담지 못하고, 어느 표본이 화소 주위에 오는지는 지터와 3:2 격자 위상에 따라 달라 보정량이 화소마다 규칙적으로 달랐다. 수정(Upscale.hlsl): 정지 — 히스토리 저주파가 표본의 min/max를 폭의 절반 넘어 벗어날 때만 보정(조명 변화·가림 해제), 폭만큼 벗어나면 전체 클립. 모델: 정지 SSIM 0.9987, 에너지 0.95; 조명 +30 % 뒤 30프레임 오차는 이전과 같음(0.037 대 0.040). 움직임 — 히스토리를 Lanczos-3(6×6)로 재표본화하고 min/max를 폭의 1/10 넓힌 상자로 클립, 이동 히스토리 8 → 4프레임(`output.upscale_history_frames_moving`): 느린 이동 0.2~0.45 px/프레임에서 SSIM 0.73~0.77 → 0.86~0.87, 에너지 0.19~0.23 → 0.37~0.38(조각 번짐 감소; 원래 해상도와 같아지지는 않음 — 2/3 배율 움직임의 구조적 한계). 비용 [예상]: 움직이는 화소에서 히스토리 적재 5 → 36회, m.upscale 1440p 출력 +0.05~0.1 ms. **확인:** crop_train_1440(정지)에서 격자·빗살·몰딩 계단이 사라졌는지, `UpscaleCompare.py`의 정지 SSIM·에너지, 움직임 crop의 조각 번짐, 조명이 바뀌는 장면(욕탕 등불 깜박임 등)에서 잔상 길이, m.upscale 시간.
- **[실행마다 영상이 다른 원인 하나 — 프레임 번호]** Harness의 워밍업이 시간(1.5 s)으로 끝나서 측정 뒤 마지막 프레임(캡처되는 프레임)의 번호가 실행마다, 그리고 속도가 다른 구성(비동기 대 직렬)마다 달랐다 → GI 캐시 수렴 정도, 지터 위상(64주기), 업스케일·반사 히스토리, 파티클 틱이 모두 다른 프레임을 비교하고 있었다(기본 모드 4.4 %, gi.deterministic 1.3 %의 상당 부분일 수 있음). 수정: `HarnessOptions::warmupFrames`, renderergate `--warmup-frames N`(캡처·`--luminance-log`가 있으면 기본 300), 결과 JSON `warmup_frames`, 로그에 캡처 프레임 번호. **추가 계측** `--luminance-log FILE.csv`: 매 프레임(워밍업 포함) 게이트 출력의 평균 휘도(정수 합, 원자 순서 무관) — 한 실행 안에서 밝기가 천천히 흔들리는지/수렴하는지 직접 본다. **확인:** 같은 설정 두 번을 `--warmup-frames 300`으로 캡처해 차이가 얼마나 줄었는지, `--luminance-log`를 기본·gi.deterministic으로 한 번씩(600 + 300프레임) — 곡선이 계속 오르면 GI 캐시 수렴 중(아래 결정론 항목과 별개), 평탄한데 실행 간 차이가 남으면 확률 경로의 차이.
- **[국소광 추정기: 최근접 차폐자 페이지 5개를 함께 적재]** vsmLocalVisibility가 수신점 페이지와 이웃 4개의 최근접 차폐자 키를 한 번에 하나씩(투영 → 테이블 → 블록, 10번의 연속 왕복) 읽었다 → `vsmLocalPageNearest5`: 투영 5개와 테이블 적재 5개를 먼저, 상주한 것은 블록 적재를 함께, 상주하지 않은 것만 거친 mip으로 내려감. 최댓값은 순서와 무관하므로 같은 값. 많은 화소가 이 단계 뒤 조기 종료하므로 그 화소들의 사슬 대부분. 쓰는 곳: s.shadow.visibility·overflow·fragments, M의 대체 경로, 볼륨·FX. DXIL 최대 193.1 KB(FxLayerSetup). [예상] 욕탕 s.shadow.visibility·overflow −5~15 %. **확인:** LocalShadowTests·ShadingTests 동일, 욕탕 두 패스 시간.
- **[정지 장면에서 실행마다 밝기 4 % 차이 — 원인 분석(모델), 확인 대기]** (1) 위의 프레임 번호 차이. (2) GI 캐시의 다중 반사 수렴 [예상, `Tools/ImageQuality/GiConvergenceModel.py`: 셀 C개가 각각 12프레임마다 64광선 갱신, 갱신 잡음 19 %, GiIntegrate의 이력 규칙 그대로]: 반사율 0.8·셀 3000개면 실행 간 0.5 %·실행 안 흔들림 0.2 %로 수렴하지만, **밝고 닫힌 방(반사 몫 0.9)에서 방 전체의 다중 반사를 소수의 거친 셀(C ~ 100)이 나르면 실행 간 4.5 %, 한 실행 안에서도 600~1000프레임 사이 10.9 % 흔들림.** 원인은 Jacobi 단계: 반사 몫 0.9면 Jacobi 길이가 ~53회 갱신(12프레임마다 → ~640프레임)이고 그동안 매 갱신이 값을 통째로 바꿔 갱신 잡음이 반사 되먹임(1/(1−ρ) = 10배)으로 전역 밝기를 천천히 흔든다. Jacobi를 8로 줄이면 흔들림은 줄지만 평균이 15 % 낮게 멈춘다(약한 수축의 누적 평균은 n^−(1−ρ)로만 수렴: gi.toml·GiInternal의 기존 측정과 같은 이유). **확인 방법:** `--luminance-log`로 욕탕 1080p 900프레임 곡선 — 수백 프레임 주기로 수 % 흔들리면 이것. **해결 후보(측정 뒤 결정):** 갱신을 비반사 몫(해·하늘·국소광: 첫 갱신부터 무편향 → 긴 평균)과 반사 몫(이웃의 현재 값: 짧은 창)으로 나눠 저장 — 편향은 기하적으로 수렴하고 잡음은 긴 평균으로 줄어듦. 캐시 저장량 증가(엔트리당 SH/맵 한 벌), 표본 수는 같음.
- **[결정론: 반사 hit의 국소광 표본·해 그림자 광선 시드]** 국소광 표본 시드가 작업 인덱스(원자 추가 순서), 해 그림자 광선 시드가 광선 슬롯(스레드별 원자 할당 순서)에서 나와 실행마다 달랐고, 용량을 넘어 inline 경로로 간 작업은 또 다른 시드를 썼다(어느 작업이 넘치는지도 순서에 따름). → `reflPixelSeed`(화소·프레임) 기반 `reflLocalSeed(job, ray)`·`reflSunSeed(jobSeed, ray)`를 세 경로(split shade·shadow, inline)가 같이 씀: 같은 작업은 어느 경로든 같은 표본. 분포는 같고 기본 모드 영상은 표본만 바뀜(통계적으로 같음). **확인:** reflectionanalytic(분포 시험)·planar 시험.
- **[결정론 감사 결과와 수정]** (분석 에이전트 감사, 파일·줄 근거) 기본 모드에서도 결과를 바꾸던 버그와 확률 경로:
  1. **GI 백그라운드 갱신 경쟁(버그, 기본 모드)** — GiTrace와 GiIntegrate가 인덱스 범위의 백그라운드 슬롯을 각자 판정(giUpdateSlot): trace 때 비어 있던(표본을 안 쓴) 인덱스를 trace 중 새 엔트리가 가져가면 integrate가 그 슬롯의 이전 용도의 표본(일시 버퍼, NaN 가능)을 새 엔트리에 섞었다(프레임당 ~10엔트리 추정, 반사 되먹임으로 퍼짐 → 4.4 %의 한 원인 후보). → `GiBackgroundList.hlsl`(r.gi.background·.done): trace 전에 백그라운드 엔트리를 선택 목록에 넣고 범위를 비움(커서 전진은 여기서). 결과 JSON의 gi_background는 이제 0이고 gi_selected_updates가 이를 포함(광선 수는 같음).
  2. **평면 거울 카메라/광선 선택이 측정 GPU 시간에 의존** → `debug.deterministic`(새 키, debug.toml)에서 사전값만 사용(`reflection.planar_ray_ns` 새 키 1.0 ns [예상], planar_view_*).
  3. 반사 표본 시드(앞 항목, db3e218). 4. **굴절·유리·물 광선 작업 시드가 작업 인덱스(원자 추가 순서)** → 작업 자신의 원점·방향 해시와 프레임. 5. **GI 퇴거 기준의 경쟁** — GiEvict 스레드가 같은 디스패치가 올리는 free count를 읽어 30/240프레임 기준이 도중에 바뀔 수 있었음 → GiBegin이 한 번 판정(flags bit 2).
  - `debug.deterministic = true`는 gi.deterministic을 포함. 남은 것(감사 목록, 아직 안 함): 결정론 모드에서 초기 프레임의 반사 광선 용량 넘침(값은 이제 경로와 무관하게 같아야 함 — inline과 split의 해 가시도 추정기가 비트까지 같은지는 미확인), GI 풀/테이블 고갈 시 도착 순서(통계 STAT_ALLOC_FAIL·TABLE_FULL이 0이면 무관), VSM·froxel 통계 읽기 프레임(아래 커밋), 가시성 버퍼 동일 깊이 타이(동일 평면 겹침에서만), 투명 합성 동일 깊이 타이.
  - **확인:** `--set debug.deterministic=true --warmup-frames 300`으로 같은 설정 두 번 → 캡처가 비트 동일한지(UpscaleCompare로 차이 0 또는 cmp). 다르면 어느 영역인지 알려 주면 좁힌다. 기본 모드 두 번의 차이(4.4 %)가 얼마나 줄었는지도.
- **[결정론 2]** (6) VSM·froxel 통계 읽기가 "GPU가 막 끝낸 가장 새 슬롯"이라 실행마다 다른 프레임을 읽었다(아틀라스 증가·그림자 overflow 용량이 이 값으로 정해짐) → framesInFlight 이상 지난 슬롯만(호스트가 기다린 프레임 = 항상 frame − framesInFlight). (7) gi.deterministic에서 텍셀 R2 회전 시드가 프레임을 포함한 우선순위라 매 프레임 회전이 새로 뽑혀 저불일치 걷기가 독립 지터가 됨(결정론 모드가 기본보다 잡음 많음) → 프레임 없는 키 해시 `giDetKey`(광선 시드·디더에는 프레임이 따로 들어감). 남은 것: 투명 합성의 같은 깊이 타이(풀 원소 순), 가시성 버퍼의 같은 깊이 타이(그리기 순) — 동일 평면 겹침에서만.

## 세션 11 · 결정론 마무리·이력 분리 실험·동일 계산 최적화 (2026-09-29)

기준은 `cloud/render-fixes`의 `25d639c`다. 아래는 **구현과 컴파일 검증** 기록이다. 이 세션에서는 GPU를 실행하지 않았다. 따라서 영상 비트 일치, 품질 향상, GPU 시간, 레지스터 수·점유율·spill, TDR 여부는 모두 로컬 확인 대기다. 특히 "같은 계산"은 설계 의도이며 **비트 동일 통과를 뜻하지 않는다**. Unreal 코드를 복사하지 않았고, 표본·광선·탭 수나 출력/내부 해상도를 줄이지 않았다.

### 11.1 항목별 커밋과 판정 기준

시간은 RTX 4080의 1440p 출력 기준 **[예상]**, 측정값이 아니다. 각각 겹치는 비용이 있어 행별 이득을 그대로 더하면 안 된다. 결정론용 추가 작업은 성능 이득에 넣지 않는다.

| 항목·커밋 | 실제 변경 | 이득·비용 [예상] | 로컬에서 확인할 것 |
|---|---|---|---|
| 0 · `2441753` | 미완성 known-entry 전역 재사용만 제거. `25d639c`의 완성된 국소광 선택 전달·GI 레벨 단일 루프는 유지 | 이득 0; 정상 조회 복원으로 일부 조회 비용 증가 가능 | 미완성 전역 상태 없음, 반사/GI 회귀 |
| 1a · `0b9d7e6` | inline 용량 초과와 split이 같은 해 분류·penumbra 필터, bary 저장 경계, 반사/해 항 FP16 저장·합성 함수를 사용 | 속도 목적 아님; 초기 overflow 프레임은 더 비쌀 수 있음 | 결정론 모드 초기 프레임, M/G, sky, nonresident, grazing. `reflection.experiment_disable=0/64` 결과 **비트 일치** |
| 1b · `8197079` | 결정론 GI에서 missing-key 요청을 모아 안정 정렬·중복 제거 후 키 순/빈 슬롯 순으로 등록. 테이블은 두 정렬 구간 조회. 프로브는 수집→등록→재생, 광선 요청은 다음 프레임 등록 | 기본 모드 추가 비용 0. 결정론 모드는 여러 정렬 dispatch와 큰 임시 메모리 추가; 2~3 ms 예산에 포함할 최적화가 아님 | `unx_test_gi_admission --validate`: 고갈·동일 버킷 충돌·중복·65536 scan 경계·입력 순열. 작은 풀 반복 캡처, origin rebase/resize, GBV, request-capacity 오류 0 |
| 1c · `a30bc03` | 투명 동심도 정렬에 실제 primitive identity 사용. 결정론 가시성은 최종 깊이를 바꾸지 않고 stable winner 선택 후 ID 재기록 | 기본 동심도 비교 0~0.03 ms 비용; 결정론 replay +0.05~0.4 ms, 8 B/pixel 승자 버퍼 | VisibilityTests의 `depth_ties_choose_stable_primitive`; 겹친 유리·heavy coverage·planar mask·움직임. 이미지뿐 아니라 decoded primitive identity 반복 일치 |
| 2 · `1a1d4ca` | GI 비반사 RGB와 cache-bounce RGB를 따로 적분·저장. `gi.split_bounce_history=false` 기본 유지, `bounce_history_updates=4` | OFF 0 ms. ON +0.15~0.45 ms; 엔트리당 3712 B, 20만 엔트리 약 708 MiB; 광선당 임시 48 B 추가 | OFF/부모 비트 일치. 욕탕 1080p 3000프레임 luminance OFF/ON, 평균 에너지·분산·저주파 drift·이동/조명 응답. 개선 입증 후 켤지 결정 |
| 3a · `963b92d` | 8개 GI corner ID를 먼저 조회. 지도 합산은 원래 순서. `reflection.batch_gi_corners=false`; OFF/ON 별도 컴파일 변형 | ON −0.03~0.15 ms 또는 점유율 악화; OFF에는 추가 ID 벡터가 컴파일되지 않음 | OFF/ON 비트 일치, registers/occupancy/spill, shade+inline 합. 이전 일괄 조회 회귀 때문에 측정 전 기본 ON 금지 |
| 3b · `3757c48` | 해 가시도 후보 레벨 최대 4개의 중심 페이지를 먼저 적재, 같은 finest-first 3×3 거주 판정 | −0.01~0.06 ms; 선조회 추가 트래픽으로 손해 가능 | 경계·비상주 hit 비트 일치. `use_stats=0` 시간; use_stats는 선조회한 중심도 읽힘으로 표시 |
| 3c · `aad3991` | 데칼 정렬용 호출자 scratch 배열을 `inout`으로 전달, 원래 정렬·혼합 유지 | 데칼 많은 경우 −0.00~0.03 ms | 겹친 priority/order 데칼·최대 개수·이동 hit 비트 일치, scratch/spill |
| 3d · `9a2cbc9` | 광선마다 VNDF 첫 시도 직전 시드 저장. shade/local-shadow가 앞 광선의 rejection sequence를 재생하지 않음; combine도 같은 저장 시드 사용 | −0.02~0.10 ms, ray slot 56→60 B | 모두 기각된 광선·grazing·초기 용량 경계·4-ray G, RNG/캡처 비트 일치 |
| 3e · `cbc5b86` | jobs의 9개 이웃 타일 유효값을 그룹당 한 번 적재. resolve 결과/표면, accumulate 이력/키의 독립 적재를 앞당김 | 세 패스 합 −0.01~0.07 ms; disocclusion은 추가 읽기 비용 가능 | 부분 타일·SELF 전환·변형/컷·이력 리셋 비트 일치, 패스별 시간 |
| 4a · `61bfcbe` | S/M 광원 순회에서 packed word 4개(광원 8개)씩 적재. 끝부분은 해당 목록 안의 워드만 읽음 | S+M −0.02~0.10 ms | 목록 0/1/2/7/8/9/31/32, 홀수 lights_max, 마지막 froxel, 투명·fallback 비트 일치 |
| 4b · `bf98e5a` | M의 overflow 헤더를 BRDF/LTC 계산 전에 적재; 필요한 가시도 word를 caster 4개 사이 재사용 | −0.01~0.05 ms | caster ordinal이 건너뛰어지는 조명, 강제 overflow capacity, planar/M 비트 일치 |
| 4c · `a41b16b` | 국소광 overflow blocker 탐색/필터 분리. FP32 handoff, 항목당 고유 결과 byte. 큐 초과는 **같은 필터를 즉시 실행** | 욕탕 S −0.03~0.20 ms 또는 dispatch 비용으로 회귀. 일반 pixel bound에서 큐 약 6 B/pixel | `shadow.vsm.overflow_filter_queue=false/true`; LocalShadowTests의 같은 프레임 logical byte 비교. 큐 포화·overflow fallback·움직이는 광원·GBV |
| 4d · `caa7eb6` | 필요한 air slice를 전체 froxel 수 용량의 큐에 모아 적분, 원래 index에 FP32 기록. tile별 국소광 합산·미디어 혼합·prefix scan은 기존 순서. `atmosphere.froxels.integration_queue`로 A/B | 기차 −0.00~0.05, 욕탕 −0.03~0.18 ms; 큐 준비·scratch 트래픽으로 회귀 가능. froxel당 68 B 추가(1920×1080, tile24, 64slice 약 14.9 MiB) | FroxelTests `--queue-ab`: 같은 프레임 volume의 실제 texel byte 비교(행 padding 제외). sky/roof/국소광/미디어/planar, 1~64 slices, GBV·각 큐 패스 합 시간 |

후속 상한·검증 보완도 별도 커밋이다.

| 커밋 | 이유·변경 | [예상]·로컬 확인 |
|---|---|---|
| `aa8c622` (3d 보완) | ray owner의 기존 4-bit index와 맞게 `g_rays_per_sample`을 변환 전에 1~16으로 검사 | 비용 0. 기본 4 유지. 0/음수/17 거부, 16-ray grazing. 설정을 조용히 낮추지 않음 |
| `a48d2f5` (4c 보완) | immediate/queued search·filter 탭을 1~64로 검사해 설정으로 무한대에 가까운 루프가 들어오지 않게 함 | 비용 0. 기본 5/16 유지. 0/음수/65 거부, 64/64 + 포화 검사. 초과 설정은 clamp하지 않고 실패 |
| `91a313f` | 기존 CPU 설정 테스트의 오래된 1080p 거부 기대 수정, 비목표 720p 거부는 유지 | GPU 변화 0. CPU `quality` 3/3 통과 |

1a~1c는 기존의 불안정한 결과를 고치는 변경이라 버그가 드러나던 입력에서 부모 영상과 같다는 주장이 아니다. 2의 ON도 다른 추정 방식이므로 동일 결과 최적화에 넣지 않는다. 3·4는 각 부모 대비 결정론 비트 일치가 통과해야 채택한다.

### 11.2 구현 범위·메모리·실행 상한

- 분리 GI 이력은 FP32 RGB 두 벌을 texel 64개, irradiance map 81개, SH 9개에 저장한다. 외부 캐시 제품은 두 평균의 합을 기존 형식으로 저장한다. 새 엔트리/epoch 리셋은 split 평균도 리셋한다. 비반사는 처음부터 긴 평균, cache-bounce는 Jacobi 구간에 즉시 갱신하고 이후 짧은 창을 쓴다. 광선 수는 동일하다. ON은 아직 "더 낫다"고 확인되지 않았다.
- 결정론 GI admission은 normal path를 대체하지 않는다. 요청 용량은 반사 생산자·GI 광선·프로브의 최악 개수로 잡으며, raw buffer가 32-bit 주소 범위를 넘으면 실패한다. request 초과 오류를 숨기거나 임의 표본을 버리지 않는다. 큰 화면에서 추가 메모리가 수백 MiB~수 GiB일 수 있다. 정렬 merge는 스레드당 binary search 최대 32회, 256원소 scan은 최대 4계층이다.
- 국소광 overflow 큐는 `min(4 * overflowWords, max(1024, pixels / 8))`개다. 48 B/항목이며, 초과는 inline 필터다. 큐 순서와 결과 byte 주소는 분리되어 있다. producer 완료 뒤 consumer가 겹치지 않는 byte를 OR한다. 큐 counter의 32-bit 최악 개수도 검사한다.
- froxel 큐에는 필요한 slice를 정확히 한 번 넣고, 용량은 모든 slice 수다. 적분 내부는 원래 최대 32 altitude substep, 기존 VSM air-walk 상한을 유지한다. 64 B FP32 scratch를 통해 원래 tile/slice index로 되돌리므로 큐 도착 순서는 FP 합산 순서를 바꾸지 않는다. 국소광의 기존 분할 수 K 및 list 순서, media 계산도 유지한다. 큐와 간접 dispatch는 표준 D3D12다.
- 기본 비동기 목록은 계속 `[]`이다. 큐를 만들었다는 이유로 async가 빨라졌다고 간주하지 않는다. 새 pass/resource의 lifetime은 render graph 선언으로 연결했다.

### 11.3 실제 검증과 로컬 실행

- `Tools/CI/Build.ps1 -Track rendercheck -Tracks all -Jobs 4 -LowPriority`로 전체 소스와 테스트를 MSVC `/W4 /WX` 빌드했다. 생성 `.obj`/scan 파일 560개를 이 작업 폴더 안에서만 제거한 뒤 수행한 빌드는 342.6초에 통과했다(`build/final-source-build.log`). 앞선 증분 빌드는 수정 테스트의 오래된 오브젝트를 재사용한 흔적이 있어 현재 소스 증거로 채택하지 않았다.
- 그 뒤 실행 상한/설정 테스트 보완의 세 C++ 오브젝트를 명시적으로 다시 만들고 전체 링크·헤더 의존성 검사를 통과했다(`build/bounds-test-build.log`, 32.0초). 검증한 구현 tip은 `91a313f`이며 이 세션 기록 커밋은 문서만 추가한다. 로컬의 기존 gate/build 캐시도 그대로 신뢰하지 말고, 새 빌드 폴더 또는 생성 오브젝트 정리 후 실제 변경 C++의 컴파일 로그를 확인한다.
- 모든 커널 변형의 200 KiB DXIL gate가 통과했다. 빌드 산출물 중 최댓값은 `FxLayerSetup.STEP0` 204560 B(한도 204800 B). froxel QUEUED0/1은 56516/45240 B, 별도 air 적분은 14064 B다. 이는 바이너리 크기이며 GPU 레지스터 수나 실행 시간의 증거가 아니다.
- GPU 장치를 만들지 않는 `unx_unit_tests quality`를 실행했다. 처음에는 기존의 1080p 거부 기대 때문에 2/3이었고, `25d639c`부터 설정은 이미 1080p를 허용함을 소스 비교로 확인했다. 테스트 기대를 수정한 뒤 **3/3 통과**했다(`build/cpu-quality.log`). GPU 디버그 레이어 검증을 실행한 것은 아니다.
- `git diff --check` 통과. 다른 checkout의 엔진 DLL/제품을 대신 사용하지 않았다. submodule과 공식 dependency cache는 사용하되 엔진/테스트는 이 작업 소스에서 빌드했다.

컴파일은 영상/GPU 성능의 검증이 아니다. 이 세션에서 실행하지 않은 것은 모든 GPU 테스트, 비트 비교, 3000프레임 곡선, 레지스터/점유율 측정, 1080p/1440p 시간이다. 2~3 ms 달성을 주장하지 않는다.

로컬 조정 세션은 기존 main의 `Tools/Verify/Verify-CloudBranch.ps1 -Ref origin/cloud/render-fixes`를 사용한다. 이 스크립트는 현재 cloud 기준 커밋에는 없고, 로컬 main에 존재하는 것을 확인했다. 로컬의 작업/검증 checkout을 이 세션에서 바꾸지 않았다. 결과는 기존 절차대로 `origin/local/verify-<커밋>`에 올린다.

기존 runner의 tests 목록에는 새 GI admission/visibility tie 및 froxel `--queue-ab`가 자동 포함되지 않는다. 로컬 GPU lock 안에서 다음을 추가하고 결과를 함께 보관해야 한다.

1. `unx_test_gi_admission --validate`, `unx_test_visibility_visibilitytests`(동심도 테스트 포함), `unx_test_shadow_localshadowtests`, `unx_test_shadow_froxeltests --queue-ab --set debug.deterministic=true`. 테스트 실행 파일은 이 소스 빌드의 `build/all/bin` 제품을 쓴다.
2. 각 최적화 커밋과 부모를 같은 scene/config, `debug.deterministic=true`, `--warmup-frames 300`, 고정 `--frames`로 비교한다. 초기 overflow 검사는 별도로 워밍업 0/초기 프레임도 포함한다. 첫 번째는 같은 커밋 반복 두 번이 byte 동일해야 한다. 그다음 부모/변경 쌍을 비교한다. SSIM/허용 오차 판정으로 비트 일치를 대체하지 않는다. 최종 캡처 byte/hash와 GPU debug/GBV 오류 0을 남긴다.
3. `reflection.batch_gi_corners=false/true`, `shadow.vsm.overflow_filter_queue=false/true`, `atmosphere.froxels.integration_queue=false/true`를 각각 단독 A/B한다. 전체 프레임과 하위 패스 합, 메모리·registers·occupancy·spill을 함께 본다. 큐 producer만 빨라진 것은 이득이 아니다.
4. 욕탕 1080p `--frames 3000 --luminance-log <파일>`에 `gi.split_bounce_history=false/true`를 비교한다. 평균 밝기가 낮아져 분산만 줄어든 경우 불합격이다. 같은 커밋 안 재현성 검사와 별도로 독립 seed의 평균·분산, 이동/가림 해제/조명 변화 응답을 봐야 한다. 확인 전 OFF 유지.
5. 시간은 **debug.deterministic=false**, 동일 출력·내부 해상도·품질 hash·직렬 큐·무경합에서 두 번 이상 측정한다. 결정론 admission/replay의 비용을 일반 프레임과 섞지 않는다. 현재 미승인 영상이 기준에 섞이지 않았는지 정지·이동 품질도 확인한다.

### 11.4 1440p 2~3 ms까지 남은 몫

최신 기준은 사용자가 제공한 `84e789f` 직렬 **[실측]**: 기차 1440p 5.33 / 1080p 3.79, 욕탕 1440p 6.10 / 1080p 4.40 ms다. 1440p에서 3 ms까지 **2.33 / 3.10 ms**, 2 ms까지 **3.33 / 4.10 ms**를 더 줄여야 한다. 이번 tip의 시간은 아직 없다.

최신 84e789f의 패스별 JSON은 이 checkout에 없으므로 패스별 현재 시간으로 꾸며 쓰지 않는다. 아래 시간은 `main:Results/Local/Timing20260929/974c6bb_*_2560x1440.json`의 pass mean 합을 다시 집계한 **과거 [실측]**이다(출력1440p/내부960p, 당시 업스케일 화질 불합격). 구조상 큰 비용의 위치와 필요 감축 규모만 보여 준다. 최신 결과로 각 행을 교체해야 한다.

| 패스 묶음 | 과거 기차 / 욕탕 ms | 3 ms 설계 배분 기차 / 욕탕 [목표] | 이번 항목이 만지는 부분 |
|---|---:|---:|---|
| R 반사 | 2.019 / 0.835 | 0.85 / 0.40 | corner·해 적재, 데칼, VNDF 재생, jobs/resolve/accumulate |
| GI | 1.576 / 1.691 | 0.80 / 0.80 | 이번 이력 분리는 성능 절감이 아니며 ON 비용 증가 가능 |
| S 그림자·VSM | 0.674 / 2.065 | 0.50 / 0.90 | 목록·국소광 overflow 분리. 페이지 렌더/요청의 큰 몫은 그대로 |
| S froxel | 0.183 / 0.554 | 0.15 / 0.20 | 같은 air slice 계산의 큐 배치 |
| M | 0.762 / 1.099 | 0.45 / 0.50 | light word/overflow 적재. BRDF·재질 계산량은 그대로 |
| V | 0.161 / 0.160 | 0.15 / 0.15 | normal mode 성능 변화 거의 없음 |
| AS | 0.111 / 0.026 | 0.06 / 0.03 | 이번 항목에 큰 절감 없음 |
| 제출 간 간격 등 여유 | 별도 측정 필요 | 0.04 / 0.02 | 새 dispatch 비용도 여기에 반영 |
| 합 | 5.485 / 6.431 (패스 합) | 3.00 / 3.00 | 2 ms는 이 배분에서 추가 1 ms 필요 |

이번 동일 계산 최적화의 합산 이득은 우선 **0.1~0.6 ms [예상]** 정도를 검증할 범위로 본다(일부 회귀 가능, 개별 행의 상단을 더한 수치 아님). 이 예상만으로 최신 기준의 2.33~3.10 ms 부족분을 메울 근거는 없다. GI split ON은 이 예산 밖의 품질 실험이다. 해상도·표본·부하를 낮춰 목표를 맞춘 것으로 보고하지 않는다.

### 11.5 후속 재설계와 결정 필요

**동일 결과 조건으로 먼저 검토할 재설계**: GI/반사 hit의 불변 조회 자료를 단계별로 분리하고, 같은 key/페이지 읽기를 묶되 광선·corner·합산 순서는 원래 index로 되돌리는 방식. GI trace와 integrate의 큰 live state를 분리해 occupancy를 확보하는 방식. 국소광 shadow의 정확한 early-out과 필터 작업 큐를 전체 slot/M fallback까지 확장하는 방식. 모두 실제 register/spill·대기 원인·새 scratch/dispatch 비용 측정이 선행되어야 하며, prefix sum이나 wave reduction으로 FP 덧셈 순서를 바꾸는 것은 동일 결과 변경으로 분류하지 않는다.

**결정 필요 — 이번에 적용하지 않은 것**:

- 분리 GI 이력 ON: 실제 게임플레이에서 에너지·시간 응답·세부가 더 나은지 먼저 확인하고 결정한다. 비용/메모리 증가도 함께 수용 여부를 정한다.
- 다른 반사/GI 추정기, reservoir/temporal reuse, proxy 근사, 더 강한 시간 필터: 정지 통계나 SSIM만으로 "보이는 것이 같다"고 할 수 없다. 움직임·가림 해제·조명 변화에서 차이가 있으면 동등 최적화 목록에 넣지 않는다. 이 세션에는 구현하지 않았다.
- 내부 해상도·광선/탭 수·GI 갱신량·광원 수·장면 부하 감소는 현재 고정 규칙에 어긋난다. 목표 달성 수단으로 채택하지 않았으며, 현 규칙 아래에서는 사용할 수 없다.

아직 완료되지 않은 것은 위 로컬 GPU 수용 검사와 2~3 ms 성능 목표의 실측 입증이다. 코드는 항목별로 되돌릴 수 있는 커밋으로 나눴고, 미완성 known-entry 경로는 남겨 두지 않았다.


## 세션 12 — d0f3cd6 로컬 회귀 수정 및 미승인 업스케일 실험 (2026-09-30)

작업 위치는 `C:\Users\USER\UnravelNext-render-followup`, 브랜치는 `cloud/render-fixes`다. main 적용과 GitHub push는 하지 않았다. 사용자의 마무리 요청 시점의 검증 자료와 제한은 [로컬 작업 상태](../../Results/Local/Followup-20260930/README_KO.md)에 보존했다.

- `da5b61a`: visibility 시험 raster 크기 불일치 및 WARNING 926의 alias 주소 쓰기를 수정. 전체 9/9 PASS, 오류 0. [예상] 시험 수정으로 렌더러 성능 이득 없음. 로컬: covering-triangle 및 raster service 회귀 유지 확인.
- `eae9c17`: 포화되지 않는 exposed-linear luminance 평균 기록. 캡처 CPU 평균과 일치 확인. [예상] 계측 비용만 해당, 게임 렌더러 성능 이득 없음. 로컬: 욕탕 3000프레임 이력 분리 비교는 미실행, 기본 OFF 유지.
- `a139daf`: 반사 split/inline 경로의 strict FP 및 캡처 프레임 정렬. 당시 기차·욕탕 frame 0/300 반복 비트 일치. [예상] strict FP 비용 증가 가능, 시간 미측정. 로컬: 최종 소스 반복 및 스위치별 비트 A/B 필요.
- `81f8288`: GI anchor가 hit까지의 자유 구간 뒤로 넘어가는 오류 수정. GI/반사 analytic 및 GPU validation 통과. [예상] min/거리 처리 추가, 표본 수 동일. **Furnace 기본 준비 프레임을 256으로 변경한 조건에서의 PASS이며, 원래 128은 11개 M outlier로 FAIL이다. 원래 조건까지 완전 해결로 보고하지 않는다.**

업스케일 수정/실험은 미커밋 상태로 보존했다. 최신 빌드 통과와 기차 화질 HF 0.9902 / SSIM 0.98595를 확인했지만 SSIM 미달 및 확대 영상의 격자가 남아 **불합격**이다. 움직임·가림 해제·조명 변화와 마지막 소스 전체의 통합 검증도 미완료다. 최종 바이너리에는 미승인 실험이 들어 있으므로 합격 제품으로 사용하지 않는다.

성능은 게임 실행 중이라는 요청에 따라 평가하지 않았다. 세션 11.4의 패스별 과거 자료와 3 ms까지 부족분 2.33 / 3.10 ms를 갱신할 새 성능 근거가 없다. 반사/GI 추정 변경 및 추가 시간 필터는 게임플레이 동등성을 입증하기 전까지 결정 필요다. native 강제 복귀, 품질/표본/부하 감소 또는 합격 기준 완화를 채택하지 않았다.

## 세션 13 — 재설계 V2 P0·P1 (로컬 세션, 브랜치 redesign-v2, 2026-09-30 밤 ~ 10-01 새벽)

작업 폴더는 `C:\Users\USER\UnravelNext-redesign`, 시작은 45f1faa다. 실측 자료와 표는 [`Results/Local/Redesign-P0P1/README_KO.md`](../../Results/Local/Redesign-P0P1/README_KO.md)에 있다. 모든 수치는 이 PC의 [실측]이고, 1080p는 내부 1280×720, 1440p는 내부 1707×960이다.

### P0 측정 기반 (끝남, 조정 확인 대기)

- `a72d614` `GpuScene::setLights`: 빌드 뒤 광원 편집(세기·색만 바뀌면 그 광원의 revision만, 모양이 바뀌면 장면 revision도).
- `29b2cfe`·`1f86efd` renderergate: `--path-time`, `--path-time-list`/`--segment-frames`, `--path-rotate`, `--path-translate v[,strafe]`, `--motion-start`, `--cut-at F[:T2]`(kDiscontinuityCut), `--light-toggle-at F,i`, `--sun-step-at F,deg`, `--capture-frames`, `--capture-layers final,gi,refl,shadow,reflmode,depth`(+ `_alpha`), `--frame-log`(카메라 움직임 가림 해제 d = `GateDisocclusion.hlsl`, GI·반사·VSM 계수), `--gi-cache-stats`(GI 캐시를 읽어 부모–자식 수렴값 차이, 지난 프레임에 읽힌 항목의 갱신 수), `--list-lights`. 두 실내 장면에는 카메라 경로가 없어서 예전 caps의 `--moving`은 정지 화면이었다. 이제 90°/s 회전으로 잰다.
- `5f38ca2`·`59beb41` Tools/Verify: `motion`(90·180°/s 회전, 1.5·4 m/s 걷기·달리기, 움직임 1~32프레임, 같은 자세의 정지 수렴 대비, 모션 블러 끔, 게이트 정지 깜빡임), `relight`(램프 하나 끔, 해 10°), `cut`(180° 컷), `-Local`(이 폴더 빌드). relight·cut의 기준은 **같은 실행을 1999프레임까지 이어 간 화면**이다(아래 이유). `motion_metrics.py`: 타일 P95, 층 σ, 층 오차, 무늬 지수, 깜빡임.
- `adbb154` CPU 모델: MB-B 표본 목록(`SampleListModel.py`), P1 컷 수렴 모델(`GiConvergenceModel.py cut`).
- 기준선(시작 렌더러): 자료 README 1절. Q-A 욕탕 1080p 76.9/55.6/50.6/32.5/13.7 %(1·4·16·64·256프레임), 조정 세션 기준선과 같은 규모. Q-E 욕탕 hf_ratio 7.1(업스케일에만 계단선·빗살), 기차 0.63~0.83(흐림). 카메라 가림 해제 d: 90°/s 3.0~3.3 %/프레임, 180°/s 5.8~6.3 %.
- 하지 않은 것: `unity` 단계(배포가 필요), J·J_mix·매질 노드·반사 hit 기하 불일치 카운터(P2·P6에서 필요할 때), 반사 blur 분포는 모드 텍스처(`reflmode` 층: 모드와 G 간격)로만 본다.

### P1 GI 수렴 (구현했으나 설계 표 미달 — 다음 단계로 가지 않음)

- `bfe96ce`: 갱신 우선순위(`gi.update_tiers`), 부모 사전 추정 패스 `r.gi.prior`(`gi.parent_prior`), 광원 변화 무효화(`gi.light_invalidation`, 기본 켬), 재시작 판정(`gi.relight_restart`), hit 광원 발자국 평균(`gi.hit_light_footprint`), 진단 비트 4096·8192·16384, GiAnalytic 7번. 앞의 둘과 뒤의 둘은 **기본 끔**이다(근거는 아래). `e7ef452`: 컷 첫 프레임 모션 블러 번짐 결함 수정(욕탕 컷 1프레임 타일 P95 약 140 % → 37 %).
- 설계 표(P1 뒤 25/12/6/3/2 %)에 대해 컷 뒤 실측은 욕탕 37/20/16/14/8 %, 기차 31/26/22/19/8 %이고, P1을 켜도 끈 것과 1~2포인트 안이다. 원인을 찾은 결과는 셋이다(README 2절).
  1. **욕탕 간접광의 두꺼운 꼬리.** 램프 바로 옆 천장에 떨어진 GI 광선이 드물게 아주 큰 값을 준다. 1000 nit에서 자르면 수렴 에너지가 44 % 준다. 광원 선택 없이 모든 광원을 그림자 광선으로 합해도(D-8의 결정적 합 구조) 반점이 그대로라 **D-8만으로는 해결되지 않는다**. 발자국 평균은 반점을 없애지만 에너지를 2~4 % 잃는다.
  2. **GI 캐시의 에너지 편향과 이력 의존.** 경로 추적기 대비 욕탕 −11~−25 %, 2000프레임에도 수렴 중이다. 같은 시점에서 카메라 이력만 달라도 수렴 수준이 최대 38 % 다르다. 그래서 cold-start 기준으로 잰 컷·조명 변화 수치는 틀렸고, 도구를 같은-이력 기준으로 바꿨다.
  3. **부모–자식 차이**가 p50 5~8 %, p95 40~50 %다. V2.1 10.6의 첫 프레임 3 % 근거가 성립하지 않는다.
- 설계에서 벗어난 것과 이유:
  - 1.1(c) 캐시 공간 필터는 넣지 않았다. 갱신마다 저장값을 이웃과 평균하면, 수렴한 칸의 측정 가중치(1/32~1/256)보다 필터 가중치(~0.5)가 커서 확산처럼 번진다. 이웃 정보는 사전 추정으로만 넣는 것이 맞다고 판단했다.
  - 재시작은 통계 판정 대신 **아는 사건**(광원 revision)으로 한다(`gi.light_invalidation`). 욕탕 램프 끔 4/8/16/64프레임: 108/105/102/74 % → 97/81/68/55 %.
  - 부모 조회를 GiIntegrate 안에서 하던 첫 판은 같은 디스패치에서 갱신 중인 부모를 읽는 경쟁이라 결정론을 깨뜨렸다. 별도 패스로 옮겨 고쳤다(비트 동일 확인).
- 시험(기본값): reflectionanalytic(256프레임 2회, 128프레임 1회), gianalytic(7번 포함), gianalytic --determinism, hostmotion, vsmtests, localshadowtests, shadingtests, volumetests, froxeltests 통과. `debug.deterministic` 게이트 두 실행 비트 동일(욕탕·기차 1080p). 128프레임 furnace는 시작 커밋 빌드에서도 한 번 실패했다(확률적 시험).
- 성능은 재지 않았다(P1 기본값은 광원 무효화 말고는 이전 경로와 같다).

### MB-B (P3 전제) 결과

출력 격자에 맞춘 지터면 s = 1/2은 4프레임, s = 2/3은 9프레임에 원래 해상도와 같다. 설계의 조명 일치 상자는 실제 디테일을 버리고(SSIM 1.000 → 0.935), 느린 이동(0.3 px/프레임)은 어느 변형도 닿지 않는다(0.79~0.93) [CPU 모델]. P3 재구성은 구현 전에 다시 설계해야 한다.

### 조정 확인 요청

1. P0 도구: `Verify-CloudBranch.ps1 -Ref origin/redesign-v2`로 독립 확인(기본 단계에 motion·relight·cut 추가, 약 40분 더). 결과 해석은 자료 README 1절 주의를 따른다.
2. P1: 설계 표 미달이다. 규칙대로 P2로 가지 않았다. 다음을 정해 주기 바란다.
   - (a) GI 에너지 편향(경로 추적기 대비 −11~−25 %)과 이력 의존을 먼저 고칠지. 수렴 수준이 움직이면 "몇 프레임 안에 깨끗"을 잴 기준 자체가 흔들린다.
   - (b) 두꺼운 꼬리(램프 핫스팟)를 에너지를 지키며 줄이는 방법. 후보: 발자국 모델의 에너지 보정, hit 국소광을 칸 단위로 캐시해 hit이 읽게 하기. 설계 세션 과제로 보인다.
   - (c) V2.1 10.6(첫 프레임 3 %)과 6절 P1 기대값을 이 실측으로 고칠지.
3. 이 세션은 이 판단을 기다리는 동안 (a)의 원인 조사(다중 반사 수렴, 이력 의존)를 계속할 수 있다. 완료 주장은 하지 않는다.

## 세션 13 이어서 — GI 편향 원인 (P1′-b 준비, 2026-10-01 새벽)

조정 세션 지시: GI 편향(경로 추적기 대비 −11~−25 %)과 이력 의존을 정확성 결함으로 고친다. 원인을 차수별로 나눴다. 욕탕, 호스트 카메라, 엔진 1080p 원래 해상도 → 540p 상자 평균, 기준 `unx_reference` CPU 960×540 1024 spp다. 모두 [실측]이고 진단 옵션은 dc449b3이다.

1. **기준 추적기가 clearcoat(A9)를 구현하지 않는다.** CPU·GPU 추적기 어디에도 코팅 층이 없다.
   - 직접광만 보면 벽만 0.84, 바닥은 1.000이었다. 그림자, 발광체, 노멀맵, 스펙큘러를 차례로 지워 봤지만 원인이 아니었다.
   - 벽 타일은 clearcoat 1.0이다(도기·대리석·목재·락커도 코팅). 램프 하나, 흰 재질에서 코팅 켬/끔의 벽 밝기 비는 **기준 1.000, 엔진 0.570**이다.
   - 이론: 매끈한 코팅(η 1.5) 아래 반사율 0.5 확산면의 총 확산 반사는 약 0.55배다. 엔진이 맞고 기준이 틀리다.
   - 기준 추적기가 공유 모델(`MaterialModel.h` evaluateCoated)로 코팅을 구현해야 ±2 % 판정을 할 수 있다(담당 결정 요청).
2. **코팅을 뺀 장면(두 쪽 모두)으로 본 엔진의 실제 결함:**

| 실행 | 599프레임 편향 | 1999프레임 편향 | 1999 타일 P50 |
|---|---:|---:|---:|
| 처음부터 이 시점 | −4.2 % | **−0.6 %** | 1.1 % |
| 반대쪽 300프레임 뒤 컷 | −8.8 % | **−7.6 %** | 7.5 % |

   - 처음부터 그린 경우는 결국 ±1 % 안에 닿지만 느리다(600프레임에 −4 %).
   - 이력이 다르면 7.6 % 낮은 수준에 머문다. P1′-b(11.2: S/B 분리, 가중치 = n의 함수, 데이터 없음 = 부모값)가 고칠 대상이다.
3. **1440p 컷 잡음 바닥 16.3 %**(조정 확인 fe7f9b4): 같은 설정 두 실행의 1999프레임 평균 휘도가 14.6 % 다르다(1080p 2.2 %). 한 화면이 덜 수렴한 것이 아니라 캐시가 실행마다 다른 수준에 자리 잡는 것이다. P1′-b 판정 항목(이력·실행 차 ≤ 2 %)에 넣는다.
4. **refl 층 지표가 수천 %**: `view.reflection`은 영상 아래에 타일 행(height + tilesY)을 더 갖는다. 캡처가 그 행까지 읽었다. 영상 행만 복사하게 고쳤다(dc449b3).
5. **무늬 지수 4~60 (3 px 주기)**: 내부 2/3 배율의 3 : 2 격자 위상이다. 움직이는 동안 출력 이력을 매 프레임 재표본화하는 지금의 업스케일에서 나온다. 조명 층(P1′·P2)과 무관하고 P3′(격자 정렬 지터, 표본 목록)가 다룰 몫이다. P1′·P2 판정에서는 이 지수를 참고로만 보고, 내부 해상도 층(gi)의 무늬 지수로 판정하겠다.
6. 디스크: 01:40에 C:가 가득 찼다(내 PFM 42 GB, 지움). Verify에 `-DropPfm`을 넣었다.

## 세션 13 이어서 (2) — 기준 추적기 코팅, GI 편향의 엔진 결함 셋 (P1′-a 막힘, P1′-b, 2026-10-01 새벽)

모든 수치는 [실측]이다. 욕탕·기차 호스트 카메라, 엔진은 1080p 원래 해상도(→ 540p 상자 평균), 기준은 `unx_reference` CPU 960×540이다. "처음"은 처음부터 그 시점에서 그린 경우, "이력"은 반대쪽을 300프레임 본 뒤 컷한 경우이고, 값은 1999프레임 편향이다. 커밋은 b236f30~b1256b9다.

1. **P1′-a 막힘(보고 완료, 조정 답: (A) 불채택, D-12/D-14는 사용자 결정)**
   - 발자국 닫힌 식 자체는 맞다(무차별 적분 ±1 %).
   - 같은 hit 집합의 에너지 감사(experiment 32768)에서 발자국/점 비는 욕탕 0.946, 기차 0.914다(발자국 크기 1). 가시성을 빼고 합해도 같다.
   - 원인은 발자국이 실제 표면(갓, 구석) 밖으로 넘치는 것이다. hit 평면 하나만 아는 광선당 원뿔 평균은 불편일 수 없다.
2. **기준 추적기 A9**(151e9e8, bb1fe67): CPU 추적기에 clearcoat와 sheen을 넣었다(공유 모델 evaluateCoated/evaluateSheen, 코팅 VNDF 혼합 + MIS).
   - 시험: 평가 = 모델 1e-5, 방향 알베도 = 구적 ±0.5 %, 코팅 평면 점광원 = 닫힌 식 2e-6.
   - 추정기 버전을 2로 올렸고, 층 있는 장면에서 GPU/WARP 장치는 실패하게 했다(GPU 추적기에는 층이 없다: 남은 누락).
   - 욕탕 기준의 코팅/비코팅 비는 0.718(벽 0.67~0.70)이다. 앞 절의 "엔진 0.570이 맞다"는 틀렸다. 정정한다.
3. **결함 A — ray hit에 코팅이 없었다**(07ca54c). GI·반사 hit이 맨 바탕으로 음영됐다. 직접 뷰(ShadeOpaque LAYERED)와 같은 코팅·sheen을 hit에 넣었다.

| 코팅 욕탕 | 처음 | 이력 |
|---|---:|---:|
| 고치기 전 | +25.5 % | +13.8 % |
| hit 코팅 | −7.5 % | −2.7 % |

4. **결함 C — 이동 평균 창의 통계 판정**(16a7269, 기본 켬). 창(32/256)을 항목 자신의 fast mean·spread로 골랐다. 두꺼운 꼬리 표본이 주변 가중치를 바꿨다(11.2-3 위반).
   - 새 규칙 `gi.history_window_rule = "lighting"`: 태양이 최근 256프레임 안에 바뀌었으면 32, 아니면 256이다. 옛 규칙은 "samples"로 A/B에 남겼다.
   - GiAnalytic 8번(b1256b9, 아래)의 점 추정기, 1920프레임: 옛 규칙 +6.9 / +12.9 / +6.9 / +10.0 %, 고정 창 +0.33 / −0.03 / +0.14 / +0.28 %.
   - 코팅 욕탕: −6.0 / −7.3 %(이력 차 4.8 → 1.3 pp).
   - 회귀: GiAnalytic(ρ 0.5, 8번 포함) PASS, 결정론 비트 동일, ReflectionAnalytic·HostMotion·VSM·LocalShadow·Shading·Volume·Froxel 통과.
   - ρ 0.9 백색로는 전후 모두 실패한다(평균 −0.38 → −1.31 %). 정적 창 256에서 다중 반사 수렴이 느리다.
5. **결함 B — 셀 값이 앵커 한 점의 조도**(e53ad28, `gi.anchor_resample`, 기본 끔). 앵커가 생성 순서·이력으로 정해진다. 조회된 표면점 가운데 무작위로 앵커를 옮기면:

| | 처음 | 이력 | 차 |
|---|---:|---:|---:|
| 무코팅 욕탕, 창 규칙만 | −2.7 % | −10.2 % | 7.5 pp |
| 무코팅 욕탕, + 재표본 | −4.5 % | −4.3 % | 0.2 pp |
| 코팅 욕탕, + 재표본 | −8.1 % | −6.7 % | 1.4 pp |
| 기차(1024 spp 기준, 잡음 큼), 창 규칙만 | −5.0 % | −4.7 % | 0.3 pp |
| 기차, + 재표본 | −4.5 % | −2.7 % | 1.8 pp |

   - **기본 켬으로 하지 않는다.** 기차 재표본 + 이력에서 옻칠 기둥 윗판에 셀 크기의 흰 안개 얼룩이 눈에 보인다(`Results/Local/Redesign/gia/train_crops.png`, 오른쪽 아래). 어떤 조회가 앵커 후보가 될지 다시 설계해야 한다. 비용 A/B(1440p, timing 잠금)는 대기 중이다.
6. **S/B 분리(11.2-2)는 채택하지 않는다**(e53ad28, `gi.bounce_split`, 기본 끔, 항목당 48 B: 긴 평균 M + L1 SH의 B − B̄).
   - 창 규칙과 함께 무코팅 욕탕: B 교체 −2.4 / −7.8 %, B 창 4는 ρ 0.9 백색로 160프레임에 −38.7 %(모델이 예측한 1 − a_B(1 − ρ) 수축).
   - B 교체는 백색로의 최악 프로브 잡음을 1.6 → 4.0 %로 키운다.
   - `GiConvergenceModel.py split`(a530b3a)는 11.2-5(iii)의 모델 시험이다.
7. **GiAnalytic 8번을 넓혔다**(b1256b9). 점광원 옆 큰 천장 / 20 cm 판 / 천장·소핏 구석 / 갓 안쪽 + 대조(1 m 아래)의 바닥 단일 반사를 광원 방향 구적 정답(가림 포함, Python 면적 적분과 0.03 %)과 ±0.5 %로 비교한다.
   - 발자국 평균은 −96 %(판), −91 %(갓), −21 %(구석), +2 %(천장)로 보고된다.
8. **남은 것**
   - 남는 수준: 무코팅 −4.5 %, 코팅 −6~−8 %, 기차 −5 %. 600 → 2000프레임에 천천히 오르므로 다중 반사 수렴 속도(ρ 0.9 백색로 −1.3 %와 같은 원인 후보)다.
   - 기차 4096 spp 기준으로 다시 잰다(코팅 커스틱 경로 때문에 1024 spp 반쪽 relMSE 1.41).
   - 그다음 D-12/D-14 근거를 만든다(조정 지시).
9. **조정 확인 요청**:
   - 결함 A·C 수정(07ca54c, 16a7269)과 기준 추적기 코팅(151e9e8)을 확인해 달라.
   - 앵커 재표본을 기본 끔으로 두는 판단(안개 얼룩)과 S/B 분리 불채택을 확인해 달라.
   - 11.2의 고정점 시험 (i) ρ 0.9와 (ii) ±3 %는 아직 통과하지 못했다.

### 추가 (①: 남는 수준의 원인, 같은 새벽)

[실측, 무코팅 욕탕, 1080p, 1999프레임, 기본 설정]

- 직접광 + 1차 반사: 엔진 experiment 512 대 기준 `--surface-order 1:2`가 +0.23 / +0.26 %다(두 실행). 정확하다.
- ρ 0.9 백색로: 1000프레임에서 +0.34 %로 통과한다. 앞 절의 실패는 시험 기본 160프레임 때문이다.
- 긴 실행은 −2.8 / −3.6 / −3.7 %(1999/3999/5999)로 정체한다. 수렴 속도 문제가 아니다.
- **같은 설정 6번: +1.6, −0.1, −2.3, −3.2, −4.8, −6.9 %(평균 −2.6 %, SD 3.1 pp).** 다중 반사 수준이 실행마다 다르다.
  - 앞 절의 단일 실행 비교(이력 차 1~2 pp 등)와 hit 셀 크기·SH/지도·보간 읽기 진단(131072, 262144)의 차이는 이 잡음 안에 있다. 정정한다.
  - 옛 창 규칙에서는 두 실행이 0.1 pp로 일치했다. 스파이크를 빨리 잊는 대신 국소 편향이 있었다.
- 원인 후보: 다중 반사를 나르는 굵은 hit 셀의 두꺼운 꼬리 갱신 분산. 다음 단계 D-12/D-14의 판정 지표에 실행 간 SD를 넣는다.
- 앵커 재표본 비용 [실측, 1440p 출력, timing 잠금 A/B 두 쌍]: GI 패스 +0.05 ms, GPU 프레임 +0.09 ms.

### 추가 (②: D-12 측정, D-14 판단 요청)

- **D-12 경로 유도**(7ba746d, `gi.path_guiding`, 기본 끔, 광선 수 동일): 슬롯별 텍셀 CDF(균일 0.5 + 이력의 텍셀 조도 몫), 1/pdf 가중, 텍셀은 떨어진 광선의 평균이다.
  - [실측, 무코팅 욕탕 1080p 1999프레임] 4번 실행: 평균 −4.1 %, SD 1.5 pp. 기본은 6번 평균 −2.6 %, SD 3.1 pp다. 목표(SD ≤ 0.5 pp, 평균 ±1 %)에 못 미친다.
  - GiAnalytic 8번의 프로브 P99는 그대로다(텍셀보다 작은 핫스팟).
  - K 텍셀이 갱신마다 광선을 못 받는 회귀가 있다(백색로 5.1 %, 열린 하늘 27.9 %, 7번 실패). 채택 근거가 없다.
- **D-14**: 캐시 항목은 간접 조도만 저장하므로, 직접광 핫스팟을 "밝은 셀"로 고를 수 없다(새 나가는 복사휘도 누적기가 필요하다). 광선 수를 유지하려면 텍셀 광선 일부를 대체해야 한다(K 텍셀 갱신 손실).
  - 조정 세션에 (가) 그대로 구현 / (나) 되먹임 구조와 함께 설계 세션으로 넘김 가운데 판단을 요청했다(권장 (나)).

## 세션 13 이어서 (3) — 누적기, 노출, 게임 결함, VSM 래스터 넘침, 수조, P2 착수 (2026-10-01 낮)

수치는 모두 [실측]이고, [예상]은 따로 표시했다. 커밋은 e1b6bea, cdfec8b, cfdfd9f, 19323a2, 3ca3ae8, 2a87837이다. main은 70b2d84로 병합했다.

1. **P1″-a 직접광 누적기**(19323a2, 기본 끔, A/B 스위치)
   - 설계 12.1의 창 평균은 에너지를 2.4 % 잃었다(감사 0.976).
   - frame 모드(GiAccFix)와 비율 추정(Σk·T / Σk)을 더해 감사를 1.00000으로 맞췄다. 0.98의 원인은 셀 안 알베도–조도 상관이었다(흰 재질 + 평균 방식 감사가 1.00000).
   - 무코팅 욕탕, 4회 × 2000프레임:

   | 설정 | 평균 | SD |
   |---|---|---|
   | 창 평균 | −8.93 % | 1.35 pp |
   | frame | −5.53 % | 1.49 pp |
   | frame + ratio, 셀 0.5 | −3.57 % | 0.71 pp |
   | frame + ratio, 셀 0.25 | −2.08 % | 1.28 pp |

   - GiAnalytic 9: 셀 0.25에서 누적기 줄은 모두 통과했다. 다만 "configured estimator" 판 +0.65 %는 실패했다.
   - 목표(평균 ±1 %, SD ≤ 0.5 pp)는 못 맞췄다. 추정기 개정은 설계 세션 A로 넘겼고, P1″는 조정 지시로 여기서 멈췄다.
2. **노출**(3ca3ae8)
   - snap 프레임(첫 프레임, 컷·복원 뒤 히스토그램이 돌아오기 전)은 같은 프레임 GPU 미터링으로 보정한다. 그 전에는 첫 2~3프레임이 EV 14로 검게 나왔다.
   - 폴백 타일 미터링 결함을 고쳤다. 같은 프레임에서 폴백 전부 대 없음의 EV가 4.79 대 5.84였고, 지금은 4.30 대 4.33이다. 남은 0.03 stop은 메모 항목이다.
   - 남은 첫 프레임 밝기 차이는 GI 냉시작 몫이다. 조정 판단에 따라 노출로 가리지 않는다.
3. **게임 결함**
   - 유리가 하얀 원인: 게임 설정의 visibility.coverage_layer = false에서는 투과 층(A6)이 돌지 않는다. 켜면 정면 창이 맞게 나온다.
   - coverage 켜는 비용(1440p): lounge +2.06 ms, bath +1.70 ms. 대부분 m.coverage 합성이고, 기록당 21~24 ns로 기록 수에 비례한다. 설계 A(V2.5 고칠 것 7)로 넘겼다.
   - 거울: 광선 반사로 맞게 보인다. 평면 카메라가 없는 것은 비용 규칙 결과다. 후보 순위 수정(늦게 보이는 큰 면)은 미커밋이다.
   - 렌더 그래프 재계획: FX 틱의 import 순서를 고정해 측정 프레임의 재계획 11 → 0이 됐다(e1b6bea).
   - 앵커 유체 NP_FluidGpuView2(cfdfd9f)를 넣었다.
   - GPU 잠금 조각 v1.83(cdfec8b)을 넣었다.
4. **VSM 래스터 넘침**(기차 라운지, 미커밋)
   - 0프레임 태양 래스터가 가시 1.67 M / 쌍 2.07 M을 요구해 용량 1 M을 넘었다. 캐스터가 빠진 페이지가 캐시에 남아 f60에도 그림자가 없었다(104k 화소).
   - 구조 수정을 구현했다.
     - (a) 요청을 뷰별 구조 상한(닿는 인스턴스의 컷 상한 합)으로 묶는다. 국소광은 면별 절두체로 센다. 태양은 메시 잎 클러스터 수로 세고, 빌더가 "부모 그룹 ≤ 자식"을 검사한다.
     - (b) 쌍 목록을 없애고 AS가 행별 접두합으로 메시 그룹을 띄운다.
     - (c) 안전장치: DepthRasterOverflows와 오류 비트 0x20.
   - 정확성 [실측]: 그림자 층 f0 = f60 = f300이 비트 동일하고, 넘침 0, 오류 비트 0이다. 대상은 기차 1080·1440, 라운지, 욕탕이다.
   - 첫 timing에서 정상 프레임이 +0.86~1.34 ms 늘었다. 원인은 요청 분할(14 → 33)이다. 상한을 조인 뒤 다시 잰다.
5. **수조 붕괴**: PoolTests 7(10분 무원천·쉬는 물체 재전송)을 통과했다(2a87837). 렌더러 해석기는 안정하고, 원인은 해석기 밖이다(B 로그 대기).
6. **P2 재구성 A 착수**(조정 지시: 사용자 "노이즈 개선이 없다"). L_gi 1단계를 미커밋으로 구현했다.
   - 화소별 상대 σ, 분산 적응 필터 반경(최대 3셀), 층 이력 8(정확 재투영, 3σ 클립).
   - 판정 run65(none / adapt / both)는 사용자 게임 중이라 GPU 대기다.
7. **조정 확인 요청**
   - 3ca3ae8(노출)과 2a87837(수조 게이트)을 확인해 달라.
   - VSM 래스터 수정 커밋 조건(정상 프레임 증가 ≤ 0.1 ms)을 다시 재고 보고하겠다.

## 세션 13 이어서 (4) — VSM 래스터 커밋, L_gi 넓은 층, 냉시작 누설과 닫기 (2026-10-01 오후)

수치는 [실측]이고 전부 사용자 게임 실행 중에 쟀다(각 1–2회). [예상]과 미측정은 따로 적었다. 14:42부터 GPU HOLD라 그 뒤는 코드 작업만 했다. 세션 간 메시지가 막혀(자동 왕복 한도) 이 절이 조정 세션으로 가는 보고다.

커밋: acd700e(잠금 조각 v1.85), b208cc6(잠금 조각 v1.86 우선 트랙), **ecdba03(VSM 래스터 넘침 구조 수정, INTERFACES v1.84)**. L_gi와 냉시작은 미커밋(회귀 테스트가 HOLD로 밀림).

1. **결함 큐 1 — VSM 래스터 넘침**(ecdba03, 기본 켬, 스위치 `shadow.vsm.raster_split`, `visibility.raster_amplification`)
   - 정확성(면별·리프 상한 포함): 기차 라운지 1080 그림자 층 f0 = f60 = f300 비트 동일, 욕탕 라운지 f0 = f300 비트 동일, 넘침 0, S 오류 비트 0x0, VisibilityTests 9/9, VSMTests, LocalShadowTests, 클러스터 빌더 15/15.
   - 비용(패스 최소값 합): 요청 수 4 → 14(기차), 14 → 23(라운지), 프레임 바닥 +0.5–0.8 ms. 조정 결정으로 커밋했고 큐 1b(2단 컬)로 되찾는다.
   - 최종 화면에서 가문비 밝은 화소가 사라졌는지는 다시 보지 않았다(그림자 층 동일성만 확인).
   - 알림: 재확인 조각은 HOLD 전(14:40:04)에 락을 잡아 14:50:23까지 돌았다. 그중 6분은 CPU 테스트(클러스터 빌더)를 GPU 조각에 넣은 실수다.
2. **L_gi 진단**
   - "f1에서 GI 층이 0"이라는 앞 보고는 틀렸다. 그 프레임의 층이 EV 14로 사전 노출돼 있었다. 노출을 빼면 f1의 GI는 수렴값의 0.5–0.8이다.
   - 화면 프로브는 광선을 쏘지 않고 캐시를 읽기만 한다. "젊은 셀은 프로브와 혼합"은 새 정보가 없어서 지웠다(측정: 얼룩 지표 0.66 → 0.66).
   - GI 얼룩은 수렴 프레임(f299)에도 남는다(셀 크기 구조 rms 0.31, 로그 단위). 시간 누적으로는 못 지운다. `gi.anchor_centroid`(P1″-b)는 효과가 없었다(0.308 대 0.303, 1회).
3. **L_gi 넓은 층**(`gi.screen_wide_filter`, 지금 기본 false; 새 파일 GiProbeFilter.hlsl, GiProbeWide.hlsli)
   - 프로브(픽셀의 1/64)의 SH를 평면·법선·σ 비례 휘도 정지로 여러 셀에 걸쳐 평균한다(5×5 à-trous × 3). 화소는 좁은 캐시 값의 상대 σ(엔트리의 실측 퍼짐/√표본수)가 2 % 이하면 캐시 값, 6 % 이상이면 넓은 층을 쓴다.
   - bath lounge 1080p, 최종 화면 타일 오차 P50/P95(자기 f299 대비):

   | 설정 | f1 | f4 | f16 |
   |---|---|---|---|
   | none | 42 / 213 % | 47 / 379 % | 25 / 91 % |
   | wide | 32 / 146 % | 26 / 148 % | 22 / 100 % |
   | wideonly(항상 넓은 층) | 31 / 138 % | 23 / 92 % | 14 / 68 % |
   | wide + miss_closure | 26 / 125 % | 22 / 117 % | 15 / 60 % |

   - 눈: 벽·천장·기둥의 블록 얼룩이 f1부터, 90°/s 회전 중에도 사라진다. 남는 것은 광택 바닥·탁자의 반사 스페클(S2 층)이 대부분이다. 목표 8/5/3 %에는 못 미친다.
   - 그림(Results/Local/Redesign/items/p2/): grid_lounge_1920x1080_final.png, _gi.png, _rot.png, grid_lounge_closure_gi.png, _final.png.
   - **편향**(f299, 필터 없는 수렴 4회 평균 대비, 128 px 타일 P50/P95): 프로브 SH 그대로 13.7 / 59 %(바닥 최대 2배 밝음, 벽 어두움). 원인은 프로브의 SH 읽기와 화소별 조회(9×9 맵, 짝 코너, 앵커 가시성)의 수준 차다. 준비 패스(r.gi.probe.wide)가 SH를 프로브 법선에서의 화소별 조회 값으로 스케일하게 고친 뒤 10.9 / 28 %, 8.2 / 24 %(필터 없는 실행 하나의 편차 5–9 / 14–31 % 안).
   - 그래도 반복되는 무늬가 남는다(같은 설정 두 실행의 편차 지도 상관 0.71, 64 px에서 rms 약 15 %): 천장 간접등 띠와 보 옆 번짐. 그래서 "항상 넓게"가 아니라 σ 혼합을 유지한다. 필터에 분산 전파(패스마다 σ·√Σw²/Σw)와 패스 수 키(`gi.screen_wide_passes`)를 넣었다. **이 판은 빌드만 했고 미측정이다.**
   - 비용은 아직 재지 않았다. [예상] 프로브 패스 4개는 픽셀의 1/64, 화소 쪽은 프로브 16텍셀 읽기와 SH 평가가 는다.
4. **냉시작 누설과 닫기**(`gi.miss_closure`, 기본 false)
   - 발견: 같은 바이너리·설정에서도 일부 실행은 f3–f16의 GI가 수렴값의 1.5–1.8배로 튄다(청백색). 원인 추정(색과 코드 근거, 수정 A/B로 확인 중): 컷 직후에는 모든 레벨이 젊어서 GiTrace의 폴백이 거친 셀을 앵커 가시성 검사 없이 읽고, 벽 너머 낮빛을 가져온다. 수렴 수준도 실행마다 −7…+17 % 다르다.
   - 수정: 폴백은 앵커가 그 점을 보는 코너만 쓴다. 데이터 없는 반사분은 엔트리 자신의 조도로 닫는다(E = E_측정 / (1 − Σ 알베도·w·cos/π), 채널별 R ≤ 0.9).
   - A/B(bath lounge, GI 층 절대 수준, 자기 f299 대비): 끔 f1 0.52–0.62 / f4 0.63–0.86, 켬 f1 0.90–1.04 / f4 1.17–1.24 / f16 0.96–1.10. f3–f4의 +17–24 %가 남는다(닫기 근사의 과대 또는 남은 누설: 화소별 조회와 프로브 읽기, 반사 hit은 아직 젊은 레벨을 검사 없이 읽는다).
   - 수렴 편향: f299 절대 수준 켬 71.8 대 끔 4회 67.1–84.3(평균 72.2). 실행 간 편차 안이다. GiAnalytic은 HOLD로 아직 못 돌렸다.
   - 조정 지시대로 수준 변화는 기록으로 두고 기본값은 false다.
5. **S2에 넘긴 것**: 반사 층 컷 뒤 기준(노출 보정 없이: 수준 0.74–0.85 → 0.90–1.00, 타일 P50 45–64 % → 14–32 %), 평면 반사 후보 순위 패치(S2가 큐 5로 맡음, R 트리에서는 되돌림), reflShadeHit의 폴백 읽기에도 같은 누설 경로가 있다는 것.
6. **GPU 잠금**: v1.85/v1.86 대기자가 섞여 12분 교착했다. main v1.87까지 병합했다(23d18e3).
7. **조정 확인 요청**
   - ecdba03(VSM)을 확인해 달라. 핫픽스에 넣을지는 조정 판단이다.
   - HOLD가 풀리면 돌릴 순서: lounge 재측정(분산 전파 필터, 편차 지도) → train → hall 1080/1440 → 회귀 테스트(gianalytic, reflectionanalytic, hostmotion, shading, volume, froxel) → 비용(timing). 순서를 바꿀 것이 있으면 알려 달라.
   - `gi.screen_wide_filter` 기본값은 위 측정 뒤에 정하겠다. `gi.miss_closure`는 기록으로 둔다.
   - 이 세션에서 다른 세션으로 보내는 메시지가 사용자 입력 전까지 막혔다. 급한 것은 이 문서로 적는다.

## 세션 13 이어서 (5) — GPU 중단 뒤 코드 작업: 끝낸 목록과 게임 뒤 검증 목록 (2026-10-01 오후)

사용자 지시(14:45, 조정 전달)에 따라 GPU 실행은 모두 내렸고 코드와 빌드만 했다. 아래 커밋은 전부 **빌드 통과까지**이고, GPU 검증은 게임 뒤에 한 번에 한다. 세션 간 메시지는 이 세션에서 여전히 막혀 있어(사용자 입력 전까지) 이 절이 조정 세션과 S2로 가는 보고다.

### 끝낸 것 (origin/redesign-v2)

| 커밋 | 내용 | 기본값 | 지금까지의 근거 |
|---|---|---|---|
| ecdba03 | 큐 1: VSM 래스터 넘침 구조 수정(+ 큐 10 updateLocalLights break 포함) | 켬 | GPU 재확인 통과(절 (4)) |
| 5a96060 | 큐 1b: 요청 묶기 상한을 뷰별 LOD 컷 상한으로(2단 컬 대신) | 켬(`shadow.vsm.raster_lod_bound`) | 빌드, 클러스터 빌더 CPU 테스트 15/15 |
| 984a8d7 | P1″-b 앵커 무게중심: 기록(효과 없음 0.308 대 0.303) | 끔 | 1회 실측 |
| bc05387 | P2 L_gi: σ, 적응 반경, 넓은 층(수준 보정·패스별 σ), 층 이력 | 켬 | 수준 보정 전·후 실측(절 (4)); 패스별 σ는 미측정 |
| 1dc8a00 | 냉시작: 반사분 읽기의 앵커 가시성(`gi.bounce_visibility`) + 닫기(`gi.miss_closure`) | 둘 다 켬 | 엄격 폴백 + 닫기 실측(절 (4)); 자기 셀 검사는 미측정 |
| f3523cf | 큐 3: `visibility.coverage_layer` 기본 켬 | 켬 | 원인 확인됨(조정 결정); 테스트 미실행 |
| ca2ab40 | 12.8 누적기 풀(`gi.hit_accumulator_pool`), INTERFACES v1.88 | 주 스위치 `gi.hit_accumulator` 끔 | 빌드만 |
| acd700e, b208cc6 | 인프로세스 잠금 조각 v1.85·v1.86 순서 | – | 빌드 |
| acc3779 | 검증 스크립트(p2slice.ps1, judge.py, judge2.py, blotch.py) | – | – |

큐 1b를 2단 컬이 아니라 상한으로 푼 이유: 2단 컬도 목록 크기는 폭주 프레임(0프레임, 전면 재그리기)의 실제 가시 수에 맞춰야 하는데, 넘친 것이 바로 그 경우였다. 뷰가 그리는 컷은 텍셀 크기에 따라 거칠어지므로(태양 레벨 2^k, 국소광 mip), 캐스터마다 "부모 오차 > 문턱 ∧ 자기 오차 ≤ 문턱"인 클러스터 수(빌더 표)를 세면 요청 수가 구조적으로 준다.

### 정정 (조정 요청)

- 오전 보고의 "광원이 수백 개면 꼬리가 두꺼워진다"는 **광원 선택 분산이 아니다**. 선택은 중요도 비례(`rtLightChoose`)이고, 전 광원 합으로도 반점이 그대로였다(V2.2 11.0 실측). 뿌리는 hit 점이 램프 옆 핫스팟에 떨어지는 **위치 분산**이고, 광원이 많으면 핫스팟 수가 는다. 그래서 원천 해법은 누적기(셀 평균)다.
- 절 (4)의 "none 실행의 튐은 벽 너머 누설"은 색(청백)과 코드에서 나온 추정이다. `gi.bounce_visibility` 단독 A/B로 분리해 확인한 적은 없다.

### S2에게 (누적기 API, ca2ab40)

- `Passes/GI/GiAccPool.hlsli`: `struct GiAccMeans { float3 A, B, C; float weight; float cellSize; };`
- `bool giAccPoolRead(pool, position, normal, footprint, out GiAccMeans m)` — pool = `FrameResources::giAccumulator`(invalid = 꺼짐; 읽는 커널은 SrvCompute, ByteAddressBuffer), footprint = hit 지점의 광선 발자국 폭(m). 쓰는 식: `radiance += m.weight * (kA*m.A + kB*m.B + kC*m.C - 점 값)`. `m.cellSize`로 "발자국의 4배 넘으면 점 값 유지"를 판단하면 된다. false = 데이터 없음.
- `giAccPoolRecord(...)`는 GI 광선만 부른다. 반사 hit은 **읽기만** 해 달라: 비율 추정기의 에너지 보존은 R을 만든 집단 안에서만 성립하고, 반사 광선은 셀 안 위치 분포와 계수가 달라 GI 독자가 받는 값을 옮긴다. 설계 12.1-1("GI 광선과 반사 광선")과 다른 점이라 감사 결과로 A와 다시 정한다.
- 엔트리 인덱스 기반 `giAccRead / giAccRecord`는 풀 모드에서 쓰이지 않는다(`gi.hit_accumulator_pool = false`일 때만 남음).
- hit 직접광 공용 함수(`rtHitDirectTerms`, `rtHitDirectFromMeans`)는 제안한 서명 그대로 좋다. GiTrace 호출부는 S2 커밋이 올라오면 R이 바꾼다.
- `gi.bounce_visibility`(GI_P1_FLAGS 780번 워드 bit 6)와 `GiCache.hlsli`의 `static bool g_giStrictVisibility`가 1dc8a00에 있다. reflShadeHit의 giCacheLevels 읽기 앞뒤에 두면 반사 hit도 앵커가 그 점을 보는 코너만 읽는다.

### 게임 뒤 GPU로 한 번에 확인할 것 (R)

스크립트: `Results/Local/Redesign/gia/p2slice.ps1`(환경 변수 P2_SCENE, P2_MODE, P2_RES; 빌드 폴더 build\dev2), 판정 `items/p2/judge2.py`, `blotch.py`. 순서는 위에서부터.

1. **새 커널 첫 실행(correctness, TDR 확인)**: GiAccFold(64비트 exchange), GiProbeFilter, GiLayerTemporal, DepthRaster.as — 뒤 셋은 이미 돌았다. GiAccFold만 처음이다.
2. **VSM 뷰별 상한(5a96060)**: 차례 `vsm2` — 그림자 층 f0 = f60 = f300(기차), f0 = f300(라운지, 욕탕), S 오류 비트 0x0(0x20 = 상한이 작았다), VisibilityTests·VSMTests·LocalShadowTests, 로그의 요청 수. 차례 `vsmtime` — lod / leaf / old 요청 수와 패스 합.
3. **L_gi(bc05387)**: 차례 `lounge2` — 패스별 σ 필터의 f1/f4/f16/f299, 회전, 편차 지도(필터 없는 수렴 실행 대비), `gi.screen_wide_passes` 2 대 3. 이어서 train, hall 1080/1440(`blend`, `none`).
4. **냉시작(1dc8a00)**: `gi.bounce_visibility`와 `gi.miss_closure` 각각 단독·둘 다·둘 다 끔, lounge·hall·train, f1/f4/f16/f299, 실행 3회 이상(수렴 수준의 실행 간 편차).
5. **누적기(ca2ab40)**: `gi.hit_accumulator=true` — 감사(64프레임, 1.000 ± 0.5 %), 무코팅 욕탕 2000프레임 × 4~6회(평균 ±1 %, SD ≤ 0.5 pp), GiAnalytic 9, 게임 장면 f1/f4/f16/회전, 결정론, 비용(r.gi.acc.begin·fold0~3·r.gi.trace), 풀 통계(빠진 hit 수).
6. **회귀(기본값 그대로)**: gianalytic, reflectionanalytic, hostmotion, shadingtests, volumetests, froxeltests, visibilitytests, vsmtests, localshadowtests — coverage_layer·넓은 층·냉시작 기본 켬 상태에서. 실패하면 어느 스위치 때문인지 하나씩 끈다.
7. **비용(timing)**: lounge·train 1080/1440 — r.gi.probe.wide·filter0~2, r.gi.screen.filter, r.gi.screen.temporal, coverage 합성, VSM 요청 수.
8. 가문비 밝은 화소(큐 1)를 최종 화면으로 확인.

### 고치지 않고 적어만 두는 것 (확인 전)

- 화소별 조회(giScreenSeen)와 프로브 읽기는 젊은 레벨을 여전히 가시성 검사 없이 읽는다(코드 사실). 화면에 누설로 보이는지는 4번 결과로 판단한다.
- 넓은 층이 대부분의 화소를 맡는다면 화소별 조회(r.gi.screen, 1440p 약 1 ms)를 σ가 낮은 곳에서만 돌려 비용을 되찾을 수 있다. σ 분포를 재야 한다.
- 누적기 접기 패스는 직접 디스패치(min(슬롯, 광선) 항목)다. 비용이 보이면 간접 디스패치로 바꾼다.
- A 요청(발광 면 면광원 변환 시 GI 이중 계산 방지, `emissiveLightsConverted`)과 HitLocalLights 결정적 합의 GI 연결은 S2·A의 공용 함수가 올라온 뒤에 한다.
- 기본 노출과 폴백 미터링의 0.03 stop 차, GiAnalytic 8 갓 +0.51 %(기존).

## 세션 13 이어서 (6) — 통합 브랜치, GiTrace 공용 함수, 발광 변환 적용이 막힌 이유 (2026-10-01 17시)

GPU HOLD 중. 코드와 빌드만 했다. 이 세션의 세션 간 메시지는 아직 막혀 있다.

1. **통합(조정 요청 1)**: origin/redesign-v2-fix(A, 739d44c까지: ShadeOpaque 두 커널 분리 포함)와 origin/redesign-v2-refl(S2, b423bf2까지)을 redesign-v2에 병합했다(fde0f12, fd8ced3, 69397e3). 텍스트 충돌은 없었다.
   - 전체 빌드(dev, all tracks) 통과, 모든 커널이 DXIL 한도 안이다. 분리 전 병합 판에서는 ShadeOpaque FALLBACK1 변종이 204,260 B(여유 540 B)로 통과했고, 분리 뒤에는 ShadeOpaque 최대 123.3 KB, ShadeIndirect 최대 91.6 KB다.
   - 지금 한도(204,800 B)에 가까운 커널: FxLayerSetup.STEP0 204,524 B(여유 276 B, FX), ReflectionTraceInline SKY0.JOB2.CORNERS1 201,932 B(S2), CoverageComposite PART1 192,972 B, GiTrace SKY0.SPLIT1 192,324 B.
   - S2의 누적기 읽기 배선 커밋이 올라오면 한 번 더 병합한다.
2. **GiTrace 호출부(조정 요청 2)**: 052d75c. 누적기의 A·B·C, 독자 계수, 점 값을 S2의 `rtHitDirectTerms` / `rtHitDirectFromMeans`로 받는다(식은 같다. GPU 값 비교는 게임 뒤).
3. **발광 면 변환의 이중 계산 방지(조정 요청 3, A 요청)는 적용하지 않았다.** 지금 제시된 방법으로는 R 쪽에서 맞게 할 수 없어서, 추측으로 넣지 않고 막힌 점을 적는다(A에게 전달 부탁):
   - `unx::lights::emissiveLights(fc)`는 부를 때마다 `g.importBuffer`를 한다(EmissiveLights.cpp 끝). 렌더 그래프는 같은 자원의 import를 합치지 않으므로(INTERFACES v1.81의 fxLights와 같은 문제), M과 R이 한 프레임에 각각 부르면 같은 버퍼가 두 번 import된다. 프레임당 한 번만 import해 돌려주는 캐시나, core가 먼저 넣어 두는 `FrameResources` 필드가 필요하다.
   - Lights는 E 트랙 라이브러리이고 GI는 R 트랙이다. R이 그 함수를 직접 부르면 R → E 링크 의존이 생긴다(E가 꺼진 빌드에서 깨짐). `FrameResources` 필드면 이 문제도 없다.
   - 발광 삼각형 MIS 표본에서 변환된 재질을 빼려면 표본의 재질 번호가 필요한데, `RtEmissiveSample`(HitLocalLights.hlsli, S2 소유)에는 재질 구조체만 있고 번호가 없다. `uint material` 한 필드가 필요하다.
   - 방출을 0으로만 하면 화소의 이중 계산은 없어지지만, 그 발광체의 1회 반사광(발광체 → 면 X → 화소)도 같이 없어진다. X의 조도는 캐시(광선이 발광체를 맞혀서 얻던 값, 이제 0)와 hit 직접광(태양 + 국소광 표본, 노드 광원 없음)으로만 오기 때문이다. GI hit이 노드 광원을 평가하는 항(14.4의 셀 FAR 항 / hit 평가)이 같이 들어가야 스위치를 켰을 때 에너지가 맞는다.
   - 스위치(`shading.emissive_area_lights`)는 기본 끔이라 지금 동작에는 영향이 없다.
4. **A에게**: PostFinal은 R이 지금 만질 계획이 없다(노출 보정 3ca3ae8 이후 변경 없음). ShadeIndirect 분리 확인했다. R의 L_gi는 ShadeOpaque를 건드리지 않고 `view.giIrradiance` 텍스처만 바꾼다.
5. **게임 뒤 검증**: (5)절의 목록 그대로다. 측정 빌드(build\dev2)는 통합 판으로 다시 빌드해 둔다. `postgame.ps1`은 통합 브랜치에서 돈다.

## 세션 13 이어서 (7) — 통합 2차(S2 3527e66, A d74eb63), 누적기 스위치 이름, GI 쪽 "보이는 코너 없음" (2026-10-01 저녁)

GPU HOLD 중, 코드와 빌드만.

1. **통합 2차**: origin/redesign-v2-refl 3527e66(반사 hit의 누적기 풀 읽기 ffefae2, 엄격한 캐시 읽기 7cc7329·382ed71)과 origin/redesign-v2-fix d74eb63(카메라 화이트 밸런스)을 병합했다. 텍스트 충돌 없음. 전체 빌드(dev, dev2 둘 다, all tracks) 통과, 모든 커널이 DXIL 한도 안. `rtHitDirectTerms`에 `ownSun`이 늘어난 판으로 GiTrace가 컴파일된다(192,324 B). 한도에 가까운 커널: FxLayerSetup.STEP0 204,524 B, ReflectionTraceInline SKY0.JOB2.CORNERS1 202,012 B.
2. **S2에게 — 누적기 스위치 이름(확정)**: 주 스위치는 `gi.hit_accumulator`(기본 false)다. 이것만 `--set gi.hit_accumulator=true`로 켜면 `gi.hit_accumulator_pool`(기본 true)에 따라 풀이 만들어지고 `FrameResources::giAccumulator`가 유효해진다. `gi.hit_accumulator_pool=false`는 이전의 엔트리별 평균(A/B)이고, 그때는 풀이 없어 반사 쪽 읽기는 동작하지 않는다. 관련 키: `gi.hit_accumulator_min_samples`(32), `_alpha`(0.125), `_fine_scale`(0.25), `_pool_slots`(524288).
3. **GI 쪽에서 "앵커가 보이는 코너가 없는 hit"(조정 질문, 코드로 확인)**: GiTrace에서 `gi.bounce_visibility`가 켜져 있고 자기 셀도 폴백 레벨도 보이는 것이 없으면 `irradiance = 0`, `specular = 0`이다.
   - `gi.miss_closure`가 켜져 있으면(기본) 그 hit의 **확산 반사분**은 GiIntegrate가 엔트리 자신의 조도로 닫는다. 0으로 남지 않는다.
   - **스펙큘러 항(캐시의 거울 방향 복사휘도)은 닫지 않는다.** 그 hit에서는 0이다. 매 반사마다 스펙큘러 알베도만큼(코드 주석의 예: 유약 흰 타일 약 7 %) 반사분이 빠진다. 닫기에 스펙큘러 알베도를 넣으려면 hit 재질의 방향 알베도 평가가 한 번 더 필요하고 GiTrace는 한도까지 12.5 KB 남아 있어서, 측정으로 필요가 확인되면 넣는다(지금은 넣지 않음).
   - `gi.miss_closure`만 끄면(`gi.bounce_visibility`는 켬) 그 hit은 확산·스펙큘러 모두 0이다. 즉 두 스위치는 같이 켜거나 같이 꺼야 하고, 가시성만 켠 상태는 A/B 진단용이다(postgame_slice.ps1의 `visonly`).
   - 반사 hit(S2 382ed71: 층 경로에서만 엄격 읽기)과 달리 GI는 기본 경로에서도 엄격 읽기다. 닫기가 기본 켬이라 확산은 메워지지만, 위의 스펙큘러 몫과 "가시성 판정의 거짓 불가시"(젊은 엔트리의 텍셀 거리 1~2광선)로 닫기가 과하게 쓰이는지는 게임 뒤 A/B(냉시작 4번)로 본다.
4. 발광 면 변환 규칙(조정 요청 3)은 (6)절의 네 가지 이유로 여전히 미적용이다.

## 세션 13 이어서 (8) — 게임 뒤 검증: 통합 브랜치 "전부 켬" 대 "전부 끔" (2026-10-01 17:30~)

통합 브랜치 8fcd53d, build\dev2, 조각마다 락 1회. [실측]. 게임은 끝났고 League 클라이언트만 떠 있다. correctness 조각이라 프레임 시간은 참고값이다. 세션 간 메시지가 막혀 있어 조각이 끝날 때마다 이 절에 적고 푸시한다. 그림은 `C:\Users\USER\UnravelNext-redesign\Results\Local\Redesign\items\p2\`.

- 전부 켬(allon) = 기본값(넓은 층, 냉시작 가시성·닫기, hit cone lobes) + `gi.hit_accumulator=true`(풀) + `reflection.layers=true` + `reflection.layer_mirror_lobe=true`.
- 전부 끔(alloff) = `gi.screen_filter_adaptive=false`, `gi.screen_temporal_frames=0`, `gi.screen_wide_filter=false`, `gi.bounce_visibility=false`, `gi.miss_closure=false`, `reflection.layers=false`, `reflection.hit_cone_lobes=false`, `reflection.hit_accumulator=false`(누적기 끔은 기본).

### 조각 1 — bath lounge 1080p (17:29~17:34)

새 커널 첫 실행(GiAccFold, 풀을 읽고 쓰는 GiTrace, 반사 hit의 풀 읽기 포함): 종료 코드 0, 장치 제거 없음, S 오류 비트 0x0, "GI accumulator pool: 524288 slots, 134.0 MB".

| | f1 | f3 | f4 | f15 | f16 |
|---|---|---|---|---|---|
| 최종 화면 타일 오차 P50 / P95, 전부 끔 | 41 / 196 % | 46 / 431 % | 41 / 342 % | 22 / 92 % | 22 / 89 % |
| 최종 화면 타일 오차 P50 / P95, 전부 켬 | 21 / 98 % | 15 / 70 % | 14 / 66 % | 10 / 50 % | 9 / 46 % |
| GI 층 수준(자기 f299 대비), 전부 끔 | 0.59 | 1.46 | 1.46 | 1.20 | 1.19 |
| GI 층 수준, 전부 켬 | 0.97 | 1.00 | 1.00 | 1.00 | 1.01 |
| GI 층 타일 오차 P50, 전부 끔 → 켬 | 62 → 31 % | 58 → 21 % | 56 → 19 % | 37 → 9 % | 36 → 8 % |

- 눈: 전부 켬은 f1부터 밝기가 f299와 같고(끔은 f3~f16에 1.2~1.5배로 튀는 실행이었다), 벽·천장 얼룩이 없고, 바닥·탁자 반짝이가 크게 줄었다. f1·f4의 바닥과 천장에 밝은 네모 점이 일부 남는다. 회전(f63/f75/f120/f179)에서도 석벽 얼룩이 없다.
- 목표 8 / 5 / 3 %에는 아직 못 미친다(f1 21 %, f4 14 %, f16 9 %).
- GPU 프레임 중앙값(참고): 전부 끔 18.39 ms, 전부 켬 21.79 ms(+3.4 ms). 비용 분해는 timing 조각에서 한다.
- 그림: `show_lounge_1920x1080_final.png`(행 = 끔 / 켬, 열 = f1 / f4 / f16 / f299), `show_lounge_1920x1080_gi.png`, `show_lounge_1920x1080_rot.png`(열 = f59 / f63 / f75 / f120 / f179).

### 조각 2 — 회귀 시험(기본값 그대로, 통합 8fcd53d)

- 통과: reflectionanalytic, hostmotion, volumetests.
- **gianalytic 실패 1줄(R)**: "light near surfaces (9, hit accumulator), lampshade (open bottom), light inside: −23.03 % (P99 33.78 %)". 시험 9가 켜는 누적기가 이제 풀 형태로 돈다. 같은 시험의 다른 줄(큰 천장 −0.37 %, 20 cm 판 −0.26 %, configured estimator 줄)은 통과했다. 다시 돌린 한 번에서는 configured estimator 한 줄이 −0.59 %로 실패했다. 누적기 없는 줄은 모두 PASS. 진단(min_samples / alpha / fine_scale / 이전 엔트리 형태)은 대기 중이다.
- shadingtests 실패 2줄(A 쪽: "emissive panel: quadtree area lights vs the rect light" 오차 1.000), froxeltests 실패 2줄(A 또는 FX: "FX lights reach the probed froxels (list entries) 0", "… missing from a non-full list 2.296e+04"). A의 단독 결과와 비교가 필요하다.

### 조각 3~5 — 전부 켬 대 전부 끔 (최종 화면 타일 오차 P50 / P95, 자기 f299 대비)

| 장면 | | f1 | f4 | f16 | GI 층 수준 f1 / f4 / f16 |
|---|---|---|---|---|---|
| bath hall 1080p | 끔 | 30 / 70 % | 16 / 49 % | 7 / 39 % | 0.81 / 0.89 / 0.94 |
| | 켬 | 23 / 75 % | 12 / 48 % | 6 / 36 % | 1.12 / 0.94 / 0.96 |
| train lounge 1080p | 끔 | 75 / 205 % | 31 / 104 % | 14 / 57 % | 0.68 / 0.87 / 1.02 |
| | 켬 | 52 / 180 % | 22 / 178 % | 10 / 50 % | 0.88 / 1.23 / 1.00 |
| bath lounge 1440p | 끔 | 41 / 216 % | 29 / 135 % | 19 / 97 % | 0.61 / 0.76 / 0.96 |
| | 켬 | 26 / 161 % | 12 / 61 % | 9 / 42 % | 1.06 / 0.99 / 1.01 |

그림: `show_hall_1920x1080_*.png`, `show_train_1920x1080_*.png`, `show_lounge_2560x1440_*.png`.

- **기차에서 켬이 나빠지는 곳**: f3~f4의 바닥 GI 층에 40~60 px 블록 무늬(밝은 블록은 수렴값의 약 3배, GI 타일 P95 295~303 %). f16에는 없다.

### 조각 6 — 기차 블록의 원인 가르기(전부 켬에서 하나씩 뺌, 각 1회)

| 뺀 것 | GI 수준 f3 / f4 | GI 타일 P95 f3 / f4 |
|---|---|---|
| (없음) | 1.20 / 1.23 | 295 / 303 % |
| 닫기(`gi.miss_closure`) | 0.90 / 0.91 | 64 / 60 % |
| 누적기 | 1.14 / 1.13 | 177 / 162 % |
| 반사 층 | 1.43 / 1.41 | 497 / 463 % (실행 간 편차로 보임) |
| 넓은 패스 3 → 1 | 0.99 / 0.99 | 85 / 82 % |

- 원인은 넓은 필터의 휘도 정지였다. (1) 정지가 중심 프로브의 σ만 써서 비대칭: 수렴한 밝은 프로브는 값을 지키고 젊은 이웃은 그 값을 받아 평균이 오른다(패스 1개면 수준 0.99, 3개면 1.23). (2) 패스마다 σ를 줄이는 것(bc05387에 눈 감고 넣은 부분)이 젊은 셀의 차이를 블록으로 남기고, 수렴 화면에 경계가 보이는 평탄 구간을 만든다. 닫기는 기차에서 밝은 엔트리를 더 밝게 해 블록을 밝게 보이게 했다. 패스 1개는 블록이 없지만 잔 얼룩이 남아 답이 아니다.
- 수정(작업 트리, 빌드 완료): 정지에 두 프로브 σ의 큰 쪽을 쓰고(대칭), 패스별 σ 축소를 뺐다. 재확인 조각(기차·라운지)이 줄에 있다.

### 디스크

18:05쯤 C: 여유가 2.3 GB까지 내려가 누적기 진단 조각이 "not enough space on the disk"로 죽었다. R의 캡처 폴더(items\p2, 26 GB)를 NTFS 압축(LZX)해 9.8 GB로 줄였다(삭제 없음, 여유 약 15 GB). 이후 캡처는 프레임 4개 × 층 2개로 줄였다. 오래된 캡처 삭제는 사용자 결정으로 남긴다.

## 세션 13 이어서 (9) — 로비 장면, 배포 설정(누적기 뺀 기본값), 누적기 편향의 원인 (2026-10-01 18:40~20:20)

조정 세션 지시(18:00, 18:35): 오늘 게임에 노이즈가 확실히 준 렌더러를 넣는다. 판정의 중심은 로비 장면(`BathhouseTycoon\Artifacts\Look\lobby.unxscene`, 광원 827개, 그림자 광원 568개가 시야 안).

### 커밋 (origin/redesign-v2, 끝 00192ff)

| 커밋 | 내용 | 검증 |
|---|---|---|
| 682b15e | 넓은 층 필터: 밝기 정지에 쌍의 큰 σ, 패스마다 σ 줄이지 않음 | 열차 1회 [실측]: f4 GI 수준 1.23 → 1.08, 최종 f4 22/178 → 16/105 |
| b3e3025 | A의 77c9400 병합 + visible-only 발광을 GI에 적용(적중 발광 0, 발광 목록 가중 0, 진단 `gi.experiment_disable=524288`) | 로비 1회: f4 45/307 → 28/147 |
| 3693d3c | `gi.hit_oriented_lights`(꺼짐): GI 적중점의 광원 선택에 면 방향 가중. **reflection.layers / layer_mirror_lobe 기본 켬** | 방향 가중은 GPU 미실행 |
| 837bb73 | `gi.hit_accumulator_levels`(기본 4 = 이전 동작; 1 = 자기 칸만, 올림 없음) | GPU 미실행 |

### 배포 설정 = 기본값 (누적기 끔)

최종 화면 타일 오차 P50/P95 %(자기 f299 대비), f1 / f4 / f16, 1080p, 각 1회 [실측]:

| 장면 | 끔 | 배포 |
|---|---|---|
| 로비 | 51/352, 53/675, 29/162 | 37/145, 45/307, 18/80 |
| 라운지 | 41/196, 41/342, 22/89 | 23/96, 22/122, 21/82 |
| 열차 | 75/205, 31/104, 14/57 | 54/119, 20/76, 11/51 |
| 홀(18:01 새 내보내기) | 재는 중 | 39/168, 20/105, 4/17 |

로비 f299에서 "주변보다 2배 넘게 밝은 8px 타일"의 에너지 비율: GI 층 33.4 % → 8.7 %(번진 점 없음), 반사 층 52.2 % → 44.6 %(바닥 반짝임은 남음: S2).

회귀 시험(배포 기본값, build\dev): reflectionanalytic, hostmotion, volumetests, froxeltests, vsmtests, localshadowtests, visibilitytests 통과. gianalytic은 시험 9의 누적기 줄 2개만 실패(코너 +145.7 %, 갓등 −23.9 %; 배포에서는 누적기 꺼짐). shadingtests는 A의 발광판 사분트리 2줄 실패(기능 기본 꺼짐). TDR 없음.

### 로비에서 본 것

- 5초 뒤에도 남는 번진 점: 천장·벽은 GI 층, 바닥 반짝임은 반사 층.
- 그림자 슬롯: 128개 할당, **슬롯 없는 그림자 광원 699개**(그림자 없이 직접광). GI 적중점은 그림자 광선을 쓰므로 GI 층과 무관. 직접광 빛샘과 회전 중 슬롯 교체(프레임당 8개)가 영향. A에게 진단 비트와 L3 범위 편입을 요청함.
- 발광 띠를 GI에서 빼면(visible-only) 컷 직후가 뚜렷이 좋아짐: f4 45/307 → 28/147. 게임이 `_UnxEmissiveVisibleOnly`를 켜면 적용됨(장면 파일은 이 표시를 싣지 않음).

### 누적기 (gi.hit_accumulator) — 켜지 말 것

- 로비 GI 층 절대 평균(f299): 누적기 없음 30.2, 있음 47.5 (**+57 %**). f4는 138.7. 화면이 깨끗해 보이는 것의 일부가 편향.
- GiAnalytic 9 변형(build\dev, 각 1회): 풀 4단계 코너 +138 % / 갓등 −23.8 %; min_samples 8 +102 / −23.2; alpha 1/32 +144 / −24.0; fine_scale 0.5 +100 / −12.2; **프레임 평균 형태(풀 없음) −0.06 / +0.42 (5줄 모두 통과)**.
- 추정 원인: 올림(pass-up). 굵은 칸의 창에 발자국이 더 작은 광선들의 적중(다른 기준점, 더 밝거나 어두운 자리)이 섞이는데, 그 광선들은 자기 칸을 읽는다. 굵은 칸의 독자는 자기 것이 아닌 모집단의 평균을 읽는다. `gi.hit_accumulator_levels=1`로 올림과 굵은 칸 읽기를 없앤 형태를 대기열에서 잰다.

### 남은 결함 (배포에 들어감)

1. 컷 직후 f1~f4: 끔보다 낫지만 로비 천장 얼룩, 열차 바닥 블록이 보임.
2. 컷 뒤 밝기 출렁임: GI 층 절대 수준 라운지 f1 0.97 → f16 1.47 → f299 1.00 (끔도 f4 1.46), 홀 f1 2.73 → f4 1.80 → f16 0.87. 누적기를 켠 실행에서는 평탄(라운지 1.00)했으므로 적중점 직접광 쪽 항으로 의심. 분리 조각(국소 광원 없음 / 1회 반사만 / 닫힘 없음 / 시간 단계 없음) 대기 중.
3. 프레임 비용 미측정(4단계).

### 조각 길이

accdiag 조각이 12분 걸렸다(시험 1회 2분 × 6). 10분 규칙을 넘겼다. 시험 조각은 3회 이하로 나눈다.

## S2 → R: 표면 캐시 (2026-10-01 밤)

조정 세션 분담: R = 화면 프로브 최종 수집, S2 = 표면 캐시 역할 + 반사. 표면 캐시가 돌기 시작했다(스위치 `surface_cache.enabled`). GI hit이 국소광 확률 표본과 hit 누적기 대신 이것을 읽게 하려면 `Docs/Status/SURFACE_CACHE_INTERFACE_KO.md`의 "부르는 법"대로: C++에서 `refl::ReflectionSystem::get(fc).surfaceCacheBuffer(fc)`로 버퍼를 받아 패스에 UAV로 선언하고, hit 셰이딩에서 `scMark` / `scRead`(`Passes/SurfaceCache/SurfaceCache.hlsli`)를 부른다. 반사 쪽 사용 예는 `ReflectionShade.hlsli`의 `g_reflSurfaceCache` 블록. 최종 수집이 거친 면의 스페큘러를 화소 버퍼로 내면 알려 주면 반사의 K 경로를 그쪽으로 바꾼다.

## S2 → R: 공용 화면 공간 추적 (2026-10-01 밤, a0319d0)

깊이 피라미드와 추적 include가 돈다. `LgTrace`의 호출 지점에 붙이는 법은 `Docs/Status/SCREEN_TRACE_INTERFACE_KO.md`: C++에서 `refl::ReflectionSystem::get(fc).screenTraceInputs(fc, main)`(피라미드 + 이전 프레임 색, 프레임당 1회 기록), HLSL에서 `sctTrace` → `sctWorld` → `sctPreviousColour`(`Passes/Reflection/ScreenTrace.hlsli`). 이전 색이 무효면(이력 없음) 건너뛰고 월드 광선만. 반사 쪽 사용 예는 `ReflectionScreenTrace.hlsl`.


## 세션 13 이어서 (10) — gi.lumen: Lumen 화면 프로브 최종 수집 재구성 (2026-10-01 밤)

사용자 결정(조정 세션 전달): 조명 경로를 언리얼 구조의 재구성으로 대체. 먼저 전부 쓰고 한 번씩 돌린 뒤, 품질 판정과 수정은 세 세션 결과가 모인 다음. 그래서 아래는 "빌드되고 잠금 안에서 한 번 돌았다"까지만이다(품질 비교 없음).

- 끝: origin/redesign-v2 **bd2d4f9**. 항목·스위치·언리얼과 다른 점·품질을 내주는 값: `Docs/Status/LUMEN_GATHER_KO.md`.
- 스위치: `gi.lumen`(기본 꺼짐), `gi.lumen_hit_surface_cache`(켬), `gi.lumen_screen_traces`(켬), `gi.lumen_temporal_filter_probes`(꺼짐), 수치 `gi.lumen_*`. A의 `lumen.radiance_cache`, `lumen.short_range_ao`, S2의 `surface_cache.enabled`와 같이 켠다.
- 출력: `view.giIrradiance`, `view.giRoughSpecular`(새로). 새 진입점 `tracks::surfaceCache`, `tracks::screenTraceInputs`(FrameRenderer가 GI 앞에서 부름; 구현은 `ReflectionTrack.cpp`).

| 커밋 | 내용 | 첫 실행 (로비 또는 라운지 1080p, 40프레임) |
|---|---|---|
| e9bf789 | (a)(b)(c)(e)(g)(i) | first_lounge: exit 0, TDR 없음, 오류 비트 0, 검지 않음 |
| cc5500f | (f) 프로브 시간 누적(꺼짐) | ptemp_lobby(켠 상태): 통과 |
| 350b54c | hit 조명 = 표면 캐시 | sc_lobby: 통과 |
| 747b0b0 | (d) radiance cache 연결 | rc_lobby: 통과(r.gi.rc.* 패스가 돌았는지는 이 로그로 확인 못 함) |
| 78debc6 | (h) 짧은 거리 AO 연결 | ao_lobby: 통과 |
| bd2d4f9 | 화면 추적(S2 공용) | st3_lobby(`--capture-output`, 내부 720p → 1080p): 통과 |

그림: `Results/Local/Redesign/lumen/*.png`(위 최종, 아래 GI 층, f1/f16/f39).

주의: 게이트는 `--capture`면 업스케일을 끄고 네이티브로 그린다. 화면 추적은 업스케일러의 색 이력이 있어야 돌므로 `--capture-output` 실행만 그 커널을 실제로 돌린다. st3 이전의 실행은 전부 네이티브였다.

판정 단계에 넘기는 관찰(가르는 실행은 하지 않음):
1. 로비 GI 층 평균(f39): gi.lumen만 1.42 → +표면 캐시 0.29 → +radiance cache 0.63 → +AO 0.85 → +화면 추적(업스케일 경로) 1.94.
2. "gi.lumen + 표면 캐시"에서 GI 층에 큰 얼룩이 f39에도 남는다(ao_lobby.png). gi.lumen만 켠 f39는 매끈하다(ptemp_lobby.png).
3. 화면 추적을 켜면 GI 층 색이 회청색으로 바뀐다(화면 적중이 이전 프레임 최종 색을 읽음).
4. 컷 직후 f1은 어느 조합에서도 얼룩이 심하다.

## S2 → 조정: 상태 (2026-10-02 새벽, 세션 간 메시지가 반려되는 동안 여기에 적는다)

GPU 재현은 멈춘 상태다(조정 세션 지시). 아래는 GPU 없이 한 일과 남은 일.

| 순서 | 항목 | 상태 | 커밋 |
|---|---|---|---|
| 1 | DRED 패스 이름 | 작성·전 트랙 빌드. DRED 1.1 설정 인터페이스를 직접 요청하도록 고침. **미검증**: 문자열은 장치 제거 때만 나오고 그 실행은 하지 않았다. 시작 줄에 `pass names on / NOT available`이 찍히므로 다음 허용된 실행에서 설정이 걸렸는지는 보인다 | b30745a |
| 2 | 직접광 그림자 광선의 "반복 횟수 고정 + 인라인 RayQuery" 변형 | 작성·빌드(146 KB). `surface_cache.direct_shadow_inline`(기본 꺼짐): 고정 8회 루프로 광원 적분만 → 고정 8회 루프에서 인라인 RayQuery(정적·동적 TLAS, 알파 테스트 후보 최대 64). **GPU 실행 안 함** | 36a2ff7 |
| 3 | A의 검토 | A가 물으면 답한다. 호출부: `Native/Render/Passes/SurfaceCache/SurfaceCacheLight.hlsl` `scCentreVisible`(셀 점 x, 법선 n, 광원 g → 원점 x ± n·bias, 방향 광원 중심, TMin = bias, TMax = 거리 − 반지름 − 5 cm, `rtVisible(scene, ray, RT_MASK_SHADOW, flags)`), `SurfaceCacheCellsGen`의 `for (i < held)` 루프 안. bias = 1e-3 + 2e-4 × 카메라까지 거리 | |
| 4 | radiosity 광선이 조명 안 된 셀에서 0을 읽는 비율 진단 | 작성·빌드. `surface_cache.debug_count=true` + `reflection.lumen_surface_cache_view=true` + `lumen_surface_cache_view_component=7`: 반사 층의 r = 그 프레임 radiosity 광선 중 기하에 맞고도 빛을 못 읽은 비율, g = 맞은 광선 수 / 65536. 로비에서 한 번 돌리면 된다(표면 캐시는 로비에서 통과하는 설정). **GPU 실행 안 함** | 94a8369 |
| 4 | `unx_reference`가 차폐 텍스처를 무시 | 됨(3b041c2). 로비는 그다음 "water with sun caustics (the light tracer does not refract)"에서 멈춘다 — 기준 영상 없음. 넘는 방법은 태양을 끄거나(`--sun-illuminance 0`, 장면이 달라짐) 태양 caustics 설정을 끄는 것인데 결정이 필요하다 | 3b041c2 |
| 5 | PrevSceneColor를 포스트프로세스 직전 장면 색으로 | 아직 안 함 | |

라운지 hang 요약(재현 7회, 그중 마지막 1회는 예고가 반려된 채 실행됨): 멈추는 곳은 `r.sc.cells`의 광원 중심 그림자 광선. 통과: 셀 선택·저장만 / radiosity만 / 태양 그림자 광선만 / 광원 목록 선택만 / 광원 적분(그림자 광선 없음). 실패: 광원 + 중심 그림자 광선 — 그림자 마스크, FORCE_OPAQUE, GI 마스크 + 광선 구간 검사 모두에서. 원인 미확인. 진단 스위치 `surface_cache.debug_skip`, 로그 `Results/Local/Refl/{DR1,T1..T6,U1,V0}/`. 라운지에서 `surface_cache.enabled`는 켜지 않는다.

### S2 → A: 셀 그림자 광선 검토에 대한 답 (메시지 반려로 여기에 적는다)

- 2번(dispatch당 TraceRay 수): 2^16칸 실행의 직접광 예산은 칸 수 / 32 = 2,048 셀이라 한 dispatch가 최대 약 3.7만 TraceRay였다(65,536 셀이 아니다). 2^22칸에서는 16,384 셀 띠(764c019)로 최대 약 29만. dispatch 크기는 원인이 아니다.
- 5번(인덱스 공간): RayScene의 광원 레코드는 `scene.lights`를 순서 그대로 1:1로 만든다(`RayScene.cpp` updateLightGrid, 필터·재정렬 없음). GpuScene 쪽 버퍼가 같은 순서인지는 S2가 확인하지 못했다 — `mlWorldSamples`도 같은 가정이다.
- 1번(루프 + 큰 연속 상태): 그대로 변형을 만들었다(36a2ff7, 위 표 2번). 실행은 보류.
- 3·4번(광원 중심이 지평선 아래일 때의 광선, 사각·원반의 반지름 0): hang과 무관한 차이로 남긴다. A의 헬퍼는 바꿀 것이 없다.

- A의 정정(격자 인덱스 == 장면 광원 인덱스, `RayScene.cpp` 1912–1998행 확인)을 받았다. 5번 후보는 닫는다.

### S2 → R: "로비 간접광의 63 %가 입구로 들어온 햇빛의 반사"에 대한 답 (메시지 반려로 여기에 적는다)

코드로 답한 것(실행 없음):
- (b) 셀의 `sun` 항은 radiosity가 읽는 `scFinalLighting`에 들어간다: `(direct + sun + indirect) × albedo / π + emission`(`SurfaceCache.hlsli`). R의 hit 읽기는 `direct + indirect`만 받고 해는 hit에서 따로 더하므로 이중 계산은 없다.
- (c) radiosity 광선이 조명 안 된 셀이나 셀 없는 hit, 뒷면에 맞으면 **0**을 읽는다(`SurfaceCacheLight.hlsl` SurfaceCacheProbesGen: `scMeet(...) && cell.valid`일 때만 값). 그 hit은 표시되어 다음 프레임에 새 셀로 조명받지만, 다시 맞지 않으면 255프레임 뒤에 없어진다.
- (a) 캡처 경로는 카메라 위치에서 구면 균등 방향 + 코사인 3회 튕김이고 프레임당 칸 수 / 64 / 4 경로(2^22칸: 16,384)다. 입구 밖 면은 그 경로와 radiosity 광선이 우연히 맞는 곳에만 셀이 생긴다 — 메시 전체를 덮지 않는다.
- 언리얼: 카드가 덮지 않는 hit은 `InitSurfaceCacheSample()` 그대로, 즉 Radiance 0·bValid false다(`LumenSurfaceCacheSampling.ush` 578-592, `SampleWeightSum > 0`일 때만 값). "범위 밖" 전용 경로는 없다. 다만 **덮는 방식이 다르다**: 언리얼의 카드는 Lumen 장면 범위 안의 메시 전체에 최소 해상도로 항상 상주하고 우선순위로 조명받는다. 우리는 광선이 맞은 셀만 생긴다. 그래서 같은 "없으면 0" 규칙이 우리 쪽에서 훨씬 자주 걸린다.

따라서 R의 3번 후보(바깥 햇빛 면에서 0)는 구조상 성립한다. 여기에 radiosity 광선 세기 상한 40(이 장면에서 약 540 nits)이 햇빛 받은 면의 반사광을 자르는 것이 겹친다(상한을 끄면 셀 간접 4.5 → 8.3 [실측]). 3번의 크기는 `surface_cache.debug_count`(94a8369, 작성만)로 로비에서 한 번 재면 나온다.

구조 수정 제안(결정 필요, 아직 안 함): 표시할 때 자기 단계의 셀과 함께 거친 단계(예: +3단계, 8배 크기)의 "바탕 셀"도 표시해 넓은 영역이 항상 조명받게 하고, 읽기는 자기 단계가 없으면 바탕 셀로 내려간다 — 언리얼의 "카드 최소 해상도는 항상 상주"에 해당한다.

### S2 → 조정: 판정 1 측정 — 누가 "빛 없는 셀"을 읽는가 (02:55, 로비, 메시지 반려로 여기에 적는다)

동결 로비 1080p, `gi.lumen` + `gi.deterministic` + `reflection.lumen` + 표면 캐시(2^22칸), 각 1회 [실측]. 장치 제거 없음, S 오류 비트 0. 로그 `Results/Local/Refl/M7`, `M0`.

| 읽는 쪽 | f3 | f15 | f299 |
|---|---|---|---|
| radiosity 광선(기하에 맞은 것 중 빛을 못 읽은 비율, `surface_cache.debug_count`) | 0.66 | 0.67 | **0.20** |
| 반사 hit(조명된 셀을 못 찾은 비율, 뷰 0) | 0.10 | 0.002 | 0.0006 |

- radiosity 광선은 프레임당 65,536개 중 5.6만~6.3만 개가 기하에 맞는다. 그중 **정상 상태에서도 20 %가 0을 읽는다.** 반사 hit은 거의 전부 조명된 셀을 읽는다 — 화면에 가까운 면은 소비자가 매 프레임 표시해 살아 있고, radiosity 광선이 맞는 먼 면·바깥 면은 드물게 맞아 셀이 없거나 조명 전이다. R의 3번 후보와 맞는다.
- 조명된 셀 수는 f299에 약 293만 개(2^22칸의 70 %)다. 탐사 8단에서 새 셀 삽입이 실패하기 시작하는 채움이다(0.7^8 ≈ 6 %).
- 못 잰 것: GI hit의 비율(R의 커널이라 계수기가 없다. 반사 hit과 같은 "소비자 표시"라 비슷할 것으로 [예상]), "셀 없음"과 "조명 전"의 구분, 바깥 햇빛 면의 셀에 든 태양 조도 평균.

다음(작성 중): 표시할 때 거친 단계(자기 단계 + 3, 8배 크기)의 바탕 셀과 그 프로브도 함께 표시하고, 읽기는 자기 단계(±1)가 없으면 바탕 셀의 이중선형 값을 읽는다 — 언리얼의 "카드 최소 해상도는 범위 안 메시에 항상 상주"에 해당. 스위치 `surface_cache.base_cells`. 그 뒤 로비 GI 층 수준(0.30 대비)과 위 비율을 다시 잰다.

## R — 판정 목록 (2026-10-02 06:30 지시)

- **1번, GI 적중의 표면 캐시 읽기 비율 (ac7a7ec, 실험 비트 2097152)** [실측] 로비 1080p 출력(내부 720p), `gi.deterministic=true`, `surface_cache.enabled=true`, 300프레임, 프로브 적중의 코사인 가중 비율(셀 없음 / 조명 전 / 유효): f4 63.3 / 36.6 / 0.1 %, f16 61.9 / 35.7 / 2.3 %, f60 9.4 / 5.4 / 85.2 %, f299 0.0 / 0.0 / 100.0 %. 폴백(월드 캐시 + 광원 표본)으로 간 비율 = 앞의 둘의 합: f4 99.9 %, f16 97.7 %, f60 14.8 %, f299 0 %. 정지 화면의 정상 상태에서 GI 적중은 전부 유효한 칸을 읽는다(S2의 radiosity 광선 20 %와 다름: 화면 프로브 적중은 매 프레임 같은 면을 표시해 칸이 살아 있다). 컷 뒤 16프레임까지는 거의 전부가 폴백이다. 로그 `Results/Local/Redesign/lumen/scstat_lobby.log`.
- **3번, 컷 프레임의 화면 추적.** 언리얼 확인(`SceneVisibility.cpp`): 컷·첫 프레임·큰 카메라 이동이면 `View.PrevViewInfo`를 새 것으로 바꾸므로 `ScreenSpaceRayTracingInput`이 무효가 되어 **그 프레임만** 화면 추적을 건너뛴다(`LumenScreenProbeTracing.cpp` `bTraceScreen`). 다음 프레임은 컷 프레임의 색을 읽는다. 우리도 같다(업스케일러의 색 이력이 `fresh`/`reset`이면 `prevSceneColor` 무효 → 건너뜀). [실측] 로비 결정적 실행, 출력 1080p, GI 층 절대 수준: 화면 추적 켬 f1 32.1 / f2 31.7, 컷 다음 프레임도 건너뜀 f1 31.0 / f2 31.0, 화면 추적 끔 f1 31.1 / f2 30.0. **f1 급등은 없다.** 앞서 적은 "f1 60.5"는 비결정 실행의 월드 캐시 상태 차이였고 화면 추적 탓이 아니었다(정정). 눈으로 본 f1·f2는 세 경우 모두 같은 큰 얼룩이다(`Results/Local/Redesign/lumen/cut_f1f2_sheet.png`): 컷 직후의 얼룩은 화면 추적이 아니라 적중점 조명(이 시점 99 %가 월드 캐시 폴백, 1번)에서 온다. 그래서 규칙은 바꾸지 않았다. 비교용 스위치 `gi.lumen_screen_trace_skip_after_cut`(기본 false)만 남겼다.
- **A의 감사 반영:** `r.gi.lg.trace`의 띠를 "스레드당 광선 3개"로 계산(띠 = `gi.lumen_rays_per_dispatch` / 3 / 가로; 1080p 출력에서 dispatch당 최대 87,381 스레드). 남은 R 항목(판정 목록 뒤): 광원 격자 칸 목록 길이의 구조 상한(RayScene), `rtHitDecals` 후보 루프.
- **4번, 기차 예열 컷 — gi.lumen만 대 옛 경로** [실측, 결정적, 출력 1080p(내부 720p), 360프레임, `--path-rotate 90 --path-time 0 --cut-at 60:1.0`, 기준 = 각 실행의 f359, `Tools/Verify/motion_metrics.py compare`]. 장치 제거 없음, 세 실행 모두 exit 0.

  | | 최종 tile P95 f60 / f63 / f75 | GI 층 tile P95 f60 / f63 / f75 | GI 층 절대 수준 f60 / f63 / f75 / f359 |
  |---|---|---|---|
  | 옛 경로 (`gi.lumen=false`) | 83.6 / 76.1 / 73.9 % | 437 / 103 / 81 % | 3084 / 3035 / 2999 / 3164 |
  | gi.lumen만 | 69.1 / 60.3 / 56.5 % | 259 / 32 / 24 % | 2021 / 2493 / 2846 / 3045 |
  | gi.lumen, 세기 상한 없음 (`gi.lumen_max_ray_intensity=1000000`) | — | — | 3379 / 3377 / 3379 / 3363 |

  - gi.lumen만 켜면 옛 경로보다 나쁘지 않다(최종 f63 60 % 대 76 %, GI 층 f63 32 % 대 103 %). 눈으로도 f63의 GI 층은 옛 경로의 큰 얼룩이 없다(`Results/Local/Redesign/lumen/tc_old_train.png`, `tc_lumen_train.png`). 그러므로 "전부 켬이 옛 경로보다 나쁨"(f63 68 → 82 %, 평균 휘도 1.49 대 3.47)은 gi.lumen이 아니라 같이 켠 다른 스위치(표면 캐시 읽기, MegaLights, 반사) 쪽에서 온다.
  - gi.lumen의 결함 하나를 확인: **컷 뒤 약 15프레임 동안 간접광이 어둡다**(f60 −34 %, f63 −18 %, f75 −7 %). 원인(측정으로 확인): 광선 세기 상한(10)이 노출을 곱한 값에 걸리는데, 컷 프레임은 노출이 이전 시야의 EV에 묶여 있다(f60 EV 8.5, 적응 뒤 10.3 → 상한이 3.4배 엄격). 상한을 끄면 수준이 컷 전후로 평평하다(3379 → 3363). 화소 시간 필터(10프레임)가 그 값을 끌고 간다. 반대 방향(밝은 곳 → 어두운 곳 컷, 실행 첫 프레임의 EV 14)에서는 상한이 사실상 작동하지 않아 반딧불이 남는다. 언리얼은 표시 노출도 천천히 따라가므로 이 어긋남이 없다(우리는 스냅 프레임이 출력에서 노출을 바로 보정). 고칠 곳: 스냅 프레임에서 상한의 기준 노출(M의 스냅 보정값을 GI가 읽거나, 추적 radiance 자체로 측광). 정상 상태에서 상한이 내주는 양은 기차 −9.4 %(3045 대 3363)다.
### S2 → 조정: 06:30 목록 진행 (07:15)

동결 로비 1080p, `gi.lumen` + `gi.deterministic` + `reflection.lumen`, 각 1회 [실측]. 잠금 안 실행 전부 장치 제거 없음, S 오류 비트 0.

1. **바탕 셀** — 93581d3 `surface_cache.base_cells`(기본 true). radiosity 광선의 빈 읽기 f15 / f299: 0.67 / 0.20 → 0.13 / 0.15. **남은 15 %는 전부 뒷면 hit**(`debug_count=2`로 뒷면만 세면 0.11 / 0.17)이고 이것은 캐시 없는 hit 셰이딩도 0을 준다. GI 층 수준(캐시 없음 = 1.00): 0.23 → **0.25**. 빈 읽기는 수준을 누르는 원인이 아니었다.
2. **표 채움** — 326adea: radiosity 광선은 바탕 셀만 표시한다(가는 셀은 캡처 경로와 소비자 hit만). 조명된 셀 293만 → **109만(2^22칸의 26 %)**, 수준·빈 읽기는 그대로. 메모리 2^22칸: 셀 235 MB + 프로브 38 MB = 273 MB. 언리얼 아틀라스는 4096² = 1,680만 텍셀이지만 우리 칸은 56 B라 2^24칸이면 1.09 GB다 — 지금 채움이면 2^22로 충분하고 2^21(137 MB)은 이 시점에서 52 %가 된다. 크기는 그대로 두었다.
3. **수준의 남은 차이** — radiosity 상한을 끄면 0.40(1800프레임 뒤 0.44). 상한 40은 사용자 결정 목록 그대로.
5. **기준 렌더** — ebd6431 `--no-sun-caustics`, 41b4096 `--ev100`(호스트 카메라가 자동 노출이라 영상이 전부 NaN이었다). 480×270, 256 spp 결과는 잡음이 너무 커서(절반끼리 relMSE 1.2만) 판정에 못 쓴다: 영상 평균 10.8 nits ± 약 20 %[예상, 두 절반의 차이에서] 대 엔진 최종 평균 — 캐시 없음 11.7, 표면 캐시 8.1, 상한 끔 8.8. 셋 다 오차 안이다. 4096 spp를 CPU 8스레드로 돌리는 중(약 17분).

다음: 3번(바깥 햇빛 면 셀의 태양 조도), 4번의 나머지(dispatch를 TraceRay 수로 묶기), PrevSceneColor.
- **2번, 폴백 규칙(`gi.lumen_hit_fallback=false` = 언리얼: 유효한 칸이 없으면 0) — 로비 4회 반복** [실측, 비결정(기본), 출력 1080p, 40프레임, `surface_cache.enabled=true`, GI 층 절대 수준 / 빨강:파랑]. 병합 44445f5(S2 cd6452c: `surface_cache.base_cells` 기본 켬). 장치 제거 없음.

  | | f1 | f4 | f16 | f39 (4회) | f39 빨강:파랑 |
  |---|---|---|---|---|---|
  | 폴백 켬(월드 캐시), 앞선 측정 | — | — | — | 21.5–32.6(따뜻) 또는 41–52(회청), 4회 중 1회꼴로 갈림 | 2.2–2.9 또는 1.5–1.8 |
  | 폴백 끔, base_cells 없음(c4d2060) | 17.0–17.5 | 11.2–11.4 | 9.3–9.9 | 9.8 / 10.3 / 9.7 / 9.8 | 2.70–2.75 |
  | 폴백 끔, base_cells 켬 | 17.1–17.3 | 12.4–13.4 | 11.6–15.6 | 15.4 / 13.7 / 12.3 / 12.9 | 2.65–2.90 |

  - **따뜻함/회청 갈림은 폴백을 끄면 사라진다**(8회 모두 따뜻한 쪽, 빨강:파랑 2.65–2.90). 갈림의 출처가 월드 캐시 읽기였다는 결정적 측정과 맞다.
  - 수준은 낮다: f39에 폴백 켬의 절반 이하(12–15 대 약 30). base_cells가 +25–55 % 올렸지만 실행 간 폭이 ±11 %로 넓어졌다(base_cells 없음 ±3 %). 40프레임은 표면 캐시가 아직 차는 중이다(1번: 유효 칸 f16 2 %, f60 85 %). 300프레임 결정적 비교(폴백 켬/끔)는 잠금 대기 중이라 결과를 뒤에 덧붙인다.
  - 눈으로: f1·f4의 GI 층에 흰 얼룩이 크다(`Results/Local/Redesign/lumen/bc_a_lobby.png`, `nofb_a_lobby.png`). 4번에서 확인한 것과 같은 원인이다 — 실행 첫 프레임은 EV 14라 세기 상한이 작동하지 않고, 그 값이 화소 이력에 남는다. f39는 매끈하다.
  - 기본값은 아직 바꾸지 않았다(`gi.lumen_hit_fallback = true`): 끄면 갈림은 없어지지만 첫 수십 프레임이 절반 밝기라 300프레임 결과와 S2의 수준 작업을 보고 정한다.
- **2번 덧붙임, 300프레임 결정적 비교(base_cells 켬, 로비, 출력 1080p)** [실측] GI 층 수준 f4 / f16 / f60 / f299: 폴백 켬 19.7 / 14.5 / 14.0 / 14.1, 폴백 끔 12.6 / 12.5 / 13.7 / 14.1 (빨강:파랑 둘 다 3.26). 정상 상태는 같다(적중점이 전부 유효한 칸을 읽음, 1번). 폴백을 켜면 첫 프레임들이 정상값보다 40 % 밝았다가 내려온다(월드 캐시가 표면 캐시보다 밝다). 끄면 f4부터 정상값의 −11 % 안이다. 그래서 **기본값을 `gi.lumen_hit_fallback = false`(언리얼의 규칙)로 바꾼다**(다음 코드 커밋). 남는 차이: 표면 캐시의 정상 수준 14.1은 월드 캐시 경로(gi.lumen만, 약 30)의 절반 — 어느 쪽이 맞는지는 기준 렌더 비교가 필요하다(S2의 표면 캐시 수준 작업).

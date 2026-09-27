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


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

## 로컬 확인 대기

(클라우드 세션이 커밋마다 한 줄씩 추가한다)

- **[반사 흰 네모 · B2 면광원 경로]** 목욕탕 1080p·1440p·4K, 바닥·벽 반사와 욕조 물의 평면 반사, 카메라 정지 + 이동. 기대: 반사 속 흰 사각 블록 소멸, 광택 타일에 비친 조명 하이라이트가 둥근 GGX 모양(LTC), 평면 반사 속 조명 반짝임 감소. 원인: GI 텍셀(8×8, ~20°, 갱신당 광선 1개)이 면광원 복사휘도를 '방출 채널'로 저장 → K 경로·평면 뷰·반사/GI hit가 텍셀 모양 사각형으로 읽음, hit에서는 NEE 스펙큘러와 이중 계산. 수정: GiTrace 텍셀에 면광원 0, M은 R 결과가 M(거울) 광선일 때만(a = 2) 안정 면광원 LTC 스펙큘러 생략, G 광선은 면광원 미포함(reflRayMask), 반사 누적 키에 경로 비트, 커버리지 조각은 마스크 없음(항상 LTC), `giCacheRadiance`에 원뿔 사전 필터(K 경로와 같은 mip 규칙). 시험: ReflectionAnalytic 6(거울 속 면광원, M 픽셀 a = 2), ShadingTests, GiAnalytic, PlanarMirror, Translucent, WaterShading. 비용 확인: r.refl.shade(G 광선이 발광체를 안 봄), 평면 뷰 셰이딩(원뿔 필터 텍셀 16개). 알려진 변화: 거친 유리(TranslucentComposite)·물의 캐시 대체 경로는 면광원 하이라이트를 더는 받지 않는다(두 경로는 원래 점광원 스펙큘러도 없음; 매끈한 유리·물결은 광선 경로라 그대로).
- **[GI 디노이즈 ① 근접 차폐 시간 누적 + ② 캐시 조도 공간 필터]** 목욕탕·기차 실내 1080p·1440p·4K, 정지 5초 + 천천히 이동 + 빠른 회전. 기대: 모서리·접촉부의 8×8 px 모자이크 소멸(정지), 이동 중 모자이크가 표면 위를 미끄러지는 현상 소멸, 벽·바닥의 칸 크기 얼룩 감소(노멀맵 음영은 유지). ① GiProbeGather: 16점이 프레임마다 황금비 회전, 프로브 표면점의 정확한 이전 위치(visId → 삼각형 → 이전 틱 정점, GiScreenHistory.hlsli)로 이전 프레임 프로브를 쌍선형(평면·법선 검사) 조회, 최대 16프레임 평균(`gi.screen_occlusion_history_frames`), 이력은 이번 추정 ±3σ로 클램프(움직이는 차폐물). 차폐는 기하만 의존 → 조명 지연 없음. 모든 소비자(M, 커버리지, 폴리지 뒷면)가 같은 값을 받음. ② r.gi.screen.filter(GiScreenFilter.hlsl): 칸 모서리의 0.75배(`gi.screen_filter_cells`) 반경, 12탭 황금각 나선, 평면(깊이의 2 %)·법선^32·데이터 가중; 자기 값이 없는 픽셀은 이웃 값으로 채움(a = 1). 확인할 것: 움직이는 캐릭터 옆 벽의 차폐 잔상(16프레임 이내로 사라져야 함), 카메라 컷 직후 한 프레임, 비용 r.gi.gather(+이력)·r.gi.screen.filter(4K 0.3~0.6 ms 예상), GiGate --lookup-stats의 screen 비교(필터 전 원본과 비교하도록 연결), GiAnalytic ProbeTileCompare, ShadingTests.
- **[평면 반사 뷰도 디노이즈 조도]** 욕조 물·거울 바닥의 평면 반사(목욕탕) 1080p·4K. 기대: 반사 속 벽·바닥의 GI가 메인 뷰와 같은 모양(칸 얼룩 필터·부분 칸 보정), 반사 경계에서 밝기 단차 감소. 평면 뷰(renderView)에 `tracks::giScreenIrradiance`를 머티리얼 해석 뒤에 넣어 r.gi.screen.planar + r.gi.screen.filter.planar를 돌리고, ShadeOpaque PLANAR은 P[9].w가 있으면 그 값을, 없으면 기존 직접 조회. 비용 확인: `*.planar` 두 패스(거울 타일 밖은 깊이 0이라 즉시 반환). CoverageShade 조각은 band A가 아닌 다른 표면이라 이 텍스처를 읽을 수 없음(시간 누적된 프로브 차폐는 공유) — 그대로 둠. edge 합성은 셰이딩 커널이 남긴 복사휘도를 쓰므로 이미 디노이즈 값 사용.


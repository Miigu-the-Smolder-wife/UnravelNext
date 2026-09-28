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
- **[세션 3 · VSM 태양 페이지 캐시 · 성능, 사용자 결정 2026-09-28]** `shadow.vsm.cache = true`(false = 이전 one path 그대로). S가 설계 개정 1 14.2④로 캐시를 뺐던 것을 사용자 결정으로 되살리되, 최악(해가 무제한 속도로 움직여 전부 다시 그림)은 one path와 같은 경로·비용이 되게 했다. 구조(VsmCache.hlsl, VsmScan.hlsl, VsmClearPages.ms.hlsl, VsmSystem.cpp): (1) 인스턴스마다 캐스터 상태(변환·변형 revision, 그림자·숨김 플래그)를 지난 프레임과 비교해 바뀐·나타난·사라진 캐스터와 매 프레임 움직이는 캐스터(바람·모프·지형 패치)의 지금·이전 경계 구를 목록에 올리고, 스킨은 V의 이번 프레임 posed 경계(`FrameResources::skinBounds`, V가 새로 게시; 무한 경계면 전체 다시 그림), (2) 그 구 아래의 상주 태양 페이지를 stale로, (3) 요청된 태양 페이지 중 지난 프레임에 같은 절대 페이지(tag)로 상주하고 stale이 아닌 것은 물리 페이지·내용·블록 계층·메타를 그대로 두고(kept), (4) 나머지만 결정적 순서의 빈 물리 페이지 목록에서 받아(아무것도 안 남기면 0, 1, 2, … = one path와 같은 배정) 메시 셰이더 사각형으로 그 페이지만 지우고(깊이 ALWAYS, 0) 래스터(컬 마스크 = DIRTY만)·pagemax. 국소광 페이지는 매 프레임 그림(기차·문과 함께 움직임). 전체 다시 그리기 조건: 해 방향, 캐스터 높이 범위(범위는 이번 프레임 캐스터가 안에 있는 동안 유지, 벗어나면 양쪽 25 % 넓혀 다시 그림), 장면 revision, 원점 이동, 복원, 아틀라스 재생성, 바람 속도·방향 변화, 비연속 프레임, 목록 용량 초과. 통계: `VsmStats::cachedPages`(kept), requested = kept + 새, dirty = 그린 페이지. **확인:** (1) VsmTests(캐시 켬: 정지 프레임 dirty 0·전부 kept, 카메라·캐스터 이동은 일부만 그림, 이동·느린 이동·해 변화·바람 뒤 기준 대비 비교 통과 = 남긴 페이지가 새로 그린 것과 같음; `--set shadow.vsm.cache=false`로 one path 단언도), ShadowGate(정지 카메라 dirty 0 전제가 다시 성립), LocalShadowTests·FroxelTests. (2) 기차 내부 1440p → 출력 4K·목욕탕 1080p: s.vsm.raster.raster(기차 1440p 1.12, 4K 1.70 ms)·s.vsm.pagemax·clear 감소량, 움직이는 기차에서 kept 비율(`cachedPages`/requested), 해가 움직이는 장면에서 one path 대비 추가 비용(캐시 패스 5개 ≈ 수십 µs [예상]). (3) 화면: 움직이는 캐릭터·기차·문·바람 식생 그림자에 잔상(stale 그림자)이 없는지, 캐릭터가 숨거나 나타날 때. GBV.
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

# gi.lumen — Lumen 화면 프로브 최종 수집 재구성 (R)

사용자 결정(2026-10-01 저녁, 조정 세션 전달): 조명 경로를 언리얼(Lumen·MegaLights) 구조의 재구성으로 대체한다. 먼저 전부 쓰고 돌린 뒤, 품질 판정과 수정은 세 세션의 결과가 한 브랜치에 모인 다음에 한다.

읽은 소스: `C:\Users\USER\RendererResearch\UnrealEngine-ue6-main\Engine\Source\Runtime\Renderer\Private\Lumen\LumenScreenProbe*.cpp`, `...\Engine\Shaders\Private\Lumen\LumenScreenProbe*.usf/.ush`. 코드는 옮기지 않았다(구조·수치·알고리즘만 읽고 새로 씀).

## 스위치와 출력

- `gi.lumen = true`: `r.gi.lg.*` 패스가 `r.gi.screen`과 그 필터 대신 주 시야의 간접광을 만든다. 월드 캐시 갱신은 계속 돈다(적중점 조명의 출처).
- 출력: `view.giIrradiance`(RGBA16F, 조도 × 노출, a = 누적 프레임 + 1; M이 읽는 기존 텍스처), `view.giRoughSpecular`(RGBA16F, 거친 스페큘러 로브의 평균 입사 radiance × 노출, a = 빠른 갱신 양).

## 항목과 파일 (`Native/Render/Passes/GI/Lumen/`, `LumenGather.cpp`)

| 항목 | 패스 | 파일 | 상태 |
|---|---|---|---|
| (a) 프로브 배치 | r.gi.lg.place, adaptive.mark, adaptive.spawn | LgPlace, LgAdaptiveMark, LgAdaptiveSpawn | 빌드됨 |
| (b) 8×8 광선·중요도 표본 | screendata, lightingpdf, rays | LgScreenData, LgLightingPdf, LgGenerateRays | 빌드됨 |
| (c) 추적·hit 조명 | screentrace, trace | LgScreenTrace, LgTrace (SKY0/1) | 화면 추적(S2 공용) → 하드웨어 광선, hit 조명은 표면 캐시(없으면 hit 셰이딩) |
| (d) 먼 거리 radiance cache | rcmark (+ A의 r.gi.rc.*) | LgRcMark, LgRadianceCache.hlsli (A: LumenRadianceCache*) | A의 모듈을 연결: 프로브가 칸 표시, LgTrace가 범위에서 멈추고 miss면 캐시, LgLightingPdf가 이력 없는 방향을 캐시로. 스위치 `lumen.radiance_cache` |
| (e) 프로브 공간 필터·세기 상한 | composite, filter0..2 | LgComposite, LgFilter | 빌드됨 |
| (f) 프로브 시간 누적 | probetemporal | LgProbeTemporal | 빌드됨(기본 꺼짐: 언리얼 기본값) |
| (g) 화소 보간·적분 | irradiance, integrate | LgIrradiance, LgIntegrate | 빌드됨 |
| (h) 짧은 거리 AO / bent normal | (A의 r.gi.sao*) | (A: LumenShortRangeAO*) | LgIntegrate가 굽은 법선 방향 조도 × 다중 반사 AO, 거친 스페큘러 × 스페큘러 AO. 스위치 `lumen.short_range_ao` |
| (i) 화소 시간 필터 | temporal | LgTemporal | 빌드됨 |

GPU 실행 기록은 CLOUD_BRIEF에 적는다.

## 품질을 내주는 값 (사용자 결정 항목, 지금은 언리얼 기본값)

| 설정 | 값 | 무엇을 내주는가 |
|---|---|---|
| `gi.lumen_max_ray_intensity` | 10 | 광선 하나의 노출 뒤 radiance를 10으로 자름: 반딧불은 없어지지만 작은 밝은 광원의 간접광이 어두워짐(편향) |
| `gi.lumen_temporal_max_frames` | 10 | 화소 이력 길이: 길수록 조용하고 조명 변화가 늦게 따라옴 |
| `gi.lumen_max_roughness_rough_specular` | 0.8 | 이 거칠기 위에서는 거친 스페큘러를 조도/π로 대신함 |
| `gi.lumen_filter_passes`, `_max_hit_angle_deg` | 3, 10° | 프로브 사이 섞기: 접촉 그림자·작은 간접 그림자가 흐려짐 |
| `gi.lumen_tile`, 프로브당 64 광선 | 16 px | 간접광의 공간·각도 해상도 |
| 거친 스페큘러 표본 4개, 거칠기 하한 0.2 | — | 0.2보다 매끈한 로브는 0.2로 넓혀 읽음 |
| 프로브 radiance → SH3 → 조도 | — | 확산 조도는 2차 SH까지만(날카로운 간접 그림자 없음) |

## 언리얼과 다르게 둔 점

1. **화면 추적.** `gi.lumen_screen_traces`(기본 켬): S2의 공용 화면 추적(`Passes/Reflection/ScreenTrace.hlsli`)을 `r.gi.lg.screentrace`에서 쓴다(반복 50, 상대 두께 0.02, 두께 확인 4걸음 = 언리얼의 화면 프로브 기본값). 다른 점: 적중점의 이전 프레임 깊이 검사 없음(깊이 이력이 없다), 적중점 색은 정지한 것으로 재투영, 화면 추적 거리를 radiance cache 범위로 자르지 않음(루트 상수 자리가 없다), 색 이력이 없는 프레임(첫 프레임, 컷)은 화면 추적을 건너뜀(언리얼도 같다: 컷이면 이전 시야 정보를 새로 만들어 입력이 무효; 그다음 프레임까지 건너뛰는 비교 스위치 `gi.lumen_screen_trace_skip_after_cut`, 기본 꺼짐), 화면 추적 뒤 월드 광선은 8 cm 물러난 곳에서 시작(언리얼 PullbackBias).
2. **hit 조명.** `gi.lumen_hit_surface_cache`(기본 켬, `surface_cache.enabled` 필요): 적중점이 S2의 표면 캐시 칸을 표시하고, 칸이 조명을 받은 상태면 칸의 조도(국소광 + 다중 반사)를 쓴다. 표면 캐시가 꺼져 있으면 적중점에서 해(그림자 광선 1개), 국소 광원 표본 1개(방향 가중 선택, 그림자 광선), 월드 캐시의 간접광을 읽는다. 캐시는 있는데 칸이 아직 조명받지 않았으면 0이다(언리얼과 같음; 14번의 스위치로 앞의 폴백을 켤 수 있다).
3. **난수.** 블루 노이즈 LUT 대신 좌표 해시 + R2 수열(`lgNoise2`). 타일 지터는 같은 8프레임 Hammersley.
4. **적응 프로브 목록.** 타일당 R16 텍스처 대신 버퍼(타일마다 머리 + 8칸). 생성 판단은 마스크만 읽어 스레드 순서와 무관(언리얼과 같은 규칙).
5. **아틀라스 행 수.** 적응 프로브 행 = ceil(최대 적응 수 / 가로)(언리얼은 세로의 절반을 버림).
6. **재투영.** 속도 버퍼가 없어 vis id에서 표면의 이전 위치를 구한다(`GiScreenHistory.hlsli`). 화소 이력의 유효 판정은 이전 평면까지의 상대 거리 0.01(언리얼 DistanceThreshold)이고 법선 판정은 없다(언리얼 기본값도 꺼짐).
7. **움직임 판정.** 변형 메시(스키닝) 적중점의 속도는 인스턴스 변환만 본다(정점 변형 속도 없음).
8. **광선 시작 편향.** 법선 방향 `gi.lumen_normal_bias`(1 mm) + 거리 비례 항(1 mm + 0.2 mm/m). 언리얼의 PullbackBias·AvoidSelfIntersection 값은 쓰지 않았다.
9. **거친 스페큘러 출력.** 프레넬·스페큘러 색을 곱하지 않은 로브 평균 입사 radiance(읽는 쪽이 곱함). 4표본은 GGX 보이는 법선 분포.
10. **양면 잎(뒷면 확산)·머리카락·Substrate 다층.** 해당 분기는 쓰지 않았다(재질 모델이 다름).
11. **타일 분류(적분 방식별 타일 목록), 다운샘플 적분, 밉 생성.** 쓰지 않았다(모든 화소가 조도 맵 경로, 전 해상도).
12. **분석적 면광원 프록시.** 광선이 맞으면 radiance 0(M이 그 광원을 직접 셰이딩하므로 두 번 세지 않음). 언리얼에는 없는 이 렌더러의 규칙.
13. **스냅 프레임의 세기 상한 기준 노출.** `gi.lumen_cap_snap_exposure`(기본 켬): 노출이 그 시야에서 측광되지 않은 프레임(실행 첫 프레임들, 컷·복원 뒤 새 시야의 히스토그램이 돌아올 때까지)에는 상한을 그 프레임의 추적 radiance로 측광한 노출 기준으로 건다(`LgMeter.hlsl`, M의 측광과 같은 식). 언리얼은 표시 노출이 상한의 기준(pre-exposure)과 같이 움직여 이 어긋남이 없다. 우리는 스냅 프레임의 출력이 노출을 바로 보정하므로, 그대로 두면 밝은 쪽 컷 뒤 간접광이 어둡고(기차 f60 −34 %) 첫 프레임·어두운 쪽 컷에서는 상한이 작동하지 않는다.
14. **유효한 칸이 없는 적중점.** `gi.lumen_hit_fallback`(기본 꺼짐 = 언리얼의 규칙: 0). 켜면 월드 캐시와 광원 표본으로 셰이딩한다(2번 항목의 폴백). 로비 [실측]: 켜면 실행 간 따뜻함/회청 갈림이 생기고, `surface_cache.base_cells`와 같이 쓰면 첫 프레임들의 수준이 튄다(f4 19.7 → 정상 14.1; 끄면 12.6 → 14.1). 정상 상태는 둘이 같다(적중점이 전부 유효한 칸을 읽음).

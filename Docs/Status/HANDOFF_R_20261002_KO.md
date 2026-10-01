# R 인계 노트 (2026-10-02) — gi.lumen (Lumen 화면 프로브 최종 수집 재구성)

작성: R 세션. 사용자 결정(조정 세션 전달)으로 이 세션은 여기서 끝나고, 새 세션 하나가 코드를 이어서 쓴다. GPU 실행은 `.gpulock/HOLD`로 막혀 있다(코드와 빌드만).

## 1. 브랜치와 끝

- 작업 폴더 `C:\Users\USER\UnravelNext-redesign`, 브랜치 `redesign-v2`, 끝 = 이 노트를 넣은 커밋(코드의 끝은 `eb9ec1e`). origin/redesign-v2에 푸시됨.
- 병합되어 있는 것: `redesign-v2-refl` cd6452c까지(S2: 표면 캐시 `base_cells`, 공용 화면 추적), `redesign-v2-fix` ce6fe7a까지(A: radiance cache, 짧은 거리 AO). **A의 그 뒤 커밋(MegaLights 띠 수정 d936d28, `direct_pairs`, 메시 카드 6ec67ad 등)은 아직 병합하지 않았다.**
- 작업 트리에 쓰다 만 코드는 없다(전부 커밋됨, `build\dev2`가 `eb9ec1e`의 빌드). 빌드: `Tools\CI\Build.ps1 -Track dev2 -Tracks all`.

## 2. 끝난 것 (파일·스위치)

상세 표는 `Docs/Status/LUMEN_GATHER_KO.md`(항목, 스위치, 품질을 내주는 값, 언리얼과 다른 점 14개).

- 모듈 `Native/Render/Passes/GI/Lumen/*.hlsl(i)`, 기록 코드 `Native/Render/Passes/GI/LumenGather.cpp`(`GiSystem::recordLumen`), 설정 `GiSettings::Lumen`(`unx/gi/GiSystem.h`), 키 `Config/quality/gi.toml`의 `lumen_*`.
- 패스 순서: place → adaptive.mark/spawn → (radiance cache: rcmark + A의 r.gi.rc.*) → screendata → lightingpdf → rays → [screentrace] → trace(행 띠) → [meter.clear/bins/meter: 스냅 프레임만] → composite → [probetemporal] → filter0..2 → irradiance → integrate → temporal. 출력 `view.giIrradiance`, `view.giRoughSpecular`.
- 스위치(기본값): `gi.lumen`(false), `gi.lumen_hit_surface_cache`(true), `gi.lumen_hit_fallback`(**false**), `gi.lumen_screen_traces`(true), `gi.lumen_screen_trace_skip_after_cut`(false, 비교용), `gi.lumen_cap_snap_exposure`(true), `gi.lumen_temporal_filter_probes`(false), `gi.lumen_rays_per_dispatch`(262144; 띠는 스레드당 광선 3개로 계산). 같이 켜는 남의 스위치: `surface_cache.enabled`(S2), `lumen.radiance_cache`, `lumen.short_range_ao`(A).
- 실험 비트(`gi.experiment_disable`): 16 GI 적중의 태양 끔, 128 국소광 끔, 512 적중의 월드 캐시 읽기 끔, 1048576 프로브 광선 miss가 0, 2097152 적중의 표면 캐시 읽기 분류를 색으로(빨강 셀 없음, 초록 조명 전, 파랑 유효).
- 광원 격자의 가장 긴 칸 목록을 로그에 남김(`R light grid: …`, RayScene.cpp).

## 3. 쓰지 않은 것 (새 세션의 R 몫, 조정 세션 08:50 목록 순서)

1. **프로브 적중점의 hit lighting(`gi.lumen_hit_lighting`, 기본 true) — 시작하지 않았다.** 요구 구조: 적중 기록 → (적중, 광원) 쌍 목록(가장 센 8광원, 해석적 조도; compute, 광선 없음) → 쌍당 그림자 광선 1개(262,144 광선 띠) → 적중별 합산, 태양 포함. 스레드 하나가 루프 안에서 TraceRay를 여러 번 쏘는 꼴은 금지(라운지 hang의 꼴). 본뜰 구조는 A의 `surface_cache.direct_pairs`(redesign-v2-fix, 병합 필요). 지금 `LgTrace.hlsl`의 적중점은 태양 그림자 광선 1개 + 국소광 표본 1개(`rtLocalLightChooseOriented`)를 같은 스레드에서 쏜다 — 이것을 대체한다. 적중 기록에 필요한 것: 위치, 법선, 알베도, 프로브 추적 텍셀 좌표, 적중의 방출·표면 캐시 간접광.
2. 적중점의 간접광은 표면 캐시(`scRead`)만, 없으면 0 — **기본값은 이미 그렇게 되어 있다**(`gi.lumen_hit_fallback=false`). 표면 캐시가 꺼져 있으면 여전히 월드 캐시를 읽는 옛 경로가 남아 있으니, 카드 캐시가 들어오면 그 분기를 정리할 것.
3. 세기 상한의 스냅 노출 — **끝남**(`eb9ec1e`).
4. 이력이 0인 프레임(컷)의 대조 — 소스는 읽었고 코드는 바꾸지 않았다. 읽은 결과: 언리얼의 화소 시간 필터(`LumenScreenProbeGatherTemporal.usf`)는 이력이 없으면 그 프레임의 적분값을 그대로 내보낸다(따로 공간 필터 없음, 누적 프레임 0). 확률적 보간과 전해상도 지터도 컷 프레임에서 그대로 켜져 있다(`GetScreenProbeFullResolutionJitterWidth`는 품질 설정만 봄). 화면 추적은 컷 프레임만 건너뛴다(`View.PrevViewInfo` 리셋). 이 셋은 우리와 같다. **아직 대조하지 않은 것**: 컷 프레임에서 조명 pdf가 이력 대신 radiance cache를 읽는 분기의 세부(우리는 `lumen.radiance_cache`가 켜져 있을 때만 캐시로), 프로브 공간 필터의 "이력 없음(disoccluded)" 완화 조건이 컷 프레임 전체에 걸리는지.
5. 광원 격자 칸 순회의 상한 — 시작하지 않았다. A의 제안서 `Docs/Status/LIGHT_GRID_BOUND_PROPOSAL_KO.md`(redesign-v2-fix): 칸 목록을 조도 상한 순으로 정렬, 머리 32개는 정확히, 꼬리는 스레드마다 8개 균등 표본(가중 × N_t / 8). 소비자: `rtLocalLightChooseOriented`(HitLocalLights.hlsli; R의 gi.trace·lg.trace, A의 rc.trace, S2의 반사 적중), `mlWorldSamples`(A), `r.sc.cells`(S2). `rtHitDecals` 후보 루프도 같은 종류로 남아 있다.
6. 그 밖에 알려진 미완: 화면 추적의 보행 거리를 radiance cache 범위에서 자르지 않음(LgScreenTrace의 루트 상수 48워드가 꽉 참), 화면 추적 적중점의 이전 프레임 깊이 검사 없음(깊이 이력이 없음).

걸어 두었다가 철회한 실행(결과 없음): 로비 "데운 뒤 컷" — `--path-rotate 90 --path-time-list 1,0,1 --segment-frames 120`, 360프레임, 캡처 240·241·243·359, 월드 캐시 적중 대 표면 캐시 적중 대 읽기 분류. 표면 캐시가 차 있을 때 컷 첫 프레임에 남는 R 쪽 얼룩을 가르려던 것이다.

## 4. 함정

- **GI 모듈은 반사 모듈을 include 못 한다**(링크 순서: 반사가 GI를 링크). 표면 캐시와 화면 추적 입력은 `tracks::surfaceCache(fc)`, `tracks::screenTraceInputs(fc, main)`(Tracks.h, 구현은 `Passes/Reflection/ReflectionTrack.cpp`, 빈 구현은 `Frame/Stubs/TrackR.cpp`)를 FrameRenderer가 GI 앞에서 부르고, 결과는 `fc.resources.surfaceCache`, `fc.resources.screenTraceHzb`, `main.prevSceneColor`로 받는다. HLSL 헤더(`Passes/SurfaceCache/SurfaceCache.hlsli`, `Passes/Reflection/ScreenTrace.hlsli`)는 include해도 된다.
- GI 모듈은 M의 노출 상태를 `shading::exposureSnapping(fc)`로 읽는다(`module.cmake`에서 shading이 빌드에 있을 때만 링크, `UNX_GI_HAS_SHADING`).
- **게이트의 `--capture`는 업스케일을 끄고 네이티브로 그린다** → 색 이력이 없어 화면 추적 커널이 돌지 않는다. 게임 경로는 `--capture-output FILE.pfm`(내부 720p → 1080p). 파일 이름 없이 주면 게이트가 바로 끝난다.
- **비결정 실행은 로비에서 실행마다 갈린다**(월드 캐시 상태). 판정·비교는 `gi.deterministic=true`로 한다. 앞서 "화면 추적이 GI 수준을 올린다", "f1 60.5 급등"이라고 적었던 것은 이 갈림을 잘못 읽은 것이었다.
- GI 층 캡처는 E × 노출이다. 절대 수준 = 평균 × 2^EV(로그의 `captured gi … ev100` 줄). 프레임마다 EV가 다르므로 층 평균끼리 바로 비교하면 틀린다(`Results/Local/Redesign/lumen/lvl.py`, `cutbands.py`가 보정한다).
- **스냅 프레임**: 실행 첫 프레임들은 EV 14, 컷 뒤 2~3프레임은 이전 시야의 EV로 그려지고 출력에서만 보정된다(M의 `Exposure.cpp`). 노출을 곱한 값에 거는 문턱(세기 상한 10)은 그 프레임들에서 기준이 틀린다 — `LgMeter.hlsl`이 프로브 추적값으로 측광해 바로잡는다. 새로 넣는 문턱도 같은 문제를 확인할 것.
- 루트 상수는 48워드(`uint4 P[12]`). Lg 커널은 P[8..11]이 공통 블록(LgCommon.hlsli 머리 주석).
- 품질 설정은 실행 때 소스 트리의 `Config/quality`에서 읽는다(빌드에 굽지 않음): 대기 중인 실행이 있을 때 toml을 바꾸면 그 실행의 설정이 바뀐다.
- 라운지에서 표면 캐시를 켜면 장치 제거(DEVICE_HUNG)가 났었다(S2의 직접광 그림자 광선 쪽, 원인 미확정). R은 라운지에서 표면 캐시를 켜지 않았다.
- PowerShell `*>` 로그는 UTF-16이다.

## 5. 측정으로 확인된 사실 (전부 [실측], 1080p 출력·내부 720p, 따로 적지 않으면 결정적 실행)

- 로비, 프로브 적중의 표면 캐시 읽기 비율(셀 없음 / 조명 전 / 유효, base_cells 전): f4 63.3 / 36.6 / 0.1 %, f16 61.9 / 35.7 / 2.3 %, f60 9.4 / 5.4 / 85.2 %, f299 0 / 0 / 100 %.
- 로비, gi.lumen만(월드 캐시 적중)의 GI 층 귀속: 적중의 월드 캐시 읽기를 끄면 수준 29.7 → 1.9(층의 94 %가 그 읽기를 거침), 적중의 태양을 끄면 11.1(63 %가 입구로 든 햇빛), 하늘 miss는 기여 없음.
- 로비의 따뜻함/회청 갈림(비결정 4회 중 1회꼴, 수준 21.5–32.6 대 41–52)은 월드 캐시 폴백 읽기에서 온다: 폴백을 끄면 8회 중 8회 따뜻한 쪽. base_cells와 함께 300프레임: 폴백 켬 f4 19.7 → 정상 14.1, 끔 12.6 → 14.1.
- 로비의 정상 수준: 표면 캐시 적중 14.1, 월드 캐시 적중 약 30, 세기 상한을 끄면 64–76(뒤의 값은 비결정 실행). 어느 것이 맞는지는 기준 렌더와 비교하지 않았다.
- 컷 프레임 화면 추적(로비 f1/f2 수준): 켬 32.1 / 31.7, 끔 31.1 / 30.0 — 급등 없음.
- 기차 데운 뒤 컷(`--path-rotate 90 --path-time 0 --cut-at 60:1.0`), GI 층을 f359와 비교(수준 / 64 px보다 큰 얼룩 / 8–64 px / 8 px 미만): 옛 경로 f60 +9 / 50 / 37 / 33 %, gi.lumen f60 0 / 15 / 18 / 28 %, f63 +3 / 5 / 7 / 10 %. 최종 화면 tile P95 f63: 옛 경로 76 %, gi.lumen 약 58 %. **컷 첫 프레임은 아직 깨끗하지 않다.**
- 세기 상한이 정상 상태에서 내주는 양: 기차 −9.4 %(3045 대 3363).
- 광원 격자: 로비 광원 827개에 칸 목록 최대 52, 기차 48개에 최대 12.

도구: `Results/Local/Redesign/gia/lumen_multi.ps1`(잠금 한 번에 여러 실행; 환경 변수 PG_BIN, PG_SCENE, PG_RUNS, PG_OUTPUT, PG_FRAMES, PG_CAPTURES, PG_LAYERS, PG_EXTRA), `Results/Local/Redesign/lumen/view.py`(시트), `lvl.py`(절대 수준), `cutbands.py`(공간 규모별 차이). 실행 기록과 수치는 `Docs/Status/CLOUD_BRIEF_KO.md` 끝의 "R — 판정 목록" 절.

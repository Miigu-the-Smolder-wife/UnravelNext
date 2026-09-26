# C 트랙 (기준·콘텐츠) 상태 — 2026-09-25

표기는 설계서와 같다: [실측]은 이 기계(i9-13900KF 32스레드, 다른 세션 빌드와 공유)에서 실제 실행한 값, [예상]은 그 실측에서 외삽한 값.

## 명령

```text
powershell -File Tools/CI/Build.ps1 -Track C                       build/C (코어 + C)
build/C/bin/unx_test_scenegen.exe                                  장면 결정성·검증·게이트 부하 (≈35 s)
build/C/bin/unx_test_reference.exe [이름]                          기준 경로추적기 닫힌 해 검사 (전체 ≈4 min)
build/C/bin/unx_test_census_replica.exe thin|card grass|nograss    인구조사 도구 ↔ 마이크로벤치 GPU 인구조사
build/C/bin/unx_scenegen.exe --scene <이름|all> --out <폴더>       .unxscene 생성
build/C/bin/unx_reference.exe render|census|compare|selfcheck ...  기준 영상(캐시)·인구조사·비교 (도구 머리말 참조)
powershell -File Reference/Tools/RenderQueue.ps1                   게이트 기준 영상 대기열 (BelowNormal, 재개 가능)
```

## 완료

| 항목 | 내용 | 검증 |
|---|---|---|
| 장면 생성기 | CityBlock, ForestThin(잎 6 cm 기하·풀잎 4 mm), ForestCard(잎 35 cm·풀 카드 30 cm, 알파), Waterside(잔잔한 평면 + 파도 만, 젖은 바위, 갈대 3 mm), Interior(거칠기 0.15/0.25/0.35/0.5 바닥 띠, 거울, 크롬 구, 면광원 6, 창 햇빛), CityNight(광원 512, 그림자 128, 젖은 도로 0.15~0.35), RidgeSunset(S 요청: 20 km, 3 km 앞 800 m 능선, 태양 17°, 300 m 탑 4개 — 공기 속 그림자). 정지·이동 경로 | 결정성(같은 요청 = 같은 해시), `validate`, 광원 수·수목/클럼프 수 [실측, 테스트 통과]. 생성 1~2 s/장면 |
| 기준 경로추적기 | Embree 4.4.1, INTERFACES 8 모델 전체, 편향 없음(러시안 룰렛), 절반 두 장 독립, 체크포인트 재개, 캐시 `Cache/Reference/...` | 아래 표 |
| 지표 | 코어 골격(PFM, relMSE, HDR/LDR-FLIP, 시간 불안정도) + 16-부표본 인구조사(`.unxids` 엔진 캡처 형식, 1표본 기준선) | 인구조사: 마이크로벤치 장면 CPU 복제가 GPU 값과 마지막 자리까지 일치 [실측, `Results/C/Census/`] |

### 기준 경로추적기 검사 [실측, `unx_test_reference`]

| 검사 | 결과 |
|---|---|
| 점광원 / 사각 / 구 / 원판 면광원 조도 (닫힌 해) | 상대 오차 −1.7e-5 / −4.4e-5 / −7.0e-5 / −4.9e-5 |
| 그림자 (검은 판) | 점광원 1.2e-5, 태양 2.1e-4 × 직접광 (잔여는 판의 스침각 Schlick 반사) |
| 흡수만 있는 대기를 지난 태양 | −1.1e-5 |
| 얇은 대기(계수 × 0.01) 하늘 = 단일 산란 적분 | +0.6% (다중 산란 몫, 예상 ~τ) |
| 전체 대기: 강제 내산란 NEE ↔ 충돌 NEE 두 추정기 | 차이 0.06~0.16% (허용 0.7~1.3%) |
| 대기 광학 깊이 표(2048 × 1024, 삼차) | 최대 투과율 상대 오차 5.8e-5 (T > 1e-4, 20000 표본) |
| BSDF 표본·pdf 일관성, 재질 모델(double) ↔ `scene::model::evaluate` | 통과; 거칠기 ≥ 0.2에서 7.6e-5 |

### 처리량 [실측, 다른 세션과 CPU 공유] 과 게이트 기준 영상 시간 [예상]

- city_block/street: 3.7 M 경로/s → 1440p·4096 spp ≈ 1.1 h, 4K ≈ 2.6 h.
- forest_thin/forest(1.1 M 인스턴스): 2.1 M 경로/s → 1440p ≈ 2 h, 4K ≈ 4.5 h.
- 대기열은 BelowNormal 우선순위로 돈다(다른 세션 빌드·CPU 측정이 선점). 실행 파일 스냅샷 `Cache/Reference/bin`에서 돌아 build/C 재빌드를 막지 않는다.

### 완성된 기준 영상 [실측]

| 장면/카메라 | 해상도 | spp | 시간 | 절반 간 relMSE |
|---|---|---|---|---|
| city_block/street (`--no-wind`) | 2560x1440 | 4096 | 3954 s | 0.00634 |

대기열 순서: ridge_sunset 1440p → 4K (S 검증용, 먼저) → city 1440p(완료, 캐시) → forest_thin 1440p(2688 spp 체크포인트에서 재개) → forest_card 1440p → city 4K → forest_thin 4K → interior floor_60·mirror → city_night wet_road → waterside lake.

## 설계 결정과 한계 (기록)

- **대기:** 모든 경로 구간의 참여 매질. 짧은 구간은 Gauss-Legendre(패널 = 고도 폭 2 km당 1), 긴 구간은 검증된 표. 하늘빛 분산은 강제 내산란 NEE로 줄인다(편향 없음). 행성 지면은 장면 기하가 없는 광선만 맞는다(골짜기가 행성 반지름 아래로 내려가도 가려지지 않게).
- **해석 광원은 카메라 광선에 보이지 않고 가리지 않는다**(실시간과 같은 의미). 반사로는 보인다(표면 정점의 NEE/MIS). 그림자를 끈 광원은 NEE만(가시성 1).
- **바람:** 변형 기하를 인스턴스별로 만든다(한도 1.5억 삼각형). ForestThin/Card(수목 10만 + 클럼프 100만)는 한도를 넘으므로 `--no-wind`로 그린다(장면 해시가 바뀌고 `--write-scene`으로 같은 장면을 엔진에 준다). 정지 품질 비교는 바람 없는 장면이 맞다(시간 안정성 지표도 정지 장면 정의). 바람 잎 위치 오차 측정(설계서 3절 식생)은 도시·수변(변형 기하 가능)에서 한다. 숲 규모의 정확한 바람은 순회 중 인스턴스별 변형(Embree 사용자 기하)이 필요하다 — 미구현.
- **물·유리:** INTERFACES 8.1에 Water/Glass 모델이 없어 Standard(물 f0 0.02, 창 f0 0.04)로 만들었다. 모델이 정의되면(P4) 장면을 바꾼다.
- **숲 인구조사 값이 마이크로벤치보다 크다**(얇은 식생 놓침 23% vs 13%): 도구는 복제 장면으로 일치를 확인했으므로 장면 차이(수평 카메라 vs 10° 아래, 기하 잎 6 × 3 cm 전체 면적 vs 알파 37% 쿼드)다. P1 게이트("놓침 ≤ 0.1%")는 엔진 캡처로 판정하므로 영향 없다.

## 요청 (Docs/Design/Requests)

- `20260925_C_embree.md` — Embree를 `Unx::Embree`로 고정. 대기 중(C 폴더에서 같은 URL·SHA-256으로 임시 처리).
- `20260925_C_wind_direction.md` — `windOffset`의 M·d → Mᵀ·d. 코어 반영(v1.1).
- `20260925_C_ggx_precision.md` — GGX D의 float 소거(거울에서 1/0). 코어 반영(v1.4).
- `20260925_C_scene_ridge_sunset.md` — `SceneId::RidgeSunset = 6` 추가(C 헤더에 덧붙임)를 INTERFACES 10.1에 기록. 대기 중.

## 남은 일

1. 게이트 기준 영상 렌더(대기열 진행 중) → 장면별 기준 잡음(절반 relMSE, 절반 간 FLIP) 실측.
2. 그 잡음 위에 `Config/quality/reference.toml`의 장면별 임계값(FLIP 평균·P99, relMSE, 시간 안정성) 결정. `unx_reference compare`가 이 키를 읽는다(지금은 키가 없어 비교가 실패한다 — 값 없이 기본값을 두지 않는다).
3. 숲 규모의 정확한 바람(순회 중 변형).

## 재개 지점 (2026-09-27, 분류 결정으로 정지; 9/30 초기화 뒤 재개)

0. (9/27 완료) haircoverage 시험 dispatch 최장 시간 실측(`Results/C/Tdr`): 모든 dispatch가 약 1 ms 이하다(S 다중 산란 표 PASS 0은 z 조각 32개로 조각당 0.44~0.86 ms, ms.radiance 32 dispatch 합 31 ms). 50 ms 넘는 dispatch가 없어 쪼갤 것이 없다. 첫 프레임 합계는 0.28~0.51 s이고 S의 1회성 다중 산란 표 빌드가 대부분이다(첫 실행 한 번 ms.single 237 ms, 다음 실행 13.9 ms: 첫 실행 클럭·초기화 차이). 결론: 이 시험에는 긴 dispatch가 없다. 9/26 TDR은 게임과 GPU를 나눠 쓰는 중 0.3~0.5 s 첫 프레임 명령 목록이 걸린 것으로 보이며 근본 원인(드라이버 선점)은 이 시험으로 더 가를 수 없다. 첫 프레임 멈칫은 S에 알림.
1. 착지: A6 투과 층(f0f1031, v1.67), A11 단면 재질(8335200, v1.71), 특수 기록 목록·M 선셰이딩 비트·A14 V/코어 부분·바다 V 부분·HiZ 크기 변경 결함 수정(v1.73). TDR(9/26 23:15): 세 변경은 원인이 아님(게임 없는 하드웨어에서 켜고 끈 실행 모두 통과, `Results/C/Bisect`). 게임 경합은 계기일 뿐이다. 구조로 묶인 dispatch는 경합에서도 2 s에 닿지 않으므로 근본 원인은 미확정이다.
2. A14 나머지: 보조 뷰의 froxels·GI·reflections·particles 뷰별 호출은 각 트랙 일반화 뒤(요청 20260926_C_per_view_history 5절: S는 shadowMarkView·froxels(view)·뷰별 클립맵 조건, R은 renderView 두 단계 분할). Split 뷰 post·FrameConstants viewOutputs는 렌더 A. FrameRenderer 수준 A14 시험(주 뷰 + 크기가 다른 보조 뷰 2개)이 남았다.
3. 바다: W가 waterGeometry에서 oceanDepth·waterSurface를 채우고, W의 가장자리 32 부표본 패스를 FrameServices::coverageAppend에 건다(W 몫). 합류 뒤 바다 가장자리 기록의 면적 정확성 시험을 V 쪽에 추가한다.
4. C5·C6 나머지(FEATURE_STATUS 렌더 C 표)와 C7·C8 HLOD 보류분은 재개 때 표에서 다시 순서를 정한다. 하드웨어에서 아직 안 돌린 것: 이번 커밋 뒤 shadow 3종(vsm·localshadow·froxel) 재실행.
5. 알려진 main 결함(내 변경 무관): visibilitytests coverage_layer_is_exact가 1a616eb에서 같은 자리에서 실패(렌더 A의 f056694 투과 합성 추정, A에 알림). WARP로 전체 프레임 시험을 돌리면 RayScene AS 빌드에서 d3d10warp 정수 0 나누기(렌더 B/R에 알림 예정).

## 메시 입자 (조정 배정 2026-09-27 07:20, 재개 지점 — 구현 전)

설계: FEATURES_GAME 0.A 7b(렌더 A). 엔진 2 인계(2026-09-27): NativeVfx 실행기 v4(Unravel 87056534, 배포 5300a0d4), FX 방향 상태(UnravelNext da4da3a: ParticleSystem 방향 쌍 버퍼 orientation[2], posAge와 같은 배치, 32 B {quat xyzw 로컬→앵커(스트림 축), spin rad/s}, 규칙 VfxParticleMath 끝 nv_orientation_*; 프로그램 reserved6 = mesh_asset lo/hi, asfloat spin_min/max, 플래그 NV_STREAM_PROGRAM_ORIENTATION 256; 출력 FX_OUTPUT_MESH 1).
순서:
1. ParticleRenderInputs에 orientation[0/1] 추가(Passes/FX, 렌더 A 폴더 — A에 한 줄 알림).
2. `FxMeshInstances.hlsl`: 살아 있는 메시 프로그램 슬롯마다 gpu::Instance(160 B) 하나를 GpuScene::gpuInstanceRange()에 쓴다(개수 = patchData[kGpuInstanceCountElement].x 원자 증가, 용량 = fx.mesh_instances_max, 넘치면 통계).
   - 위치: FxLayerSetup의 fxParticleAt 규칙(Hermite, 출생·죽음 외삽). 방향: nv_orientation_advance(q_latest, ω, −(1−w)dt)(죽는 입자는 q_prev를 +w dt). 이전 변환: time − frameDt에서 같은 식.
   - 렌더러 축: R_r = M R_s M(M = diag(streamAxes)), 위치 = axes⊙(anchor + originAnchor + p) − worldOrigin. 크기 = size × sizeScale × 크기 곡선(메시 원래 크기 1).
   - flags: Dynamic | CastShadow(SKINNED·WIND 없음), materialRemap kNone, morph/patch kNone, transformRevision 매 프레임 증가.
3. mesh_asset(64비트) → 장면 메시 색인: 호스트 ABI `UnxVfxMapMeshAsset`(렌더 A 계약) + FX 상태 표. 키 없는 프로그램은 그리지 않고 통계.
4. 시험: GPU 기록 변환 = FX CPU 실행기(VfxStreamCpu) 참조 변환(위치·회전·크기) 일치, 토크 없는 회전의 tick 경계 연속(w = 1 → 다음 tick w = 0), 캡처(탄피 장면) — 그리기는 기존 동적 인스턴스 경로라 V/M 시험을 다시 쓴다.
5. Unity 저작(VfxOpcode.MeshShape 46)은 엔진 2.
비용 [예상]: 기록 N_m × 0.02 ns, 나머지는 인스턴스 N_m개의 기존 V·M 행.

## 잔잔한 물 평면 반사 (최종 스프린트, 2026-09-27) — 완료 c8737fb

- 구조: `Passes/Water` — PoolTrack이 정지 수면 평면을 `addWaterPlane`로 올림 → `waterSurface`가 마스크 2패스(`WaterPlanarMask.PASS0/1`) → 비용 규칙 → `renderView`(W의 water() 훅 안에서 호출, shade(main) 재진입) → 표 뒤 평면 블록(48 B × 4) → `waterPlanarReflection`(WaterSurface.hlsli)이 마스크 1·영상 이동 ≤ 0.1 px인 표본에서 GI 거울 로브/반사 작업을 대체.
- 비용 상수는 `WaterSurface.cpp` kPlanar*(패스 합 실측). 타임스탬프 구간은 다른 큐 겹침을 포함해 3.2 ms로 과대(한계 비용 아님) — 온라인 적합은 넣었다가 뺐다. 카메라 패스가 바뀌면 게이트(`--planar on/off`, `--pool 3`/`12 --ahead 4`)로 다시 잰다.
- 남음: 기록(가장자리) 표본도 마스크 1이면 카메라를 쓰지만 기록 경로 전용 시험은 없음; 물속 카메라(아래에서 본 수면)의 거울상·스넬 창 카메라(1.3 (b)); 여러 수조가 겹칠 때 후보 4개 상한; S 평면 froxel 고정비 0.67 ms(B).

## A5 피사계 심도 (재배정 4, 2026-09-27)

- 프레임 합류는 이미 되어 있다(ShadingSystem: 시간 적분 뒤, RGBA16F 중간 영상; 렌즈 값은 호스트 → FrameContext). 정확성: PostTests --dof 하드웨어 통과(relMSE 3.2e-5 ~ 4.1e-3, 두 번·다른 메모리 뒤 비트 동일).
- 4K 비용 [실측, `PostTests --dof-time`, RGBA16F, 중앙값 8프레임]: 조준(무기 15 %, ρ −30 px) 1.84 ms, 화면 전체 흐림(ρ +20) 3.28 ms, 초점 근처 0.97 ms. reach를 화면 최대 반경까지만 보게 해 0.19 → 0.01 ms(23b31bb, 비트 동일). 설계 행(조준 0.2~0.3 ms)의 약 6~7배다.
- 음의 결과(기록, 되돌림): gather 옥타브 0 원천을 공유 메모리에 올리기(1.17 → 1.38 ms), 픽셀 커널 표를 공유 메모리로(0.51 → 0.74 ms), 옥타브 0만 남기기 실험(0.51 → 0.46 ms). 비용은 탭 로드나 레지스터가 아니라 정확 픽셀 커널의 탭당 연산(9~13탭)과 setup의 전 화면 고정비(0.36 ms)다.
- 게이트 (ii) 통과(2d7b59e, `unx_test_reference_dof`): 렌더러 DoF를 CPU 얇은 렌즈 기준의 핀홀 영상에 적용해 같은 장면의 얇은 렌즈 렌더와 비교했다. 내부 relMSE ≤ 1e-4(한도 1e-3), 가려짐 해제 가장자리는 3.4e-3·1.8e-3으로 보고했다. 픽셀보다 잘은 결의 텍스처는 초점 근처에서 2.2e-3이다(후처리 연산자가 가진 정보는 픽셀 값뿐이다).
- (a) 비용 재설계 다음 단계(측정 근거, 재개 지점):
  1. setup 0.36 ms: 32 px 타일마다 64스레드 그룹이 공유 메모리 약 29 KB를 쓴다 → SM당 워프가 적어 지연에 묶인다. 타일당 256스레드(스레드당 2 × 2)로 바꿔 점유율을 4배로 하고, 흐린 옥타브가 없는 타일은 피라미드 쓰기를 0 채움만 하게 한다.
  2. gather: 초점 근처 0.50 ms는 탭 9~13개의 정확 커널 연산이다(공유 메모리 적재·표 이동·옥타브 0만·통계 원자 제거 실험 모두 효과 없음). 설계 후보: 옥타브 ≥ 1을 단계 c − 1 해상도에서 모으기, 흐린 원천이 닿지 않는 타일(reach[c ≥ 1] < 0)을 옥타브 0 전용 커널로 나누기(간접 dispatch 타일 목록).
  2b. (음의 결과, 2026-09-27, 되돌림) 옥타브 0 탭을 픽셀 커널 지지(표의 첫 비영 행: 4·reach > 행 − 1)로 잘라 초점 근처 21 → 9탭: 출력 비트 동일(PostTests --dof 전 사례 동일)이지만 gather 0.50 → 0.55 ms(편차 안) — 초점 근처 비용은 탭 수가 아니다(`Results/C/Dof/doftime_support.log`). 남은 큰 항목은 흐린 경우(gather 2.83~2.96 ms, 픽셀당 옥타브 3 탭 약 61개 × 텍스처 2회)이고, 그 해결은 단계 c − 1 해상도 gather다. 정확 조건을 먼저 세워야 한다: 텍셀 퍼짐 w가 작은 원천(밝은 점의 보케 가장자리 w ≈ 1.4 px)과 수신 픽셀별 앞/뒤 층 경계가 낮은 해상도에서 뭉개지지 않게 — 예: 거친 격자에서 모으고, 원판 가장자리·층 경계가 걸린 수신 픽셀만 전 해상도로 다시 모으는 2단 구조.
  3. 실행 사이 편차가 크다(같은 코드에서 조준 gather 1.17~1.57 ms). 비교는 반복 실행 중앙값으로 한다.
  4. 오차 검증은 `unx_test_reference_dof`와 `PostTests --dof`로 한다.
- 남은 것: (a) 비용 재설계(옥타브 ≥ 1을 낮은 해상도에서 모으기, 초점 근처 경로, setup을 흐린 타일에만) — 오차는 같은 기준으로; (b) 기준 추적기(얇은 렌즈, GPU 경로추적기) 대 렌더러의 조준 장면 비교와 캡처 — 두 경로로 같은 장면을 그리는 도구가 필요하다.

# R 트랙 상태 (광선·GI·반사) — 2026-09-25

표기: [실측] = 이 기계(RTX 4080, 드라이버 591.86)에서 실행한 결과, [예상] = 비용식·가정.
빌드: `Tools/CI/Build.ps1 -Track R` (코어 + R). 정확성 실행은 GPU 잠금 없이, 성능 측정은 `Tools/CI/GpuLock.ps1 -Track R` 아래에서만.

## 1. 가속구조 (설계서 2.12, 2.8) — `Native/Render/RayTracing`

- `RayScene` (`include/unx/rt/RayScene.h`): 이전 TitanNative의 광선 장면 파티션·변형 로직을 식으로 옮겼다(뼈대 아님).
  - 강체 인스턴스: 메시당 BLAS 하나(원본 삼각형, 서브메시당 geometry, 알파 재질이 아니면 `OPAQUE`, `PREFER_FAST_TRACE`, compaction). 모든 인스턴스가 공유.
    인스턴스 재질 교체가 불투명 geometry를 알파로 바꾸면 인스턴스에 `FORCE_NON_OPAQUE`(any-hit가 불투명 재질은 받아들이므로 반대 경우는 정확).
  - 변형 인스턴스(스킨): 인스턴스당 BLAS(월드 공간 정점, `ALLOW_UPDATE | PREFER_FAST_BUILD`), 정점은 `RayTracing/Deform`이 `deformVertex()`(래스터·그림자와 같은 함수)로 쓴다.
  - 정적 TLAS(강체·비동적, 로드/스트리밍 경계에서만 빌드) + 동적 TLAS(Dynamic·변형, 매 프레임 재빌드). 광선은 동적 → 정적(TMax 절단) 순서로 질의.
  - 프레임 경로(설계서 4.1 C2): `r.as.deform` → `r.as.refit` → `r.as.tlas.dynamic`, 그래프 AS 사용(INTERFACES v1.1)으로만 동기. 트랙 상태 키 `R.rayScene`.
  - 감김: DXR 기본 앞면 규약이 우리 오른손 좌표의 CCW 앞면과 같다(`TRIANGLE_FRONT_COUNTERCLOCKWISE`를 쓰면 전부 뒤집힘) [실측, 테스트로 확인].
- `RayPipeline`: 라이브러리(lib_6_6 커널 파일)당 상태 객체 하나, 전역 루트 시그니처 = 장치의 bindless 시그니처, 로컬 루트 시그니처 없음, 셰이더 테이블은 기본 힙에 한 번 업로드.
- `RayShaders.hlsli`: 공용 hit group(closest-hit는 hit 식별만 기록, any-hit = 알파 테스트), `rtTraceClosest`, `rtVisible`. `RayScene.hlsli`: hit → 장면 데이터(인스턴스·서브메시·삼각형), 표면 재구성.
- **정확성 [실측]** `unx_test_raytracing_rayscene`: 회전·균일 스케일 정적 인스턴스 40, 동적 강체, 스킨 인스턴스, 서브메시 2개(알파 재질 포함) 장면에서 광선 200k를 CPU 전수 교차와 비교.
  빌드·refit 모두 hit/miss 불일치 0, t 불일치 0(스침각 조건수 반영 허용오차), 앞/뒷면 0, 법선 0, 가시성 광선 0, 동일 t 동률 12/94981(공유 대각 모서리). 디버그 레이어 + GPU 기반 검증 오류 0.
- 남은 것:
  - 캐릭터 RT 프록시(≤ 8k 삼각형): V의 클러스터 LOD 절단(`lodLevelClusters`, v1.1)에서 가져온다. 지금은 변형 인스턴스가 원본 메시 전체를 refit한다(통계 `deformedAboveProxyBudget`).
  - 군중 캡슐 프록시, 반사 정확 집합(원본 BLAS·바람 잎), OMM(자산별 실측이 any-hit보다 빠를 때만), 동적 인스턴스 서술자의 GPU 생성(코어가 인스턴스 변환을 tick마다 갱신하면).
  - 성능: 동적 TLAS 재빌드(인스턴스 0) 0.011 ms [실측, 2절 게이트]. 캐릭터가 든 장면(refit·프록시)의 측정은 캐릭터 장면이 생기면 한다.

## 2. GI (설계서 2.5) — `Native/Render/Passes/GI`

구조(`GiCache.hlsli`, `GiSystem.h` 주석):
- 월드 복사 캐시: 표면 위 항목, 키 = (레벨, 셀, 법선 부류 ±x/±y/±z). 화면 표면의 셀 = 카메라 거리 × tan(0.5°)(`gi.cache_cell_angle_deg`), 광선 hit의 셀은 최소한 그 광선의 footprint(텍셀 원뿔 ~0.36 t).
  해시는 64-bit 키·선형 탐사, 매 프레임 살아 있는 항목으로 다시 짓는다(삭제 표식 없음).
- 항목: 앵커(점·법선), 8×8 반구 팔면체 입사 복사 텍셀(RGB·hit 거리 fp16), 코사인 합성곱 L2 SH 조도(월드 좌표, fp16 ×1/64 저장 스케일), 앵커의 태양 가시성.
- 갱신: 프레임 광선 예산(0.5 M)을 **반구 전체 갱신(64광선) × 7812 항목**으로 쓴다. 요청은 두 층이다: 0층 = 화면 프로브가 읽는 항목,
  1층 = 전 프레임 GI 광선이 맞힌 항목(반사광 셀). 1층에 갱신의 `gi.hit_update_share`(25 %)를 보장하고, 층 안에서는 나이 히스토그램으로 가장 오래된 것부터 고른다.
  텍셀은 항목 이력 가중치로 섞고(리셋 뒤 `gi.jacobi_updates`=8회는 α=1(Jacobi), 그 뒤 최대 `gi.history_updates_max`=32회 평균),
  SH는 텍셀별 정확 적분표 + L2 정확 회전으로 투영한다. 리셋은 조명 epoch(장면 revision, 하늘 상수 변경)로 한다(설계서 2.5 "revision 무효화").
- hit의 나가는 복사 = 방출 + 알베도/π × (hit 점의 직접 태양: 태양 원반 안 그림자 광선 1개 + hit 셀의 간접 조도). 간접(2차 이후 반사)은 매끄러우므로
  그 셀은 광선 footprint(~0.36 t)의 `gi.hit_cell_footprint_scale`(4)배로 거칠다. 직접 태양은 셀 평균이 아니라 hit 점에서 정확하다(햇빛 든 면의 반사광 경계 보존).
- 캐시 압박: 빈 항목이 1/8 미만이면 LRU 나이 한도를 240 → 30프레임으로 낮춘다.
- 화면 프로브(8×8 px당 1): 광선 없이 캐시 SH 삼선형 + 근거리 가림(16탭, 반경 = min(0.5 m, 셀 크기): 캐시 해상도 아래의 가림만).
  M의 API `screenProbeIrradiance`는 평면 거리·법선 가중 4프로브 보간. 한 번도 갱신 안 된 항목은 보간에서 뺀다.
- **정확성 [실측]** `unx_test_gi_gianalytic` (1920×1080, 광선으로 만든 1차 가시성으로 V/M 대신):
  - 백색로(닫힌 상자, 방출 1, 알베도 0.5, 기대 E = 2π): 평균 −0.28 %, 최악 프로브 0.44 %, 냉시작에서 1 % 이내까지 13프레임, 살아 있는 항목 24 k.
  - 열린 하늘(L = 1, 기대 E = π): 평균 −0.03 %, 최악 0.08 %, 2프레임.
  - 디버그 레이어 + GPU 기반 검증 오류 0.
- **성능 [실측]** `unx_gate_gi_gigate` (GPU 잠금, 600프레임 중앙값, 큐 HIGH, SM 2820 MHz, C의 장면 파일, 정지 카메라 0, 1차 가시성은 광선 대역(R 예산 아님)).
  재질 텍스처(알파 포함)는 M의 텍스처 시스템 전이라 바인딩되지 않았다(알파 재질의 any-hit는 모두 받아들임).

  | 장면 | 해상도 | GI 합 | trace(0.5 M + 태양 광선) | gather | place | 기타 | 설계 | 캐시 |
  |---|---|---:|---:|---:|---:|---:|---:|---|
  | city_block (거리) | 4K | **0.351** | 0.159 | 0.075 | 0.046 | 0.07 | 0.45 | 172 k, 할당 실패 0 |
  | city_block | 1440p | **0.274** | 0.146 | 0.029 | 0.027 | 0.07 | 0.30 | 171 k, 실패 0 |
  | forest_thin (숲) | 4K | **0.888** | 0.566 | 0.12 | 0.08 | 0.12 | 0.54 | 200 k 가득, 할당 실패 9.5 k/프레임 |
  | forest_thin | 1440p | **0.780** | 0.568 | 0.06 | 0.04 | 0.11 | 0.30 | 가득, 실패 6.4 k/프레임 |

  - 도시: 설계 비용식 안. 가속구조(동적 TLAS 재빌드, 인스턴스 0) 0.011 ms.
  - 숲: 설계보다 0.35 ms 크다. 원인 구분:
    - **설계 가정 누락(trace).** 설계 비용식은 GI 광선 0.5 M만 셌고 hit의 직접 태양을 어떻게 얻는지 정하지 않았다. 햇빛을 받는 hit(숲 ~50 %)마다 그림자 광선이 하나 든다.
      이 광선은 텍셀 footprint 평균 태양 항의 정확한 MC 표본이라 품질 항목이다. 얇은 식생 alpha any-hit 바닥(1.2~1.3 G/s)에서 0.5 M + ~0.25 M 광선 = 0.56 ms.
    - **설계 가정 오류(캐시 크기).** 방향이 무작위인 얇은 잎은 셀마다 법선 부류 6개로 갈라져 화면 항목 60 k + 반사광 셀 68 k/프레임이 된다. 200 k 용량을 넘어 항목을 못 만든다(반사광 구멍).
      구조 수정안: 양면 얇은 식생(Foliage 부류)은 법선 부류 대신 **등방 항목**(구 전체 팔면체 8×8, 부류 하나)으로 키를 잡는다. 대역 C aggregate 브릭과 같은 표현이다. P3(식생)에서 한다.
    - 구현 비효율: `r.gi.age`(원자 128 주소 경합) 0.042 → groupshared, `r.gi.setup`(단일 스레드) 0.0125 → 128 스레드 [실측 반영, 숲 −0.03 ms].
      남은 후보: `r.gi.gather`(프로브마다 해시 8회 + 깊이 16탭) 0.12, `r.gi.place` 0.08.
- 남은 것: hit 셰이딩의 재질 텍스처(M의 텍스처 시스템), 국소광의 반사광(월드 광원 격자 + 그림자 광선), 식생 등방 항목(P3), 기준 경로추적기 대비 relMSE(C 트랙).

## 3. 반사 (설계서 2.6) — `Native/Render/Passes/Reflection`

K 경로: 요청 `Docs/Design/Requests/20260925_R_reflection_k_path.md`. K 픽셀은 화면 대부분이라 R의 전화면 패스로 풀면 20 B/px(≥ 0.28 ms, 4K)가 든다.
M의 셰이딩 커널이 화면 프로브의 방향 복사를 조회하게 하는 함수 `screenProbeRadiance`와 공용 분류 `reflectionLobeHalfAngle`, M이 쓰는 타일 최소 거칠기(`roughnessTiles`)를 요청했다(합 ≈ 0.1 ms [예상]).
G/M 광선 경로와 평면 반사 카메라를 먼저 만든다.

## 4. 인터페이스 사용 메모

- `view.screenProbes`: RGBA32_UINT, (probesX·4) × (probesY + 1), 형식은 `ScreenProbes.hlsli` 주석. M은 `ProbeSrvs{ srv, srv, 0, 0 }`(두 필드 모두 같은 텍스처)로 채우고 `SrvCompute`로 선언한다.
- `FrameResources::giCache`: raw 버퍼 하나(헤더에 구역 오프셋). `GiSrvs{ srv, srv, 0, 0 }`.
- DispatchRays 패스의 자원은 `SrvGraphics`/`UavGraphics`, TLAS·변형 BLAS는 `AccelerationStructureRead`(`RayScene::declareTraversal`).

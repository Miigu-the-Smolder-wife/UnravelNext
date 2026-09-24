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
  - 성능 게이트: TLAS 재빌드·refit·광선 처리량을 4K/1440p 장면에서 GPU 잠금 아래 측정 — 아직 안 했다.

## 2. GI (설계서 2.5) — `Native/Render/Passes/GI`

구조(`GiCache.hlsli`, `GiSystem.h` 주석):
- 월드 복사 캐시: 표면 위 항목, 키 = (레벨, 셀, 법선 부류 ±x/±y/±z). 화면 표면의 셀 = 카메라 거리 × tan(0.5°)(`gi.cache_cell_angle_deg`), 광선 hit의 셀은 최소한 그 광선의 footprint(텍셀 원뿔 ~0.36 t).
  해시는 64-bit 키·선형 탐사, 매 프레임 살아 있는 항목으로 다시 짓는다(삭제 표식 없음).
- 항목: 앵커(점·법선), 8×8 반구 팔면체 입사 복사 텍셀(RGB·hit 거리 fp16), 코사인 합성곱 L2 SH 조도(월드 좌표, fp16 ×1/64 저장 스케일), 앵커의 태양 가시성.
- 갱신: 프레임 광선 예산(0.5 M)을 **반구 전체 갱신(64광선) × 7812 항목**으로 쓴다. 요청 = 화면 프로브가 쓰는 항목 + 전 프레임 광선이 맞힌 항목, 선택은 나이 히스토그램으로 가장 오래된 것부터, 남으면 배경 항목.
  텍셀은 항목 이력 가중치로 섞고(리셋 뒤 `gi.jacobi_updates`=8회는 α=1(Jacobi), 그 뒤 최대 `gi.history_updates_max`=32회 평균), SH는 텍셀별 정확 적분표 + L2 정확 회전으로 투영한다.
  리셋은 조명 epoch(장면 revision, 하늘 상수 변경)로 한다(설계서 2.5 "revision 무효화").
- 화면 프로브(8×8 px당 1): 광선 없이 캐시 SH 삼선형 + 근거리 가림(16탭, 반경 = min(0.5 m, 셀 크기): 캐시 해상도 아래의 가림만). M의 API `screenProbeIrradiance`는 평면 거리·법선 가중 4프로브 보간.
- **정확성 [실측]** `unx_test_gi_gianalytic` (1920×1080, 광선으로 만든 1차 가시성으로 V/M 대신):
  - 백색로(닫힌 상자, 방출 1, 알베도 0.5, 기대 E = 2π): 평균 −0.26 %, 최악 프로브 0.40 %, 냉시작에서 1 % 이내까지 25프레임, 살아 있는 항목 40 k.
  - 열린 하늘(L = 1, 기대 E = π): 평균 −0.03 %, 최악 0.08 %, 2프레임.
  - 디버그 레이어 + GPU 기반 검증 오류 0.
- **설계 대비 바뀐 것과 이유**
  - 갱신 방식: 설계의 "항목 250k × 2광선"은 백색로에서 160프레임 뒤 −35 %였다 [실측]. 부분 텍셀 갱신 + 텍셀 이동평균은 반사 반복을 조화급수 속도로 늦추고, 광선 hit가 카메라 거리 셀로 항목을 만들면 화면 밖 항목이 177 k로 늘어 갱신이 굶는다. 반구 전체 갱신·footprint 셀·Jacobi 뒤 평균으로 바꿨다.
  - 메모리: 설계 4.2의 "200k × 160 B = 32 MB"는 8×8 팔면체 복사를 담을 수 없다. 실제 131.6 MB(텍셀 97.7 MB). 프레임당 접근은 갱신 7.8 k 항목 × 592 B + 프로브 조회라 L2 규칙(상주 ≤ 48 MB)의 대상인 작업 집합은 작다 [예상, 게이트에서 확인].
  - 재조명: 광원 변경 뒤 화면 항목 전체 재관측 = 요청 수 / 7812 프레임(백색로 31 k 요청 → 4프레임) [실측 통계]. 다중 반사 수렴은 추가 갱신이 필요하다(알베도 0.5에서 냉시작 25프레임).
- 남은 것: 하늘은 S의 `Atmosphere.hlsli`가 커밋되면 `GiTrace` 변형 SKY0으로 바꾼다(지금은 상수 하늘 변형만 빌드). hit 셰이딩의 재질 텍스처(M의 텍스처 시스템), 국소광의 반사광(월드 광원 격자 + 그림자 광선), 성능 게이트(4K/1440p), 기준 경로추적기 대비 relMSE(C 트랙).

## 3. 반사 (설계서 2.6) — `Native/Render/Passes/Reflection`

아직 시작 전. K 경로 비용 문제를 먼저 적는다: K 픽셀은 화면의 대부분이라 픽셀마다 월드 캐시를 조회하면 비싸다. 설계서의 K 조건 d_r ≥ 2ρ/θ_r의 ρ가 화면 프로브 간격이므로 K 경로는 화면 프로브의 방향 복사(prefilter)로 푸는 것이 정보량에 맞다.
M의 셰이딩 커널이 그 조회를 하려면 공개 API가 하나 더 필요할 수 있다(요청 예정).

## 4. 인터페이스 사용 메모

- `view.screenProbes`: RGBA32_UINT, (probesX·4) × (probesY + 1), 형식은 `ScreenProbes.hlsli` 주석. M은 `ProbeSrvs{ srv, srv, 0, 0 }`(두 필드 모두 같은 텍스처)로 채우고 `SrvCompute`로 선언한다.
- `FrameResources::giCache`: raw 버퍼 하나(헤더에 구역 오프셋). `GiSrvs{ srv, srv, 0, 0 }`.
- DispatchRays 패스의 자원은 `SrvGraphics`/`UavGraphics`, TLAS·변형 BLAS는 `AccelerationStructureRead`(`RayScene::declareTraversal`).

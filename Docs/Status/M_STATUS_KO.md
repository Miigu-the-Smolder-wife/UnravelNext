# M 트랙 상태 (재질·셰이딩) — 2026-09-25

표기: [실측] = 이 기계(RTX 4080, 드라이버 591.86, 큐 우선순위 HIGH)에서 실행한 결과, [예상] = 비용식·가정.
빌드: `Tools/CI/Build.ps1 -Track M`(코어 + M). 게이트 측정은 전 트랙을 켠 M 전용 폴더 `-Track Mall -Tracks "V;M;S;R;C"`
(`build/all`은 다른 세션이 측정 중일 수 있어 덮어쓰지 않는다), S·R 없는 분리 측정은 `-Track Mvc -Tracks "V;M;C"`.
정확성: `build/M/bin/unx_test_material_materialtests.exe`, `unx_test_shading_shadingtests.exe`(디버그 레이어, GPU 잠금 없음).
성능: `Tools/CI/GpuLock.ps1 -Track M -- build/Mall/bin/unx_gate_shading_mgate.exe --scene city_block --resolution both`.

## 1. 재질 해석 (설계서 2.2) — `Passes/Material`

- **텍스처 시스템** (`TextureSystem.cpp`): 장면 텍스처 전부를 전체 밉 체인으로 올린다(박스 필터, 배정밀도, 홀수 크기 분수 가중).
  - 베이스 컬러(알파 테스트 재질): 색 밉은 알파 가중(투명 텍셀 색이 잎 가장자리로 새지 않음), 알파 밉은 레벨마다 컷오프 통과 비율 유지.
  - 노멀맵: LEAN 계열 기울기 모멘트(RGBA16_UNORM: 평균 기울기, 내부 분산 대각합, |평균|²). 하드웨어 필터가 발자국 위 모멘트를
    정확히 평균한다.
  - 재질별 텍스처 표(M 내부). V의 알파 테스트·R의 hit 셰이딩이 쓰려면 `gpu::Material`에 게시해야 한다 → 요청
    `20260925_M_material_textures.md`(코어 반영 대기). 반영 전 통합 프레임에서는 V가 알파 재질을 잘라내지 못한다.
- **표면 재구성** (`MaterialSurface.hlsli`): 카메라 상대 픽셀 중심 광선 ∩ 변형된 삼각형 평면, 무게중심의 화면 미분을 해석적으로(클립
  공간 행렬식의 소거 없음).
- **대역제한 거칠기**: α'² = α² + (발자국 기울기 분산 대각합). 축소 시 LEAN 모멘트, 확대 시 mip 0 쌍선형 장의 정확한 기울기
  (셀을 셰이더가 정해 Load 4회; Gather의 고정소수점 셀 선택과 어긋나는 문제를 실측으로 확인), 곡률 = 보간 법선의 픽셀 박스 분산.
- **출력**: G-buffer 8 B, 재질 워드(M 내부: 재질 16 | metallic 8), 발광(텍스처 재질만), 클래스별 타일 목록 + 간접 디스패치 인자,
  `reflectionLobeTiles`(v1.3, R이 읽음, 내림 저장).
- **정확성 [실측]** (가짜 V 래스터, v1.5 형식): 배정밀도 복제 대비 uv 1.7e-4 픽셀 스텝, 미분 8e-5, 곡률 분산 1.7e-4, hit 위치
  6e-6(원점 2 km 카메라 3e-7), G-buffer 법선 6.5e-5, 거칠기 양자화 이내, 클래스 목록·로브 타일 CPU와 일치, LEAN 확대 분산이
  쌍선형 장 값과 8.7e-7, 전체 타일 발자국에서 맵 전체 분산과 6e-4. 디버그 레이어 오류 0.

## 2. 셰이딩 (설계서 2.11) — `Passes/Shading`

- 클래스별 커널(`ShadeOpaque`: Standard·Foliage, 모델 정의 전까지 Subsurface·Water도, `ShadeSky`), 재질 해석의 타일 목록으로
  ExecuteIndirect. 출력 OUTPUT0(PBR Neutral + sRGB OETF → RGB10A2), OUTPUT1(선형 × 노출: 검증·보조 뷰).
- **태양 원반**: 스페큘러를 원반 위에서 적분 — α ≥ 16θs 점 평가(최댓값 대비 ≤ 0.13%), 2~16θs 4점 원판 규칙(3차까지 정확, F만 중심,
  ≤ 0.25%), α < 2θs 로브 질량 중 원반 안 비율(등방화한 좌표의 타원, GGX 반경 CDF, 픽셀 각 발자국 포함, ≤ 0.65%), 명암 경계 띠는
  점별 cos 자르기 구적. 확산·Foliage 투과는 원반 위 잘린 cos의 정확 평균. 하늘의 원반은 반평면∩픽셀 정확 면적.
- GI는 R의 화면 프로브(근거리 가림 포함), 반사는 R의 G/M 결과 또는 K 경로 `screenProbeRadiance`, 대기는 S의 `atmosphereAerial`.
  f0 분리 스페큘러 방향 알베도(A, B; 모델 E 표와 같은 표본, A + B = E 8e-8).
- **정확성 [실측]**: 태양 원반 스페큘러 위 수치, 원반 픽셀 coverage 128×128 초표본 대비 0.29%, 총 선속 5.7e-5, 장면 끝-끝(CPU
  모델) 0.24%, 톤맵 출력 ±0.56 LSB. 디버그 레이어 오류 0.
- **아직 없음**: 국소광(S의 프록셀 리스트가 커밋됨 — 다음 작업, 면광원 LTC 적합 포함), 프록셀 산란 합성, 가장자리(E) 해석 coverage
  합성(V의 `Coverage.hlsli` 있음), coverage fragment 셰이딩(V의 대역 B 대기), Hair/Water/Glass/SSS 모델(INTERFACES 8.1 추가 필요).

## 3. 성능 [실측, city_block, 본 뷰; 평면 반사 뷰의 M 패스는 `.planar`로 따로]

| 구성 | 해상도 | 재질 해석 ms (설계) | 셰이딩 ms (설계) |
|---|---|---:|---:|
| V·M·S·R·C | 4K | 0.629 (0.40) | 2.73~2.94 (0.37~0.42) |
| V·M·S·R·C | 1440p | 0.272 (0.18) | 1.244 (0.18) |
| V·M·C (S·R 없음) | 4K | 0.63~0.68 | 0.385 |

- **셰이딩 초과의 원인(항 제외 분해, 4K)**: S `atmosphereAerial` ≈ 1.15 ms, R 프로브 조도 ≈ 0.57 ms, R 반사·K 경로 ≈ 1.15 ms,
  나머지 0.37 ms(설계 메모리 바닥 0.36). → 설계가 "프로브 4·프록셀"을 조회 몇 번으로 계상한 가정과 현재 공개 API 작업량의 불일치.
  요청 `20260925_M_shading_lookup_cost.md`(S·R). M 몫(태양 원반)은 48점 구적 → 4점·로브 질량으로 0.2 → 0.04 ms.
- **재질 해석 초과(0.63 vs 0.40)**: 텍스처 ≈ 0.13 + 노멀 모멘트 ≈ 0.13, 표면 재구성 ≈ 0.24, 기본(읽기·쓰기·분류) 0.14.
  설계는 16 B 압축 정점과 단순 삼선형 5회를 가정했다(실제 v1 정점 32 B, 스침각 비등방 16×). 구현 비효율: 같은 삼각형의 정점을 픽셀마다
  다시 변환 → 웨이브 협력 정점 처리로 줄이는 중.
- 평면 반사 뷰(R, 최대 4개)의 M 패스 4K 3.2 ms — R의 C_planar 예산 확인 중(조율 세션 경유).

## 4. 인터페이스 메모

- 코어 버그 보고(해결됨): `VisBuffer.hlsli`의 `triangle` 인자(HLSL 키워드) → v1.4에서 수정.
- v1.5: `VIS_NONE = 0`, vis id + 1. 가짜 V는 `packVisId`로 쓰고 0으로 클리어한다.
- 요청: `20260925_M_material_textures.md`(코어), `20260925_M_shading_lookup_cost.md`(S·R).
- 품질 키: `material.experiment_disable`, `shading.experiment_disable` — 비용 분해 전용(기본 0, 품질 해시에 들어간다).

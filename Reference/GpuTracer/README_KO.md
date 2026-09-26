# GPU 기준 경로추적기 (C, 2026-09-26)

조율 결정(사용자): 기준 영상 생성을 GPU로 옮긴다. 숲 4K 한 장이 CPU로 6~8시간이라 기준 대기가 일정의 병목이다. GPU판은 근사가 아니라 CPU판과 **같은 추정량**이다. 전환은 아래 검증 게이트를 통과한 뒤에만 한다.

## 1. 같은 추정량을 보장하는 방법

- **공용 코드** (`shared/*.hlsli`): 표본기(Owen-scrambled Sobol + PCG32), 재질 v1(평가, 로브 확률, VNDF·코사인 표본, pdf), 대기(계수, 위상, 위상 표본, τ_top 표 쌍삼차, Gauss-Legendre 적분, 구간 pdf), 태양 원뿔, 국소광(창, 중요도, 격자 선택, 표본, 교차)을 둔다. 같은 파일을 DXC(HLSL 2021)와 C++이 함께 컴파일한다(`Compat.hlsli`).
- **CPU 추정기는 바꾸지 않는다.** 바꾸면 기존 CPU 기준이 모두 무효가 된다. 대신 `unx_test_reference_gpu`가 공용 코드의 C++ 컴파일본을 CPU 함수와 함수별로 비교한다. [실측]
  - 표본기: 비트 단위로 같다.
  - 광원 선택·확률·표본: 같다.
  - 대기: 투과율 상대 오차 1e-5 이하, 구간 광학 깊이 2e-5.
  - BRDF: 상대 1.5e-3이다. 최악은 α = 1e-4 거울 봉우리로, float 대 CPU double 차이다.
  - 제외 영역: 행성 표면 아래에서 시작하는 20 km 이상 구간. 여기서는 두 구현 모두 1e20을 넘는 τ_top 두 값을 빼므로 값에 의미가 없다. 경로 루프는 이런 구간을 만들지 않는다.
- **경로 구조는 PathTracer.cpp를 한 줄씩 옮겼다**(`shaders/Common.hlsli`). 해당 요소: 표본 차원 순서, MIS, 강제 산란 NEE와 p_force, 러시안 룰렛, 차수 창, 태양 코스틱 경로 공간 분할(dropSun), 광원 추적 splat.
  - HLSL은 모든 호출을 인라인하므로 무거운 코드가 한 곳에만 있어야 DXIL이 200 KB 안에 든다(452 KB → 148 KB). 그래서 NEE 가시성·투과율 질의는 반복마다 큐에 모았다가 한 곳에서 푼다. 질의는 난수를 쓰지 않으므로 표본 차원 순서는 그대로다.
- **CPU가 double을 쓰는 곳은 같은 정밀도로 재정식했다.**
  - 고도: h = (|p|² + 2R·p_y)/(r + R).
  - 지평: b < −√q.
  - τ_top 표 좌표: d_top의 켤레 근. d_min = (R_t − R) − h.
  - 행성·대기 상한 구 교차: 2차식 계수를 double로 만들고(CPU와 같은 IEEE 연산) 켤레 근으로 푼다.
  - 태양 원뿔: sin(θ_s/2)는 CPU double 값을 쓴다. 표본의 sin·cos는 x = √u·sin(θ_s/2)에서 2x√(1−x²), 1 − 2x²로 구한다(삼각함수 없음).
  - log1p·expm1: 정확한 float 형식을 쓴다.
  - 발광 CDF: double이다.
- **누적**: 배치(dispatch 하나)별 float 합을 픽셀 double 누적에 더한다(DAdd). 코스틱 splat은 패스마다 float 버퍼에 CAS로 가산한 뒤 double 누적에 합친다.

## 2. 기하와 이식성

- 표준 D3D12만 쓴다: DXR 1.1 인라인 RayQuery(compute), SM 6.6 bindless, double 연산과 int64(`CheckFeatureSupport`로 확인하고, 없으면 실패). 벤더 전용 경로는 없다.
- 원본 기하만 쓴다. BLAS는 메시마다 하나이고 submesh마다 geometry 하나다(geometry 번호 = submesh 번호). TLAS는 장면 인스턴스 번호를 InstanceID로 쓰고, 마스크는 bit0 전체·bit1 그림자, 컬링은 없다. 렌더러의 프록시·LOD·브릭·RayScene은 쓰지 않는다.
- 알파 잎: 알파 재질을 쓰는 인스턴스가 있는 submesh는 non-opaque다. 후보마다 CPU와 같은 mip0 이중선형 표본(CPU가 복호한 텍셀을 그대로 올림)으로 판정한다.
- 변형 인스턴스(스킨, 바람): CPU와 같은 함수(`deformInstance`)로 world 복사본을 만들고, 상한도 같다.
- 데이터 의존 루프는 모두 상한과 오류 비트를 둔다(INTERFACES 3.6). 대상은 후보 루프 2^20, CAS 1024, CDF 탐색 64, 경로 4096이다. 비트가 서면 렌더가 실패한다.

## 3. 다른 세션과의 공존

- dispatch는 타임스탬프로 크기를 맞춰 약 25 ms로 한다. 배치는 약 250 ms이고, 배치마다 카운터·오류 비트를 읽는다.
- GPU 잠금: 15 s 조각이다(kind correctness, Track C, INTERFACES 3.3 v1.40 규약).
  - 대기자 파일을 쓰고, timing 대기자가 있으면 양보한다(얻은 직후 재확인 포함).
  - `.gpulock/HOLD`가 있으면 멈춘다.
  - history에 "slice k/N" 줄을 남긴다.
  - core의 `GpuLockSlice`가 들어오면 그것으로 바꾼다.
- 큐 우선순위는 NORMAL이다.
- VRAM: 장면·누적 버퍼 합과 프로세스 최대 사용량을 JSON에 적는다.

## 4. 캐시 정체성

GPU 영상의 키에는 `estimator.device = gpu`가 붙는다. 기본 render seed는 0x6E5EED로 CPU(0x5EED)와 독립이다. 따라서 GPU 영상은 CPU 캐시와 섞이지 않는다. 게이트를 통과하면 대기열을 GPU로 바꾸고, 그때부터 새 기준은 GPU 정체성으로 쌓인다.

## 5. 검증 게이트 (`unx_reference gatecompare`)

대상은 CPU 기준이 있는 장면이다: ridge_sunset 1440p(대기), forest_thin 1440p(알파 잎), city_block 1440p(코스틱, `--strip-dynamic`로 강체 이전 장면), interior(재질). 판정은 이렇다.
1. 16×16 타일 z(각 영상의 반쪽으로 잡음 추정)는 평균 0(표준오차 안), 편차 약 1이어야 한다.
2. 마스크별(하늘, 경계, 잎, 금속, 지면, 기타) 평균 편향 / 표준오차가 잡음 수준이어야 한다.
3. relMSE(CPU, GPU)는 (h_CPU + h_GPU)/4 수준이어야 한다.
4. furnace·에너지 시험이 CPU와 같아야 한다.

결과는 `Results/C/GpuTracer/`에 둔다.

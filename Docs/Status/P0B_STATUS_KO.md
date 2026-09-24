# P0b 진행 상황 (ARCHITECTURE_KO.md 7.1)

수치 표기는 설계서와 같다: [실측]은 이 기계에서 실제 실행한 값, [예상]은 비용식 값.

## 빌드·검사 명령

```text
powershell -File Tools/CI/Build.ps1                 구성 + 빌드 (CMake/Ninja, 커널 DXIL ≤ 200 KB 검사 포함), 증분 5~7 s [실측]
build/Release/bin/unx_unit_tests.exe                단위 테스트 10개 + 디버그 레이어 (≈1 s) [실측]
build/Release/bin/unx_gate_empty_frame.exe --validate      debug layer + GPU-based validation, 120패스 그래프 두 정책
build/Release/bin/unx_gate_empty_frame.exe                 빈 프레임 게이트 4K·1440p (≈25 s)
build/Release/bin/unx_gate_empty_frame.exe --experiments   제출 구조 비용 분해
```

## 7.1 항목

| 항목 | 상태 | 근거 |
|---|---|---|
| 1. 저장소·빌드·CI, 마이크로벤치 재확인 ±5% | 완료 | 의존성 공식 배포본(Agility NuGet 1.618.5, NVAPI R590 서브모듈). 재측정 중앙값은 P0a와 일치하거나 빠름. 느린 항목 하나(얇은 식생 GI 광선 1.19 G/s)는 설계에 반영(1.4) |
| 2. Native/Render (디바이스, 큐, bindless, render graph, 타임스탬프 하네스, 품질 설정) | 완료 | enhanced barriers, 트랜지언트 aliasing(placed resources), 패스 컬링, 계획 캐시, 큐 동기(선택), 패스당 타임스탬프 1개, 4K/1440p 외 해상도 거부, 품질 SHA-256·빌드 identity·클럭·큐 우선순위를 모든 보고에 기록 |
| 3. 빈 프레임 게이트 (4K ≤ 0.2 ms, 재현성 ±2%) | **통과** | 4K 0.1711 ms, 5회 편차 0.43%, 1440p 0.1638 ms [실측, `Results/Gates/EmptyFrame/`] |
| 4. 기준 경로추적기 + 지표 + 테스트 장면 1 | 남음 | |
| 5. Unity 디바이스 경계 측정 | 남음 | |
| 6. OMM NVAPI 경로 | 부분 | 디바이스가 NVAPI OMM cap 1, SER cap 1을 확인·보고. OMM 빌드 경로는 P2에서 광선 경로와 함께 |

## 게이트 3에서 바뀐 설계 가정

- 처음 구현(설계서 4.1의 컴퓨트 큐 배치 그대로)은 4K 0.333 ms로 실패했다. 원인 분해 [실측]: 패스 자체는 0.85~1.1 µs로 설계 가정과 같았고, 큐 간 동기 3회(62~79 µs씩)와 ExecuteCommandLists 11회(~15 µs씩)가 나머지였다. → 설계 가정(큐 경계 비용 미계상)과 구현(동기마다 리스트 분할)이 함께 원인. 프레임 안 async compute를 끄고 큐 하나·제출 1회로 바꿨다(설계서 1.3-7, 4.1, 4.3).
- 타임스탬프를 패스 경계당 1개로 줄였고(쌍 0.26 µs/패스 → 0.11), 재질·셰이딩 클래스별 패스처럼 겹치지 않는 타일에 쓰는 패스 사이의 불필요한 UAV 배리어를 없앴다(`Use::UavComputeDisjoint`). 작업량은 바꾸지 않았다.
- 빈 프레임의 VSM clear는 페이지 1개만 지운다. 64페이지 clear(16 µs)는 VSM 작업이고 설계서 2.3(페이지 관리·clear 0.04 ms)에 이미 계상돼 있다.
- 다른 GPU 앱의 시분할이 NORMAL 우선순위에서 프레임 ~5%에 0.16~0.47 ms를 더한다. 큐 우선순위 HIGH가 기본값이다.

## P1로 넘기는 관찰

- VSM 물리 페이지 풀(8192×6144 D32)을 depth로 그리고 SRV로 읽는 레이아웃 전환은 이번 측정에서 이상치의 원인이 아니었지만(이상치는 무작위 패스에 떨어짐), 풀 전체 크기에 비례할 수 있는 전환 비용은 P1 VSM 설계에서 실측으로 확인한다.
- 렌더 그래프 CPU 비용 0.15 ms/프레임(선언 + 기록 + 제출, 120패스) [실측]. 5.1의 렌더 제출 1 ms 할당 안이다.

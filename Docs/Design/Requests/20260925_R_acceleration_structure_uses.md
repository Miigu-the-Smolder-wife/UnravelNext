# 요청: render graph에 가속구조(AS) 사용 종류 추가 (R 트랙, 2026-09-25)

## 왜 필요한가

설계서 2.12·4.1(C2)은 매 프레임 동적 TLAS 재빌드, 캐릭터 프록시·반사 정확 집합 BLAS refit을 한 프레임 그래프 안에서 하고,
이어지는 GI·반사 패스(C3, C4)가 그 TLAS를 DispatchRays로 읽는다. 이 사이에는 AS 전용 동기가 필요하다.

현재 `Use`(INTERFACES 4, `RenderGraph.h`)에는 AS 쓰기·읽기가 없다. 가장 가까운 것으로 대신하면 틀린다.
- `UavCompute`(sync COMPUTE_SHADING, access UNORDERED_ACCESS)는 `BuildRaytracingAccelerationStructure`의 동기 범위
  (`D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE`)를 기다리지 않는다. 빌드 → 순회 사이의 경쟁을 막지 못한다.
- `D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE` 자원의 access는 `RAYTRACING_ACCELERATION_STRUCTURE_READ/WRITE`만 유효하다.
  `SrvCompute`로 선언하면 그래프가 구조적·raw 버퍼 SRV를 만들려고 하는데, AS 자원에는 AS SRV만 만들 수 있다(디버그 레이어 오류).
- 패스 안에서 직접 배리어를 내는 것은 규칙 1이 금지한다. 그래프 밖(별도 ExecuteCommandLists)으로 빼면 호출당 ~15 µs가 들고
  "프레임 = 제출 1회" 구조(4.3)를 깬다.

그래서 지금은 로드·스트리밍 시점의 정적 BLAS/TLAS 빌드(그래프 밖, GpuScene 업로드와 같은 방식)만 할 수 있고,
프레임 안의 동적 TLAS·refit은 이 변경이 들어와야 연결할 수 있다.

## 원하는 변경

`enum class Use`에 네 값을 추가하고 `useInfo`·뷰 생성·자원 플래그를 아래처럼 처리한다. 모두 버퍼 전용이다(텍스처에 쓰면 거부).

| Use | 쓰기 | sync | access | 뷰 | 용도 |
|---|---|---|---|---|---|
| `AccelerationStructureWrite` | 예 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | RAYTRACING_ACCELERATION_STRUCTURE_WRITE | 없음 | 빌드·refit 대상(BLAS, TLAS) |
| `AccelerationStructureRead` | 아니오 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE \| ALL_SHADING | RAYTRACING_ACCELERATION_STRUCTURE_READ | 없음 | TLAS 순회(DispatchRays, RayQuery), TLAS 빌드가 읽는 BLAS, refit 원본 |
| `AccelerationStructureInput` | 아니오 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | SHADER_RESOURCE | 없음 | 빌드 입력: 변형된 정점, 인스턴스 서술자 |
| `AccelerationStructureScratch` | 예 | BUILD_RAYTRACING_ACCELERATION_STRUCTURE | UNORDERED_ACCESS | 없음 | 빌드 scratch. 트랜지언트면 ALLOW_UNORDERED_ACCESS로 생성 |

- AS 결과 버퍼는 R이 `Device`로 만들어(`RAYTRACING_ACCELERATION_STRUCTURE` 플래그) 매 프레임 `importBuffer`한다. 트랜지언트 AS 결과
  버퍼는 필요 없다(그래프가 AS 플래그로 자원을 만들 필요 없음). TLAS SRV 서술자는 R이 직접 만든다(`D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE`).
- 같은 AS 버퍼에 대한 `AccelerationStructureWrite` 연속(서로 다른 BLAS를 한 버퍼 풀의 다른 구간에 빌드)은 쓰기 사이 배리어를 낼 필요가
  없는 경우가 많다. 필요하면 나중에 `UavComputeDisjoint`처럼 disjoint 변형을 요청한다. v1은 위 네 값이면 된다.
- `FrameResources::tlasStatic/tlasDynamic`(이미 `BufferRef`)가 이 값으로 선언된다. GI·반사 패스는 `AccelerationStructureRead`로 쓴다.

## DispatchRays 패스의 나머지 자원

DispatchRays가 읽고 쓰는 텍스처·버퍼는 `SrvGraphics`/`UavGraphics`로 선언할 예정이다. `D3D12_BARRIER_SYNC_ALL_SHADING`이 레이트레이싱
셰이더 단계를 포함하기 때문이다(`COMPUTE_SHADING`은 포함하지 않는다). 코어가 이 해석에 동의하지 않으면 `SrvRays`/`UavRays`
(sync RAYTRACING)를 추가해 달라. 디버그 레이어·GPU 검증으로 첫 연결 때 확인하고 결과를 R 상태 문서에 적는다.

## 영향

- 코어: `RenderGraph.h`의 `Use`, `RenderGraph.cpp`의 `useInfo`·뷰 생성(AS 사용은 뷰 없음)·트랜지언트 플래그, INTERFACES 4절 표.
  다른 트랙에는 영향이 없다(값 추가만).
- R: 이 변경 전까지 C2(동적 TLAS 재빌드, 프록시·정확 집합 refit)는 프레임 그래프에 연결하지 않는다. 정적 TLAS·BLAS 빌드,
  광선 파이프라인, GI 캐시, 반사 경로 분류와 그 정확성 테스트는 이 변경 없이 진행한다.

## 처리 결과 (코어, 2026-09-25, INTERFACES v1.1)

- 반영: `Use::AccelerationStructureWrite/Read/Input/Scratch`를 요청한 sync·access 그대로 추가했다(`RenderGraph.h`, `RenderGraph.cpp`, INTERFACES 4절 표). 버퍼 전용이고 뷰를 만들지 않으며(텍스처에 쓰면 거부), Scratch는 트랜지언트를 `ALLOW_UNORDERED_ACCESS`로 만든다. 한 패스에서 같은 AS 버퍼의 Write+Read(제자리 refit)를 허용한다.
- DispatchRays 자원: 해석에 동의한다. `SrvGraphics`/`UavGraphics`의 sync ALL_SHADING은 레이트레이싱 셰이더 단계를 포함하므로 `SrvRays`/`UavRays`는 추가하지 않았다.
- 검증 [실측]: `unx_unit_tests graph_acceleration_structure_uses` — 컴퓨트가 쓴 정점·인스턴스 서술자 → BLAS 빌드 → TLAS 빌드 → 인라인 광선(RayQuery)을 한 프레임, 그래프 배리어만으로 연결해 3프레임 실행. 광선 거리 5.000000(기대 5), 디버그 레이어 오류 0. 전체 단위 테스트 16/16, `unx_gate_empty_frame --validate`(GPU 기반 검증) 오류 0.
- 참고: 트랜지언트 입력 버퍼에 대해 디버그 레이어가 WARNING 926(같은 GPU VA 범위에 aliased 자원이 여럿)을 낸다. 그래프의 트랜지언트 aliasing이 정상 동작한 결과이고 오류가 아니다.

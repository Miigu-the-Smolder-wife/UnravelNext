# FX 트랙 상태 (GPU 입자 모듈 V1) — 2026-09-25

소유: `Native/Render/Passes/FX`, `Config/quality/fx.toml`, `Results/FX`.
표기: **[실측]** = 실행 결과(성능 수치는 GPU 잠금 아래), **[예상]** = 측정 전 계산값.
기준 문서: `C:\Users\USER\Unravel\Docs\Rebuild\WORLD_VFX_DESIGN_KO.md` 3·7(V1)·8절, ARCHITECTURE 2.9·2.14·4.1(C0).

## 1. 구조

- 입력은 VFX 세션의 스트림 계약(`NativeVfxStream.h`, 공유 수식 `VfxParticleMath.hlsli`, CPU 참조 `VfxStreamCpu.h`)이다. `Stream/`에
  NativeVfx 커밋 9f982941의 바이트 동일 사본을 두고, 테스트가 SHA-256으로 확인한다. CPU가 생사·사건 슬롯·자식·개수·용량을 정하고 GPU는
  연속 상태만 채운다.
- `unx::fx::ParticleSystem`(TrackState `fx.particles`): 호스트가 `submit(packet)`, C0 진입점 `tracks::simulation`이 쌓인 tick을 기록,
  `readback(stream, generation, tick)`(사건·카운터 링), `checkpoint()`(명시 되읽기).
- tick 파이프라인(패스 이름 `fx.particles.*`): upload → begin(RESET: 복원 레코드) → [용량 변경 시 repack] → surfaces(몸체 프레임) →
  grid(clear·count·scan·fill) → spawn.d0 → integrate.d0 → (깊이 1~4: child → spawn → integrate) → compact(scan·sums·scatter) →
  outputs(ribbon 점·strip, 매질 셀) → sort(hist·scan·scatter × 3) → readback.
- 결정성: 슬롯 = dead 목록의 순위(원자 연산 없음), 압축 = 접두합, 정렬 = 안정 LSD. 원자 연산은 충돌 사건 append·격자 채우기(순서 무관하게
  결과가 같음)뿐이다.
- 이식성(INTERFACES 2.6): 64비트 정수·double 없음. 정렬 순위는 wave 연산 없이 묶음 바이트 비교, 압축의 wave 폭 가정 없음(WARP 4-lane
  wave에서 검증).
- 안전: 모든 루프는 패킷 개수로 묶이고 모든 인덱스를 검사한다. 범위 밖 = `NV_STREAM_STATUS_RANGE`(16), 열거 상한 도달 =
  `FX_STATUS_WATCHDOG`(32, 모듈 비트) — 절단이 아니라 실패 기록이다.

## 2. 정확성 [실측]

| 실행 | 결과 |
|---|---|
| RTX 4080 + 디버그 레이어 + GBV, 8,192개 × 70 tick (GpuLock) | PASS: 오류 0, status 0, 충돌 사건 66건(CPU와 개수 차 0), 위치 오차 최대 6.4e-6 / p99 1.05e-6 |
| WARP, 같은 실행 | PASS: 위치 오차 최대 6.4e-6 / p99 1.2e-6 |
| RTX 4080, 524,288개, tick 100까지(중단) | status 0, 위치 오차 p99 1.3e-6, **최대 7.5e-5(충돌 입자) — 게이트 1e-5 초과** |

- 최대 오차의 원인: 공유 수식의 충돌 시험이 anchor 공간 좌표로 돌아 정밀도가 이미터 원점이 아니라 anchor에서의 거리에 비례한다(설계서
  3.1 정밀도 조건 위반). 원점 공간 시험으로 고치도록 VFX 세션에 제안했다(공유 수식이라 CPU·GPU가 같이 바뀐다). 허용 오차는 넓히지 않는다.
- 찾아서 고친 공유 수식 결함: box 추첨의 인자 평가 순서(C++ 미정), CpuExecutor의 noise_seed 누락, 충돌의 anchor 왕복 반올림.

## 3. 사고 기록

- 14:18 TDR(DXGI_ERROR_DEVICE_HUNG, 잠금 없는 정확성 실행, 첫 충돌 tick). 원인: 코어 렌더 그래프의 가져온 자원 뷰 캐시(포인터 재사용 시
  옛 서술자). 코어 4532054에서 고쳐졌고, 되돌린 모듈(우회 없음)로 GBV 실행 통과. 임시 규칙: 하드웨어 GPU 실행은 모두 GpuLock -Track FX 안.
- 긴 CPU 참조를 잠금 안에서 계산해 다른 트랙을 막았다 → `--record`(잠금 안, GPU만) / `--replay`(잠금 밖, CPU 참조)로 나눴다.

## 4. 남은 일

- V1 게이트: 524k·600 tick 참조 대조(충돌 정밀도 수정 뒤), 결정성 두 번 실행, WARP 전체, GpuLock 시간 측정 ≤ 0.2 ms/tick(`unx_gate_fx_particlegate`,
  core·features 부하, 4K·1440p).
- 요청: `FrameResources::particles`(20260925_FX_simulation_slot.md), 렌더 레코드·보간·셀 키(20260925_FX_particle_render_rules.md, 렌더 패스는
  조율 세션의 비용식·품질 정의 뒤).
- 이후: FX 렌더 패스, 물·볼륨·SSS·헤어(ARCHITECTURE 해당 절; 비용식·품질 정의 없는 항목은 조율 세션에 먼저).

# FX 트랙 상태 (GPU 입자 모듈 V1) — 2026-09-25

소유: `Native/Render/Passes/FX`, `Config/quality/fx.toml`, `Results/FX`, 요청 `Docs/Design/Requests/20260925_FX_*`.
표기: **[실측]** = 실행 결과(성능 수치는 GPU 잠금 아래, RTX 4080), **[계산]** = 측정 전 계산값, **[추론]** = 실측에서 끌어낸 원인 해석.
기준 문서: `C:\Users\USER\Unravel\Docs\Rebuild\WORLD_VFX_DESIGN_KO.md` 3·7(V1)·8절, ARCHITECTURE 2.9·2.14·4.1(C0).

## 1. 구조

- 입력은 VFX 세션의 스트림 계약(`NativeVfxStream.h`, 공유 수식 `VfxParticleMath.hlsli`, CPU 참조 `VfxStreamCpu.h`)이다. `Stream/`에
  NativeVfx 커밋 f461b7c9의 바이트 동일 사본을 두고, 테스트가 SHA-256으로 확인한다. CPU가 생사·사건 슬롯·자식·개수·용량을 정하고 GPU는
  연속 상태만 채운다.
- `unx::fx::ParticleSystem`(TrackState `fx.particles`): 호스트가 `submit(packet)`, C0 진입점 `tracks::simulation`이 쌓인 tick을 기록,
  `readback(stream, generation, tick)`(사건·카운터 링), `checkpoint()`, `readState(name)`(진단).
- tick 파이프라인(23 패스, core 부하): upload → emitters(델타 블록 → 상주 표) → begin(보내지 않은 행의 tick 필드 지움, 격자·정렬
  히스토그램 비움; RESET: 복원) → [용량 변경 시 repack] → surfaces(몸체 프레임 + 격자 계수) → grid(scan, fill) → spawn.d0 →
  integrate.d0 → (깊이 1~4: child → spawn → integrate) → compact(scan·sums·scatter + 정렬 1패스 히스토그램) → cells(셀당 스레드, 그룹
  단위 연속 쓰기) → ribbon(범위당 그룹 1개, 한 디스패치) → sort(scan·scatter × 3, scatter가 다음 패스 히스토그램을 셈) → readback.
- 상태는 tick 패리티 두 벌(`posAge[2]`, `velocity[2]`): integrate가 지난 tick 출력을 읽어 이번 tick 출력을 쓴다. 렌더러는 두 벌을
  그대로 보간한다(별도 렌더 레코드 없음, 요청 20260925_FX_particle_render_rules.md 개정).
- 이미터 표 델타(`NV_STREAM_EMITTER_DELTA`): GPU 상주 표 + 갱신 산포 + tick 필드 지움. CPU 거울은 O(블록 + tick 필드가 있던 행).
- 충돌 후보: 표면 = 기준점 + 오프셋(VFX 768e7e16). 움직이는 표면의 상자는 θr + (1+θ)u /(1−θ), 질의 상자는
  ((1+θmax)Dmax + θmax|d|)/(1−θmax)만큼 키운다(유도는 `Particles.hlsli`, VFX가 `NativeVfxStream.h` e절에 채택). 후보는 격자 셀 + 연속
  상자 겹침 시험.
- 결정성: 슬롯 = dead 목록의 순위, 압축 = 접두합, 정렬 = 안정 LSD(그룹 안 순서 먼저, 자릿수 구간 연속 쓰기). 원자 연산은 순서와
  무관한 결과(충돌 사건 append, 격자 채우기·계수, 정렬 히스토그램 계수, 운동 최대값)에만 쓴다.
- 이식성: 64비트 정수·double 없음, wave 폭 가정 없음(WARP 4-lane wave에서 검증), WaveMatch(SM 6.5 표준).
- 상태 패킷(dt = 0): 표면·격자를 만들지 않고 몸체 인덱스를 검사·해석하지 않는다(VFX fa5821b5 계약).

## 2. 정확성 [실측]

| 실행 | 결과 |
|---|---|
| WARP, 8,192 루트 × 200 tick, 참조 대조(f461b7c9) | PASS: 위치 오차 최대 3.05e-6 / p99 9.6e-7, 결정성 비트 동일, 충돌 수 GPU = 참조 |
| WARP, 256 ribbon × 601점(3 청크) | 링크 정확, 꼭짓점/uv 오차 1.26e-6 (순차 double 보행 대비) |
| RTX, 전체 부하 2400 tick 기록(fa5821b5: 524k 루트 + ribbon 256 + 볼륨 16, 깊이 4, 23.5k 행) | 구조·출력 검사 전부, 결정성 40 비교 tick 비트 동일, IMPACT_OVERFLOW 9 tick(한 지점) → VFX f461b7c9가 해결 |

- 찾아서 VFX와 고친 결함: 표면이 float anchor 공간 절대점이라 정밀도가 anchor 거리에 비례(100 m에서 1.4 ulp, 반례: anchor를 36 m
  옮기면 오차 18배 감소) → 표면 = 기준점 + 오프셋. 출생 원뿔 표본의 1 − z² 상쇄 → w(2 − w). 움직이는 몸체 사이 끼임(쐐기·닫히는 틈)
  → 마주 보는 표면 극한 규칙.
- ribbon 끊김 시험(|구간| ≤ break)은 float 길이의 문턱이다. 한계에서 1e-6 이내인 구간은 GPU의 결정을 따르고 센다(2400 tick 중 2
  tick). 그 밖은 모두 정확 비교.
- 결정성 해시는 정의된 상태만 덮는다(살아 있는 슬롯의 두 tick 상태, 목록의 개수까지). dead 목록의 오래된 꼬리가 실행마다 달랐다(새
  버퍼가 받은 메모리, 읽히지 않음).

## 3. 성능 [실측, GpuLock]

| 부하 | 4K ms/tick | 비고 |
|---|---|---|
| core 524,288(이전 고정물: ribbon·볼륨이 루트 16개에 섞임) | 0.651 → 0.514 → 0.506 | 셀 쓰기 정렬(168 → 29 µs), 상태 두 벌 |
| core RPP 모양 622,592(루트 524k + ribbon 256×256 + 볼륨 16×2048, 셀 262k) | 0.560 → 0.5625(격자 3패스) | 패스 타임스탬프 끔: 0.5586(계측 비용 없음) |

- 기여 분해(524k, 이전 빌드): integrate(충돌 제외) 약 0.10, 충돌 약 0.10, 정렬 0.145, 셀 0.029, ribbon 0.081(→ 한 디스패치로 바꿈).
- 대역폭 바닥[계산, 약 650 GB/s]: integrate 슬롯당 80 B(약 77 µs), 정렬 3패스 × 16 B/키(약 45 µs), 셀 29, 압축 약 15, 나머지 약 50 →
  약 0.22 ms. 0.2 ms 게이트는 지금의 tick당 데이터량으로는 대역폭 바닥 아래다. 커널 조정만으로는 닫히지 않고 데이터량(정렬 패스·키
  폭, 슬롯 상태 이동량)을 줄여야 한다 → 조율 세션에 보고.
- 델타 패킷: features 7.83 → 1.49 MB/tick(23.5k 행 중 3.8k 블록). 남은 1.28 MB는 tick 필드만 바뀌는 블록이다 → 32 B tick 레코드 제안
  예정(업로드 시간 실측 뒤).
- features 부하의 닫힌 고리 측정은 대리 권위(고정물)의 CPU가 GPU를 쉬게 해 클럭이 떨어진다(무효로 표시). `--packets DIR`로 기록된
  스트림을 열린 고리로 재생해 GPU만 잰다(결정성 덕분에 기록된 패킷이 이번 실행의 사건과 맞는다).

## 4. 사고 기록

- 14:18 TDR(DXGI_ERROR_DEVICE_HUNG, 잠금 없는 정확성 실행). 원인: 코어 렌더 그래프의 뷰 캐시. 코어 4532054에서 고쳐졌고, 우회 없이 통과.
- 긴 CPU 참조를 잠금 안에서 계산해 다른 트랙을 막았다 → `--record`(잠금 안, GPU만) / `--replay`(잠금 밖).
- 게이트 CPU 프레임이 고정물 비용(23k 행에서 선형 행 탐색·스폰 오프셋 이차 합)으로 20 ms → 클럭 570 MHz 아래 → 무효 표시, 고정물을
  O(변경)으로 고침(패킷 바이트 동일 확인).

## 5. 남은 일

- V1 게이트: f461b7c9 기록 600 tick CPU 재생(참조 1e-5), WARP 전체, ≤ 0.2 ms/tick(위 대역폭 바닥 문제 → 조율 결정).
- 성능: 정렬(히스토그램 융합 측정 중), integrate(살아 있는 목록 순회, 충돌 후보 비용), 업로드 tick 레코드(VFX 형식).
- 요청: `FrameResources::particles`(20260925_FX_simulation_slot.md), 렌더 규칙·셀 키(20260925_FX_particle_render_rules.md, 렌더 패스는
  조율 세션의 비용식·품질 정의 뒤).
- 이후: FX 렌더 패스, 물·볼륨·SSS·헤어(비용식·품질 정의 없는 항목은 조율 세션에 먼저).

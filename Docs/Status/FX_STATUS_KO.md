# FX 트랙 상태 (GPU 입자 모듈 V1) — 2026-09-25

소유: `Native/Render/Passes/FX`, `Config/quality/fx.toml`, `Results/FX`.
표기: **[실측]** = 실행 결과(성능 수치는 GPU 잠금 아래), **[예상]** = 측정 전 계산값, **[추론]** = 실측에서 끌어낸 원인 해석.
기준 문서: `C:\Users\USER\Unravel\Docs\Rebuild\WORLD_VFX_DESIGN_KO.md` 3·7(V1)·8절, ARCHITECTURE 2.9·2.14·4.1(C0).

## 1. 구조

- 입력은 VFX 세션의 스트림 계약(`NativeVfxStream.h`, 공유 수식 `VfxParticleMath.hlsli`, CPU 참조 `VfxStreamCpu.h`)이다. `Stream/`에
  NativeVfx 커밋 f2cdf56a의 바이트 동일 사본을 두고, 테스트가 SHA-256으로 확인한다. CPU가 생사·사건 슬롯·자식·개수·용량을 정하고 GPU는
  연속 상태만 채운다.
- `unx::fx::ParticleSystem`(TrackState `fx.particles`): 호스트가 `submit(packet)`, C0 진입점 `tracks::simulation`이 쌓인 tick을 기록,
  `readback(stream, generation, tick)`(사건·카운터 링), `checkpoint()`(명시 되읽기).
- tick 파이프라인(패스 이름 `fx.particles.*`): upload → emitters(델타 블록 → 상주 표) → begin(보내지 않은 행의 tick 필드 지움; RESET: 복원
  레코드) → [용량 변경 시 repack] → surfaces(몸체 프레임) → grid(clear·count·scan·blocks·fill) → spawn.d0 → integrate.d0 → (깊이 1~4:
  child → spawn → integrate) → compact(scan·sums·scatter) → cells(셀당 스레드) → ribbon s0~s7(병렬 구간 스캔) → sort(hist·scan·scatter × 3)
  → readback.
- 이미터 표 델타(`NV_STREAM_EMITTER_DELTA`, VFX 4cc29b49): GPU 상주 표 + `FxEmitters`(블록 → 행, 도장 = 패킷 일련번호) + `FxBegin`이
  도장이 없는 행의 tick 필드(rebase, TRANSPORT·SOURCE·KILLED, 부모 사건)를 지운다. CPU 거울은 O(블록 + tick 필드가 있던 행)이고,
  ribbon·volume 행 목록을 증분으로 유지한다. 표가 커지면 옛 표를 복사한다.
- 결정성: 슬롯 = dead 목록의 순위(원자 연산 없음), 압축 = 접두합, 정렬 = 안정 LSD(그룹 안 순서를 group memory에 먼저 만들고 자릿수
  구간을 연속으로 쓴다). 원자 연산은 충돌 사건 append·격자 채우기·격자 운동 최대값(순서 무관하게 결과가 같음)뿐이다.
- 이식성(INTERFACES 2.6): 64비트 정수·double 없음. wave 폭 가정 없음(WARP 4-lane wave에서 검증). WaveMatch는 SM 6.5 표준 연산이다.
- 안전: 모든 루프는 패킷 개수로 묶이고 모든 인덱스를 검사한다. 범위 밖 = `NV_STREAM_STATUS_RANGE`(16), 열거 상한 도달 =
  `FX_STATUS_WATCHDOG`(32, 모듈 비트) — 절단이 아니라 실패 기록이다.
- 충돌 후보(움직이는 표면, VFX e0d7bcea 이후 표면마다 자기 틀에서 쓸기): 표면 상자를 θr + (1+θ)u /(1−θ)로, 질의 상자를
  ((1+θmax)Dmax + θmax|d|)/(1−θmax)로 키운다(θ = |ω|h, u = |v|h, D = 운반 표면의 되돌림 변위 상한; 유도는 `Particles.hlsli`, VFX가
  `NativeVfxStream.h` e절에 채택). θ ≥ 1/2이면 large 목록. 후보는 격자 셀 + 연속 상자 겹침 시험으로 거른다(같은 상한, 정확성 불변).

## 2. 정확성 [실측]

| 실행 | 결과 |
|---|---|
| WARP, 8,192개 × 200 tick, 델타·새 정렬·셀당 스레드, 참조 대조 끔 | PASS: 구조 검사·출력 검사 전부, 결정성 비트 동일, 자식 1,596행, 깊이 4, 충돌 수 GPU = 참조 |
| RTX 4080, 같은 실행(잠금, 새 커널 첫 실행) | PASS: 결정성 비트 동일 |
| WARP·RTX, 8,192개 × 60 tick, 참조 대조(f2cdf56a) | FAIL: 입자 (96,18) 위치 오차 3.7e-5(게이트 1e-5) |
| 같은 실행, anchor를 그 몸체 곁으로 36 m 옮김(세계 고정, 진단) | PASS: 같은 입자 2.0e-6 |
| RTX, 524k × 300 tick 기록(f2cdf56a) | 충돌 사건 270,718(개수 차 0), IMPACT_OVERFLOW 7 tick — VFX에 기록 전달(`Cache/FX/overflow_f2cdf56a`) |
| RTX, 524k × 600 tick 기록 → CPU 재생(4cc29b49 수식, 잠금 밖) | 결정성 비트 동일; 1e-5 초과 44개, 최대 0.021(당시 수식의 적중/빗나감 갈림) |

- 남은 오차의 원인 [실측으로 확인]: tick 표면이 float anchor 공간 절대점이다. GPU 몸체 표면 꼭짓점은 double 변환 대비 1.4 ulp(|p|)
  (약 100 m에서 1.1e-5 m)이고, 0.3 m 구의 법선을 약 1e-5 기울여 튐 속도에 5e-5 m/s를 남긴다. anchor를 옮기면 오차가 18배 준다. 정밀도가
  anchor 거리에 비례하므로(1 km에서 약 6e-5 m) 품질 결함이다. 표면을 자기 기준점(질량 중심) 상대 오프셋으로 싣는 표현을 VFX에 제안했다
  (공유 수식·스트림 계약 변경). 허용 오차는 넓히지 않는다.
- 해결된 공유 수식 결함(VFX): box 추첨 순서, noise_seed, 충돌 anchor 왕복, 움직이는 몸체 관통(e0d7bcea), 충돌 float 조건수(f2cdf56a,
  내 보고 계기).

## 3. 성능 [실측, RTX 4080, GpuLock]

| 부하 | 4K | 1440p | 비고 |
|---|---|---|---|
| core(524,288, 128행) | 0.682 ms/tick | 0.683 | integrate 0.199, cells 0.153, sort scatter 0.105 |
| features(545k, 23,224행, 깊이 4, 델타) | 1.098 | 1.308 | 픽스처 CPU 4 ms/tick이 남아 클럭 중앙값 2565 MHz(최소 975) |

- 델타 패킷: features 부하 7.83 MB → 1.38 MB/tick(23,224행 중 3,540 블록). 델타 전 A/B의 GPU 업로드 시간은 픽스처 CPU(20 ms/tick)로
  클럭이 떨어져 무효; 픽스처의 이차 비용을 고친 뒤 다시 잰다.
- 게이트(≤ 0.2 ms/tick)까지 3.4배. 항목별 기여는 `fx.particles.experiment_disable`(시간 측정 전용) 비트로 분해한다.

## 4. 사고 기록

- 14:18 TDR(DXGI_ERROR_DEVICE_HUNG, 잠금 없는 정확성 실행, 첫 충돌 tick). 원인: 코어 렌더 그래프의 가져온 자원 뷰 캐시(포인터 재사용 시
  옛 서술자). 코어 4532054에서 고쳐졌고, 되돌린 모듈(우회 없음)로 GBV 실행 통과.
- 긴 CPU 참조를 잠금 안에서 계산해 다른 트랙을 막았다 → `--record`(잠금 안, GPU만) / `--replay`(잠금 밖, CPU 참조)로 나눴다.
- 게이트 CPU 프레임이 픽스처 비용(23k행에서 선형 행 탐색·스폰 오프셋 이차 합)으로 20 ms가 되어 클럭이 570 MHz 아래로 떨어졌다 → 그
  측정은 무효로 표시하고 픽스처를 O(변경)으로 고쳤다.

## 5. 남은 일

- V1 게이트: 표면 상대 표현(VFX) 뒤 524k·600 tick 참조 대조, WARP 전체, GpuLock ≤ 0.2 ms/tick.
- 성능: 기여 분해 → integrate(렌더 레코드 중복 쓰기 32 B/슬롯 제거 검토, 충돌 후보 비용), cells, 정렬, 디스패치 수 축소.
- 요청: `FrameResources::particles`(20260925_FX_simulation_slot.md), 렌더 레코드·보간·셀 키(20260925_FX_particle_render_rules.md, 렌더 패스는
  조율 세션의 비용식·품질 정의 뒤).
- 이후: FX 렌더 패스, 물·볼륨·SSS·헤어(ARCHITECTURE 해당 절; 비용식·품질 정의 없는 항목은 조율 세션에 먼저).

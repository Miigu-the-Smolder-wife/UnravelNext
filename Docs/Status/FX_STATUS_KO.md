# FX 트랙 상태 (GPU 입자 모듈 V1) — 2026-09-26

소유: `Native/Render/Passes/FX`, `Config/quality/fx.toml`, `Results/FX`, 요청 `Docs/Design/Requests/20260925_FX_*`.
표기: **[실측]** = 실행 결과(성능 수치는 GPU 잠금 아래, RTX 4080), **[계산]** = 측정 전 계산값, **[추정]** = 실측에서 끌어낸 원인 해석.
기준 문서: `C:\Users\USER\Unravel\Docs\Rebuild\WORLD_VFX_DESIGN_KO.md` 3·7(V1)·8절, ARCHITECTURE 2.9·2.14·4.1(C0), COVERAGE_REDESIGN 14.8.

## 1. 구조 (f13a50d 이후)

- 입력은 VFX 세션의 스트림 계약이다(`NativeVfxStream.h`, 공유 수식 `VfxParticleMath.hlsli`, CPU 참조 `VfxStreamCpu.h`). `Stream/`에
  NativeVfx 2cbfe994의 바이트 동일 사본을 두고, 테스트가 SHA-256으로 확인한다. CPU가 생사·사건 슬롯·자식·개수·용량을 정하고 GPU는
  연속 상태만 채운다.
- **입자 레이아웃(슬롯 없음).** 스트림 계약상 한 이미터 행의 산 출생은 정확히 [death_birth, next_birth) 연속 구간이다. 그래서 CPU가
  매 tick 스트림 표에서 레이아웃을 계산한다. 산 행마다 출생 순서로 [base, base + count)에 놓고, 볼륨 행을 첫 셀 순서로 앞에 둔다(볼륨
  레코드 색인 = 상태 색인). integrate는 이전 레이아웃(행 구간 + 256 스레드 그룹당 구간 색인)을 읽고, 입자를 새 레이아웃의
  base + (birth − first)에 쓴다. 이것으로 meta, 생존 플래그, 생존/죽음/dying 목록, 압축 3패스, repack, 깊이 ≥ 1 integrate 패스가
  없어졌다. 신생 입자는 spawn이 바로 적분한다. GPU가 쓴 입자 수를 세고, CPU가 alive_after와 대조해 보고를 만든다.
- tick 파이프라인(core 부하, 12패스): upload → emitters(블록·패치 → 상주 표) → begin(보내지 않은 행의 tick 필드 지움, RowMotion·dynamic,
  격자 비움; RESET이면 복원 기록을 입력 레이아웃에) → surfaces(몸체 프레임 + 격자 계수) → grid(scan, blocks, fill) → spawn.d0 ∥
  integrate.d0(서로 의존이 없어 장벽 없이 겹침) → (깊이 1~4: child → spawn) → child.keep → collide → ribbon → readback.
- 상태는 tick 패리티 두 벌(`posAge[2]`, `velocity[2]`)이고, 각 벌은 그 tick의 레이아웃을 따른다. 렌더러는 두 벌과 두 tick의 행 표로
  보간한다(요청 20260925_FX_particle_render_rules.md 1~3절, 2026-09-26 개정). 정렬은 렌더 패스가 프레임마다 타일 단위로 한다(14.8
  판정 1).
- 충돌: 적분의 운동 단계는 모든 입자에서 돈다. 충돌 프로그램의 입자는 큐(68 B, wave당 원자 1회)로 모으고 FxCollide가 스윕한다. 큐
  용량은 그 tick 충돌 행의 입자 수다(CPU 계산). 후보는 해시 격자(표면 상자를 운동 한계만큼 키움, 유도는 `Particles.hlsli`)에 연속
  상자 겹침 시험을 더한 것이다.
- 호스트 시뮬레이션 큐 진입점: `record(graph, shaders, tick, queue)`(WORLD_VFX 3.7). 링 슬롯의 펜스는 그 큐를 따른다.
- 결정성: 레이아웃과 출생 색인은 CPU가 계산한다. 원자 연산은 순서와 무관한 결과(충돌 사건 append, 충돌 큐, 격자 채우기·계수, 운동
  최대값, 입자 수)에만 쓴다.
- 이식성: 64비트 정수·double 없음, wave 폭 가정 없음(WARP 4-lane wave에서 검증). 데이터 의존 루프는 모두 하드 상한과 오류 비트를
  갖는다(INTERFACES 3.6: 후보 순회 WATCHDOG, 이진 탐색 guard 32).

## 2. 정확성 [실측]

| 실행 | 결과 |
|---|---|
| WARP, 8,192 루트 × 130 tick, 참조 대조(레이아웃 빌드 f13a50d) | PASS. 위치 오차 최대 2.9e-6 / p99 9.7e-7, 충돌 사건 수 GPU = 참조(909), 결정성 비트 동일, 용량 변경 14회(repack 없음) |
| RTX 첫 실행(잠금 안), 레이아웃 빌드 | PASS, 결정성 3 비교 tick 비트 동일 |
| RTX 전체 부하 2400 tick 기록 rec_T → CPU 재생(참조, 잠금 밖) | 진행 중. tick 60 최대 6.6e-6. tick 180에 행 56의 입자 7개가 1e-5 초과(최대 1.94e-4, 속도 y 차 약 0.003 m/s) → 추적 예정 |
| WARP 전체 부하 200 tick(패치36 빌드) | tick 120 최대 1.68e-5, tick 131 사망 사건 (56,341) 1.32e-5 — 같은 행 56 [추정: 같은 기구] |

## 3. 성능 [실측, GpuLock, 4K core RPP: 622,592 live, 400행]

| 빌드 | ms/tick | 비고 |
|---|---|---|
| T(충돌 큐 + 필드 상수 버퍼) | 0.357, 0.353 | 2820 MHz |
| U(행별 RowMotion 112 B, aa91870) | 0.285, 0.291, 0.286, 0.295 | integrate 0.169 → 0.106~0.113 |
| L(행 레이아웃, f13a50d) | 0.244, 0.242 (2820 MHz), 1440p 0.240 | 압축 0, integrate 0.084~0.086, spawn 0.011(신생 적분 포함) |
| G(begin 융합 + 한 패스 격자, 9ea7583) | 0.256 | **후퇴 → 되돌림.** begin 28.8 µs(이진 탐색 의존 사슬), surfaces 22.7, collide 56 µs(버킷 넘침 사슬) |

- L 패스별(µs, 2820 MHz): upload 12.5, emitters 9.8, begin 10.0, surfaces 11.2, grid 4.2 + 3.9 + 11.2, spawn 11.2, integrate 83.7,
  collide 48.3, ribbon 26.6, readback 5.6. 충돌 큐 65,536, 격자 항목 약 28k, 대형 목록 2.
- recorded 부하(23,496행, 640k): L 0.246 [실측, 2820 MHz]. 이전의 0.57~0.77은 클럭이 떨어진 실행이라 무효다.
- 측정 중인 V 빌드: L에 다음을 더했다.
  - 충돌 큐 원자를 wave당 1회로;
  - spawn.d0과 integrate.d0을 장벽 없이 겹침;
  - 충돌 큐와 디스패치를 충돌 행 입자 수로;
  - 상태 쌍을 정확한 크기로.

## 4. 사고 기록

- 09-25 14:18 TDR. 원인은 코어 렌더 그래프의 뷰 캐시였고, 코어 4532054에서 고쳐졌다.
- 긴 CPU 참조를 잠금 안에서 계산해 다른 트랙을 막았다 → `--record`(잠금 안, GPU만) / `--replay`(잠금 밖, 장치 없음, --yield).
- 09-26: 잠금 없이 연달아 돌린 WARP 실행이 다른 세션의 timing에 `contended`로 잡혔다. PDH가 WARP 소프트웨어 어댑터의 엔진 시간을
  GPU 사용으로 세고, WARP는 CPU도 포화시킨다. 사용자 결정: 포화시키는 정확성 실행은 잠금(kind correctness) 안에서 돌리고, 시험 묶음은
  잠금 하나 안에서 돌린다.
- 917787d에 다른 트랙의 스테이징(Atmosphere 삭제)이 섞였다. 이후로는 `git commit --only -- <FX 경로>`로 커밋하고, 커밋 전에
  `git diff --cached --stat`으로 확인한다.

## 5. 남은 일

- V1 게이트 ≤ 0.2 ms/tick(L 0.242). 다음 항:
  - collide 48 µs(큐 65k의 후보 순회);
  - ribbon 27 µs;
  - 격자 30 µs(원자 계수 2회; 한 패스 실험은 후퇴했다);
  - upload/readback 18 µs(그래프가 아직 복사 큐를 지원하지 않는다 → 코어 요청 후보);
  - integrate 84 µs(필드 16개는 전역 지지라 걸러 낼 수 없다; 부하 정의).
- 행 56 재생 이상값 추적: 추적 기록을 남기고, 같은 입력을 double로 다시 계산해 분기 판정 차이인지 확인한 뒤 VFX에 보고한다.
- V3: 렌더에 보이는 tick 버퍼의 ≥ 3 링, 슬롯 상태기계(pending → claimed → submitted), 렌더 입력 행 표 업로드.
- 설계 세션의 VRAM 합계표에 입자 몫을 측정값으로 보고한다.

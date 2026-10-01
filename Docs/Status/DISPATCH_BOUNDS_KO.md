# 새 커널의 dispatch 구조 상한 감사 (2026-10-02, 작성 A — GPU 실행 없음)

조정 06:30 지시. 라운지 장치 제거(hang)가 표면 캐시 직접광의 그림자 광선에서 났고 원인이 아직 확정되지 않아, 2026-10-01~02에 들어온 새 패스 전체를 코드로 읽어 "dispatch 하나의 최악 작업"을 표로 닫아 둔다.

**읽은 커밋**: A `redesign-v2-fix` 05ff458 + 상한 수정(작업 트리에 있음, 잠금 안 첫 실행 대기 — 통과하면 다음 코드 커밋), R `origin/redesign-v2` 1c1c65a, S2 `origin/redesign-v2-refl` 21a3aac. 값은 전부 **코드에서 읽은 것**이고 잰 것이 아니다(잰 값은 따로 [실측] 표시). 해상도 의존 값은 1080p / 4K로 적는다.

**기준**: (1) DispatchRays·Dispatch 하나가 내보내는 논리 광선 수(스레드 수 × 스레드당 최대 광선; 광선 하나는 정적·동적 TLAS에 TraceRay 2회) ≤ 262,144. (2) 스레드당 루프는 엔진 상수로 묶일 것 — 장면 데이터(광원 수, 레코드 수, 후보 삼각형 수)에 따라 길이가 달라지는 루프는 표시. (3) 첫 프레임에 몰리는 항목 표시.

표기: **X** = 기준 위반(고침/알림), **D** = 데이터에 따라 길이가 달라지는 루프, **1F** = 첫 프레임 집중.

## 1. A의 패스

| 패스 | dispatch 단위 | 스레드당 광선 | dispatch당 최악 광선 | 스레드당 루프 | 판정 |
|---|---|---|---|---|---|
| `m.ml.tiles` | 다운샘플 타일(8×8) 그룹, 전화면 | 0 | 0 | 고정(64 화소) | 통과 |
| `m.ml.sample` | 나열된 다운샘플 타일당 그룹(간접) | 0 | 0 | **D**: 프록셀 목록의 광원 수만큼 후보 평가(면광원은 LTC 적분). 목록 길이는 구조 상한 없음(`atmosphere.froxels.lights_max` 32는 정렬된 머리일 뿐, 닿는 광원은 전부 뒤따름) | D — 옛 ShadeOpaque의 화소별 루프와 같은 목록·같은 평가. 3절 참조 |
| `m.ml.trace` | 표본 텍셀당 스레드 | 1 | **고치기 전 X**: 표본 텍스처 전체가 DispatchRays 하나 — 1080p 2,073,600 / 4K 8,294,400 광선. **고침**: 행 띠로 나눠 dispatch당 ≤ 262,144 (1080p 8개, 4K 32개 dispatch) | 없음 | 고침(다음 코드 커밋, 첫 실행 대기) |
| `m.ml.shade` (+ FULL1) | 클래스 타일 목록 / 전화면 8×8 | 0 | 0 | 화소의 표본 ≤ 4개(고정) | 통과 |
| `m.ml.sets`, `.filter`, `m.ml.temporal`, `m.ml.spatial` | 화소·타일당 | 0 | 0 | 고정(표본 4, 탭 5/12, 공간 탭 ≤ 8) | 통과 |
| `m.ml.cov.nearest` | V의 블록 인자(1024 레코드/그룹, 256 스레드 × 4) | 0 | 0 | 고정(4) + `coverageBlockTile` 이분 탐색 ≤ 32 | 통과 |
| `m.ml.cov.surface`, `m.ml.cov.shade` | 전화면 8×8 | 0 | 0 | 고정 | 통과 |
| `s.ml.volume` | 프록셀당 스레드(24 px 타일 × 64 슬라이스) | 표본 수(2) | **고치기 전 X**: 격자 전체가 하나 — 1080p 80×45×64×2 = 460,800 / 4K 1,843,200 광선. **고침**: 슬라이스 띠로 dispatch당 ≤ 262,144 (1080p 2개, 4K 8개) | **D**: 프록셀 목록 광원 수만큼 후보 가중(가벼운 식, 적분 없음) | 고침(다음 코드 커밋, 첫 실행 대기), D는 3절 |
| `r.gi.rc.clear / mark / reuse / allocate / select / traces / finish / validate / reset` | 칸(4³ 그룹)·슬롯(64)·1 스레드 | 0 | 0 | 고정(mark 8칸, select 16칸, finish 청크 수) | 통과 |
| `r.gi.rc.trace` | 프로브 텍셀당 스레드, 프로브 청크 | 3(자기 광선 + 히트에서 태양·국소광 그림자 광선) | **92b7c92 전 X**: 최대 1024 프로브 × 1024 = 1,048,576 스레드(광선 3,145,728). 92b7c92: 256 프로브(스레드 262,144, 광선 786,432). **다음 코드 커밋**: 광선 3개를 세어 85 프로브(스레드 87,040, 광선 261,120) | 히트의 `rtLocalLightChooseOriented`: **D**(광원 격자 칸의 광원 수, R의 헬퍼), `rtHitDecals`: **D**(히트 지점의 데칼 후보, RayQuery) | 고침. **1F**: 새 프로브는 항상 추적 → 첫 프레임들이 프레임 상한(1024 프로브)에 닿음 — 총량은 청크 수(13)로 묶임 |
| `r.gi.rc.filter / store` | 청크당 (4,4,프로브) 그룹 | 0 | 0 | 고정(이웃 6) | 통과 |
| `r.gi.sao`, `r.gi.sao.temporal` | 화소당 | 0 | 0 | 고정(슬라이스 2 × 스텝 3 × 양방향, 3×3) | 통과 |
| `fx.layer.setup` ML1, `volume.setup`(표본 볼륨 읽기) | 입자당 | 0 | 0 | 볼륨 조회 2회(고정). ML0·옛 경로는 **D**(프록셀 목록 루프 + VSM 보행) | 통과(켬) |

A가 고친 것(다음 코드 커밋): `m.ml.trace` 행 띠, `s.ml.volume` 슬라이스 띠, radiance cache 청크를 "텍셀당 광선 3"으로 계산.

## 2. R의 패스 (`r.gi.lg.*`, 1c1c65a)

| 패스 | dispatch 단위 | 스레드당 광선 | dispatch당 최악 광선 | 스레드당 루프 | 판정 |
|---|---|---|---|---|---|
| `place`, `screendata`, `lightingpdf`, `rays`, `composite`, `probetemporal`, `filter0..2`, `irradiance`, `integrate`, `temporal`, `rcmark` | 프로브·텍셀·화소당 compute | 0 | 0 | 고정(64, LG_GATHER_RES, 8칸 표시) | 통과 |
| `adaptive.mark / spawn` | 타일당 | 0 | 0 | 고정(LG_TILE_ADAPTIVE; 비트 루프 ≤ 32) | 통과 |
| `screentrace` | 추적 텍셀당 compute(아틀라스 전체가 Dispatch 하나: 1080p 783,360 스레드) | 0(깊이 피라미드 보행) | 0 | `gi.lumen_screen_trace_iterations` 50 + 두께 스텝 4 (설정값 상한) | 통과(광선 없음). 참고: 스레드 78만 × 최대 54 스텝이 한 Dispatch |
| `trace` | 추적 텍셀당 스레드, 행 띠 | 3(프로브 광선 + 히트에서 태양·국소광 그림자) | 띠 = `gi.lumen_rays_per_dispatch` 262,144 **스레드** → 최악 광선 786,432 | 히트의 `rtLocalLightChooseOriented` **D**, `rtHitDecals` **D** | **알림**: 띠가 "프로브 광선 수"만 세고 히트의 그림자 광선 2개를 세지 않음. A가 radiance cache에서 같은 식을 고친 것처럼(÷3) 87,381 스레드로 낮추면 기준에 맞음 |

## 3. S2의 패스 (21a3aac)

| 패스 | dispatch 단위 | 스레드당 광선 | dispatch당 최악 광선 | 스레드당 루프 | 판정 |
|---|---|---|---|---|---|
| `r.refl.hzb` | 화소당 compute | 0 | 0 | 고정(레벨 6) | 통과 |
| `r.refl.screentrace` | 띠(kBand 262,144 스레드) 간접 | 0(깊이 피라미드 보행) | 0 | `lumen_screen_trace_max_iterations` 50 + 두께 스텝 | 통과 |
| `r.refl.scenecolor` | 광선 슬롯당 compute | 0 | 0 | 고정 | 통과 |
| `r.refl.trace`(M/G, 옛 경로 포함) | 띠 262,144 **작업(job)** 스레드 | 작업당 `j.rays`(G 표본당 광선 수, 필드 4비트 = 최대 15) — **[loop] 안에서 rtTraceClosest** | 262,144 × j.rays | 고정 상한 15이나 **TraceRay가 루프 안**(표면 캐시와 같은 꼴) | **알림**: 띠가 작업 수만 셈. 광선 수(작업 × j.rays)로 묶을 것. [실측, A의 라운지 격리 실행] 옛 경로 `r.refl.inline.g` 한 패스 최대 30 ms |
| `r.refl.localshadow`, `r.refl.shadow`, `r.refl.inline.*` | 띠(kBand / kInlineBand 65,536) | 1 / 0(인라인 질의) | ≤ 262,144 | 인라인: 후보 루프 | 띠는 있음. 인라인 후보 루프는 **D**(불투명이 아닌 후보 수) |
| `r.sc.begin`, `r.sc.update.cells / probes` | 슬롯당 compute(행 16,384 그룹) | 0 | 0 | `scFind` 탐사 ≤ 4(고정) | 통과 |
| `r.sc.seed` | 캡처 경로당 스레드, 예산 = N/64/(바운스+1) | 바운스+1 = 4, **루프 안 rtTraceClosest** | N = 2²² 일 때 16,384 × 4 = 65,536 | 고정(바운스 3) | 통과(광선 수). TraceRay가 루프 안 |
| `r.sc.cells` | 셀당 스레드, kCellBand 16,384 | 광원 8 + 나머지 1 + 태양 1 = 10 | 163,840 | 격자 칸 광원 루프 2회 **D**(칸의 광원 수 상한 없음). 그림자 광선: `direct_shadow_inline`이면 평가 루프(고정 8)와 인라인 질의 루프(고정 8) 분리, 후보 ≤ 64 — 아니면 옛 꼴([loop] 안 rtVisible, 큰 상태 생존) | 띠·분리는 들어감. **1F**: 아직 조명되지 않은 셀이 먼저 → 첫 프레임들이 예산(N/32 = 131,072 셀 = 띠 8개)을 꽉 채움 |
| `r.sc.probes` | 프로브당 스레드, 예산 N/64/16 = 4,096 | 16, **루프 안 rtTraceClosest** | 65,536 | 고정(16) | 통과(광선 수). TraceRay가 루프 안 |

## 4. 공통으로 남는 "데이터에 따라 길이가 달라지는 루프" (오늘 생긴 것이 아님)

| 루프 | 어디서 | 길이 | 비고 |
|---|---|---|---|
| 프록셀 목록의 광원 | `m.ml.sample`, `s.ml.volume`, (옛) ShadeOpaque·CoverageShade·FroxelIntegrate | 프록셀에 닿는 광원 수(상한 = 장면 광원 수) | S의 목록 구조. 옛 음영 경로가 화소마다 같은 루프를 돌았으므로 MegaLights가 새로 키운 것은 아님. 표본 커널은 다운샘플(화소의 1/4)에서만 돎 |
| 광원 격자 칸의 광원 | `rtLocalLightChooseOriented`(R: gi.trace, lg.trace, A: rc.trace, S2: refl 히트), `mlWorldSamples`(A), `r.sc.cells`(S2) | 칸에 범위가 닿는 광원 수(상한 없음, RayScene.cpp 1985–2002행) | R의 격자. 칸당 광원 수에 구조 상한을 두는 것이 근본 해결(예: 칸을 더 나누거나 칸 목록을 중요도 머리 + 확률 꼬리로) — R 소유 |
| 히트의 데칼 후보 | `rtHitDecals` (RayQuery) | 히트 지점을 덮는 데칼 수 | R 소유 |
| 인라인 질의의 비불투명 후보 | S2 `r.refl.inline.*`, `scVisibleInline` | 광선이 지나는 알파 테스트 삼각형 수(표면 캐시는 64에서 끊음) | S2 소유 |

## 5. 조치

- A(다음 코드 커밋, 첫 실행 대기): `m.ml.trace` 행 띠, `s.ml.volume` 슬라이스 띠, radiance cache 청크 ÷3.
- R에게 알림: `r.gi.lg.trace` 띠를 "스레드당 광선 3"으로(262,144 ÷ 3), 광원 격자 칸의 광원 수 상한. → R 답(10-02): 띠를 광선 기준(÷3, 최대 87,381 스레드)으로 고침 — 다음 커밋. 격자 칸 목록의 길이 상한과 `rtHitDecals` 후보 루프는 R의 남은 일로 받음(판정 목록 뒤 RayScene에서 한 번에).
- S2에게 알림: `r.refl.trace` 띠를 광선 수(작업 × j.rays)로, 루프 안 TraceRay(반사 M/G, `r.sc.seed`, `r.sc.probes`)를 표면 캐시 직접광에서 한 것처럼 "평가 → 광선" 분리 또는 인라인으로, `direct_shadow_inline`이 꺼진 경로를 남겨 두지 말 것.
- 이 표는 라운지 hang의 원인을 확정하지 않는다. 기준을 넘는 dispatch를 없앤 것일 뿐이다.

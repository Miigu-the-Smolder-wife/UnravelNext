# 요청: 입자 렌더 입력과 보간 규칙 (FX 트랙, 2026-09-25, 개정 2026-09-26)

WORLD_VFX_DESIGN_KO.md 8절 3("FX 패스의 셀 키 함수 정의, 슬롯 단위 보간 규칙")의 요청이다(슬롯은 2026-09-26에 행 레이아웃으로 바뀌었다). 시뮬레이션(C0)이 만드는 것과
렌더러 FX 패스가 읽는 규칙을 고정한다. 렌더 패스 자체(설계서 2.9: 프록셀 합류, 1/4 층 + 가장자리, 알파 블렌드 순서)의 비용식과
품질 정의는 아직 없으므로 **렌더 패스 구현 전에 조율 세션(Fable 설계)에 넘긴다.** 이 파일은 시뮬레이션 쪽이 지금 정해야 하는
부분만 제안한다.

## 1. 렌더 입력 = 상태 쌍 + 행 레이아웃 (개정 2026-09-26: 슬롯·압축 없음)

시뮬레이션 상태가 tick 패리티로 두 벌이다(`posAge[2]`, `velocity[2]`). integrate가 지난 tick 출력을 읽어 이번 tick 출력을 다른 벌에
쓴다. 각 벌은 그 tick의 **입자 레이아웃**으로 놓인다. 살아 있는 이미터 행마다 행의 산 출생 [death_birth, next_birth)가 출생 순서대로
[base, base + count)에 연속으로 놓인다. 스트림 계약상 한 행의 산 출생은 정확히 이 구간이다(NativeVfxStream.h). 입자의 색인은
base + (birth − first)다. 볼륨 행이 첫 셀 순서로 앞에 오므로 볼륨 입자의 레코드 색인(3b)이 곧 상태 색인이다. 레이아웃은 CPU가 스트림
표에서 계산한다(tick마다 행 수에 비례, 입자 수와 무관). 슬롯, 생존 플래그, meta, 생존/죽음 목록, 압축은 없다.

```text
posAge[cur][i]   float3 position (이미터 원점 기준, m), float age (tick 끝, s)   i < 이번 레이아웃의 합계
velocity[cur][i] float3 velocity (m/s), 0
posAge[prev][j], velocity[prev][j]   지난 tick 끝, 지난 tick 레이아웃의 색인 j
행 표(tick마다 업로드, 32 B): { row, base, first, count, prevBase, prevFirst, prevCount, 0 }
    이번 tick에 산 행 + 지난 tick에만 있던 행(count 0: 모두 이번 tick에 죽음). 이번 tick base 순서.
```

- 입자 i의 행은 행 표의 base로 찾는다(블록 → 행 색인 표, 탐색 1~2단계). birth = first + (i − base).
- 지난 tick의 같은 입자: rel = birth − prevFirst < prevCount이면 prev 색인 = prevBase + rel. 아니면 tick n에 태어난 입자다.
- 설계서 3.1의 레코드(위치·크기·색·이미터·정렬 키)에서 **크기·색을 빼고 속도·나이를 넣는다.** 크기·색·알파·회전·uv·flipbook은 모두
  (프로그램, 이미터 매개변수, 나이, 수명)의 순수 함수다. 렌더 프레임 시각의 나이로 렌더러가 평가하면 tick 값의 선형 보간보다
  정확하다(곡선의 꺾임이 tick 사이에 있어도 그대로 나온다). 매개변수는 그 tick의 이미터 표에서 읽는다("다음 tick부터 적용" 계약 유지).

## 2. 입자 보간 규칙

렌더 프레임의 시각을 tick n−1 끝(w = 0)과 tick n 끝(w = 1) 사이의 w로 쓴다. Δt = tick 간격.

| 경우 | 판별 | 표시 조건 | 위치 (이미터 원점 기준, 그 tick의 원점을 더해 anchor 기준으로) | 나이 |
|---|---|---|---|---|
| 계속 산 입자 | 이번 레이아웃, birth − prevFirst < prevCount | 항상 | 두 끝의 위치·속도로 3차 Hermite (C1) | age_n − (1 − w)Δt |
| tick n에 태어남 | 이번 레이아웃, 지난 구간 밖 | age_n − (1 − w)Δt ≥ 0 | p_n − v_n (1 − w)Δt | 같음 |
| tick n에 죽음 | 지난 구간의 [prevFirst, first) (count 0 행은 지난 구간 전체) | w Δt < t_d, t_d = 수명 − age_{n−1} | p_{n−1} + v_{n−1} w Δt | age_{n−1} + w Δt |

- **identity는 (행, 출생 번호)로 정확하다.** 두 tick의 같은 입자를 행 표로 짝짓는다(슬롯 재사용 규칙이 필요 없다).
- 죽은 입자는 행의 앞쪽 연속 구간이다(FIFO: 한 행의 수명이 같다). KILLED 행은 이번 tick 구간이 없고 지난 구간이 모두 사라진다.
  KILLED 행은 표시하지 않는다(사망 사건 없음과 같은 규칙). 렌더러는 행 표의 플래그로 구별한다.
- 태어난·죽은 tick의 선형 외삽 오차는 |a| Δt² / 2 이하다(중력 9.8 m/s², Δt = 16.7 ms: 1.4 mm) [계산]. v1은 선형이다.
- 원점 이동(rebase)·anchor 이동: 각 tick의 이미터 표·anchor로 그 tick의 위치를 anchor 기준으로 바꾼 뒤 보간한다.

## 3. 깊이 순서: 렌더 패스의 프레임별 타일 지역 정렬 (설계 개정 14.8 판정 1, 2026-09-26 채택)

- tick에는 정렬이 없다(FX 416fd12). 렌더 FX 패스가 프레임마다 입자를 화면 타일로 빈닝하고 타일 목록을 groupshared에서 그
  프레임 카메라의 뷰 깊이(입자 중심)로 정렬한다. tick 정렬(C0 카메라)은 다음 ≤ 2.75 프레임 동안 낡는 품질 결함이었고, 프레임별
  정렬은 프레임마다 정확하다. 비용은 렌더 패스 비용식에 들어간다(설계: 프레임당 약 0.01~0.02 ms, tick −0.15 ms).
- 렌더 입력: 1의 상태 쌍 + 행 표(두 tick의 레이아웃). 정렬된 목록은 없다. 빈닝 스레드 수 = 이번 합계 + 이번 tick에 죽은 입자 수(CPU가 안다).

## 3b. 국소 볼륨: 셀 대신 입자 레코드 (설계 개정 14.8 판정 2, S 동의, 2026-09-26)

셀(`NV_MediumCell` 96 B × n³)은 입자 레코드와 프로그램 상수의 순수 함수인 캐시이고, tick 셀은 프레임 사이 입자 이동에 낡는다. 프록셀
패스가 입자에서 직접 tent-mass를 평가한다. 계약:

1. 타일 목록은 프록셀 타일(`atmosphere.froxels.tile_px`, 뷰마다 격자)과 1:1이다. 렌더 빈닝 타일이 다르면 FX가 프록셀 격자로 다시
   빈닝한다. 목록 항목은 입자의 view-depth 범위 [z0, z1]을 담는다(슬라이스 번호는 S가 airNodeCoord로 구한다).
2. 레코드(SoA, tick마다 FX가 쓴다; 색인 = 행의 입자 기준 + (birth − death_birth), 결정적):
   - `record16` = { float3 중심(anchor 공간), uint half2(r, m) } 16 B — 슬라이스마다 읽는 부분.
     밀도장 ρ(x) = m · Π_i max(0, 1 − |x_i − c_i| / r) / r³ (∫ρ dV = m), r = size(u)/2, m = alpha(u), u = age/lifetime.
   - `side8` = { uint 발광 계수 = medium_emission × colour(u) (R11G11B10F, HDR, 포화 없음, 발광 없는 종류는 0), uint 종류(프로그램
     색인) } 8 B — 항목당 한 번.
   - 종류 표(프로그램당): medium_absorption.rgb, medium_scattering.rgb, medium_phase(g).
   - 매질: σa = medium_absorption × ρ, σs = medium_scattering × ρ(RGB, 종류별 → 소광은 종류별로 색이 있다), 발광 = side8 × ρ. 스트림의
     셀 식 그대로다: colour(u)(색 곡선 × 이미터 colorScale)는 **발광에만** 곱해지므로 입자별 값은 발광 계수뿐이다(side12 불필요).
   - 매끄러운 발광 매질(빛나는 연기, 안개 속 광원)은 이 경로로 프록셀에 들어간다. 경계가 뚜렷한 발광(불꽃)은 1/4 층 입자 렌더링(2.9).
   - 좌표: anchor 공간 = 월드 − 스트림 anchor(NV_StreamHeader.anchor, double3), 순수 평행이동(회전·배율·전단 없음) → tent의 축은 월드
     축이다. FX 빈닝 패스(프레임·뷰마다)가 record16을 한 번 읽어 카메라 상대 사본(centre − (camera − anchor), 오프셋은 CPU double로
     만든 스트림·뷰당 float3)을 쓰고, 타일 목록 항목 = { uint 레코드 색인, half z0, z1 } 8 B를 쓴다. S는 슬라이스마다 카메라 상대
     record16 16 B, 항목마다 목록 항목 8 B와 side8을 읽는다.
3. tent-mass 정의(질량이 반지름 r의 tent 커널로 퍼진 밀도)를 공식으로 고정하고, S는 광선 구간의 해석 적분(구간당 닫힌 식)을 쓴다.
4. 입자 매질은 공기 그림자(VSM 걷기)에 들어가지 않는다. 입자끼리의 소광은 프록셀 투과율로 들어간다.

tick 쪽: cells 패스를 없애고 볼륨 입자마다 record16 + side8을 쓴다(RPP: 32k × 24 B, 셀 262k × 96 B 대신).

## 4. 요청

- 코어: 1~3을 INTERFACES에 FX 계약으로 기록(`FrameResources::particles`와 같이, `20260925_FX_simulation_slot.md`).
- 조율 세션: 렌더 패스(2.9) 비용식·품질 정의 작성 시 2의 외삽 허용을 함께 정해 달라(3·3b는 14.8에서 판정됨).

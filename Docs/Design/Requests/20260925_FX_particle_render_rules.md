# 요청: 입자 셀 키와 슬롯 보간 규칙 (FX 트랙, 2026-09-25)

WORLD_VFX_DESIGN_KO.md 8절 3("FX 패스의 셀 키 함수 정의, 슬롯 단위 보간 규칙")의 요청이다. 시뮬레이션(C0)이 만드는 것과
렌더러 FX 패스가 읽는 규칙을 고정한다. 렌더 패스 자체(설계서 2.9: 프록셀 합류, 1/4 층 + 가장자리, 알파 블렌드 순서)의 비용식과
품질 정의는 아직 없으므로 **렌더 패스 구현 전에 조율 세션(Fable 설계)에 넘긴다.** 이 파일은 시뮬레이션 쪽이 지금 정해야 하는
부분만 제안한다.

## 1. 렌더 입력 = 상태 쌍 (개정 2026-09-25 저녁: 별도 32 B 레코드 없음)

시뮬레이션 상태가 tick 패리티로 두 벌이다(`posAge[2]`, `velocity[2]`: integrate가 지난 tick 출력을 읽어 이번 tick 출력을 다른 벌에
쓴다). 렌더러는 두 벌을 그대로 읽는다. 복사 레코드(슬롯당 32 B 쓰기, 96 B 간격의 부분 섹터 쓰기)는 없앴다 [실측: integrate 쓰기 32 B/슬롯 감소].

```text
posAge[cur][s]   float3 position (이미터 원점 기준, m), float age (tick 끝, s)
velocity[cur][s] float3 velocity (m/s), 0
posAge[prev][s], velocity[prev][s]   지난 tick 끝 (repack 때 슬롯과 같이 옮겨짐)
meta[s]          uint emitter, uint birth
```

- 새로 태어난 슬롯은 prev 쪽에 스폰 레코드(나이 부호 비트 = −elapsed)가 들어 있으므로 "tick n에 태어남"은 prev 나이의 부호로도 판별된다.
- 설계서 3.1의 레코드(위치·크기·색·이미터·정렬 키)에서 **크기·색을 빼고 속도·나이를 넣는다.** 크기·색·알파·회전·uv·flipbook은 모두
  (프로그램, 이미터 매개변수, 나이, 수명)의 순수 함수다. 렌더 프레임 시각의 나이로 렌더러가 평가하면 tick 값의 선형 보간보다
  정확하다(곡선의 꺾임이 tick 사이에 있어도 그대로 나온다). 매개변수는 그 tick의 이미터 표에서 읽는다("다음 tick부터 적용" 계약 유지).
- 정렬 키는 레코드 밖(키·값 버퍼)에 있다.

## 2. 슬롯 보간 규칙

렌더 프레임의 시각을 tick n−1 끝(w = 0)과 tick n 끝(w = 1) 사이의 w로 쓴다. Δt = tick 간격.

| 경우 | 판별 | 표시 조건 | 위치 (이미터 원점 기준, 그 tick의 원점을 더해 anchor 기준으로) | 나이 |
|---|---|---|---|---|
| 계속 산 입자 | tick n 생존 목록, age_n ≥ Δt | 항상 | 두 끝의 위치·속도로 3차 Hermite (C1) | age_n − (1 − w)Δt |
| tick n에 태어남 | 생존 목록, age_n < Δt | age_n − (1 − w)Δt ≥ 0 | p_n − v_n (1 − w)Δt | 같음 |
| tick n에 죽음 | 죽음 목록(dying) | w Δt < t_d, t_d = 수명 − age_{n−1} | p_{n−1} + v_{n−1} w Δt | age_{n−1} + w Δt |

- **identity는 생존 플래그만으로 보장된다.** 슬롯은 죽은 뒤 압축에서 dead 목록에 들어간 다음 tick에만 재사용되므로, tick n−1과 n에
  모두 생존한 슬롯은 같은 입자다. 따라서 레코드에 출생 번호가 필요 없다.
- 태어난·죽은 tick의 선형 외삽 오차는 |a| Δt² / 2 이하다(중력 9.8 m/s², Δt = 16.7 ms: 1.4 mm) [계산]. 필요하면 가속도 항을
  레코드 예비 비트 없이 프로그램·필드에서 다시 평가할 수 있지만 v1은 선형이다.
- 원점 이동(rebase)·anchor 이동: 각 tick의 이미터 표·anchor로 그 tick의 위치를 anchor 기준으로 바꾼 뒤 보간한다.

## 3. 깊이 순서: 렌더 패스의 프레임별 타일 지역 정렬 (설계 개정 14.8 판정 1, 2026-09-26 채택)

- tick에는 정렬이 없다(FX 416fd12). 렌더 FX 패스가 프레임마다 입자를 화면 타일로 빈닝하고 타일 목록을 groupshared에서 그
  프레임 카메라의 뷰 깊이(입자 중심)로 정렬한다. tick 정렬(C0 카메라)은 다음 ≤ 2.75 프레임 동안 낡는 품질 결함이었고, 프레임별
  정렬은 프레임마다 정확하다. 비용은 렌더 패스 비용식에 들어간다(설계: 프레임당 약 0.01~0.02 ms, tick −0.15 ms).
- 렌더 입력: 1의 상태 쌍 + `aliveList`/`counters[alive]`(슬롯 순서) + `meta`. 정렬된 목록은 없다.

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
